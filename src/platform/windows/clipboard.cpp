/**
 * @file src/platform/windows/clipboard.cpp
 * @brief Windows clipboard sync for a Sunshine session.
 *
 * A watcher thread owns a message-only window that listens for clipboard changes
 * with AddClipboardFormatListener, plus a slow sequence-number poll as a fallback.
 * Host copies are read as UTF-8 text and one PNG image and offered to every
 * subscribed client. A client clipboard is written as CF_UNICODETEXT, the
 * registered "PNG" format, and a CF_DIB so every application can paste it.
 */
// platform includes
#include <Windows.h>
#include <objbase.h>
#include <shlwapi.h>
#include <wincodec.h>

// standard includes
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

// local includes
#include "src/logging.h"
#include "src/platform/clipboard_sync.h"
#include "src/platform/common.h"
#include "src/utility.h"
#include "utf_utils.h"

namespace platf {
  using namespace std::literals;

  namespace {
    /**
     * @brief Posted to the watcher window to apply a client clipboard or answer a new subscriber.
     */
    constexpr UINT WM_CLIPBOARD_WAKE = WM_APP + 1;

    /**
     * @brief Timer used to poll the clipboard sequence number in case a change notification is missed.
     */
    constexpr UINT_PTR poll_timer_id = 1;

    /**
     * @brief Interval of the fallback poll.
     */
    constexpr UINT poll_interval_ms = 500;

    /**
     * @brief Largest uncompressed bitmap read from the host clipboard before PNG encoding.
     */
    constexpr std::size_t max_dib_bytes = 512 * 1024 * 1024;

    /**
     * @brief CLSID of the WIC imaging factory that is available on every supported Windows version.
     */
    constexpr GUID wic_factory_clsid = {0xcacaf262, 0x9370, 0x4615, {0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a}};

    /**
     * @brief WIC container format for PNG.
     */
    constexpr GUID wic_png_container = {0x1b7cfaf4, 0x713f, 0x473c, {0xbb, 0xcd, 0x61, 0x37, 0x42, 0x5f, 0xae, 0xaf}};

    /**
     * @brief WIC pixel format for 32-bit BGRA, which maps directly onto a 32 bpp DIB.
     */
    constexpr GUID wic_bgra32_format = {0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0f}};

    /**
     * @brief Owning pointer for a COM interface.
     */
    template<class T>
    class com_ptr_t {
    public:
      com_ptr_t() = default;
      com_ptr_t(const com_ptr_t &) = delete;
      com_ptr_t &operator=(const com_ptr_t &) = delete;

      ~com_ptr_t() {
        if (ptr_ != nullptr) {
          ptr_->Release();
        }
      }

      /**
       * @brief Address to receive a new interface pointer.
       *
       * @return Pointer to the stored interface pointer.
       */
      T **put() {
        return &ptr_;
      }

      /**
       * @brief Stored interface pointer.
       *
       * @return The interface, or null.
       */
      T *get() const {
        return ptr_;
      }

      T *operator->() const {
        return ptr_;
      }

    private:
      T *ptr_ = nullptr;  ///< Owned interface pointer.
    };

    /**
     * @brief Process-wide Windows clipboard watcher state.
     */
    struct state_t {
      std::mutex mutex;  ///< Guards the fields below for the input and watcher threads.
      std::condition_variable ready_cv;  ///< Wakes subscribers waiting for the watcher window.
      bool ready = false;  ///< Whether the watcher can accept clipboard operations.
      bool failed = false;  ///< Whether creating the watcher window failed.
      bool started = false;  ///< Whether the watcher thread has been launched.
      bool pull = false;  ///< Whether a new subscriber still needs the current host clipboard.
      HWND window = nullptr;  ///< Message-only window owned by the watcher thread.
      std::shared_ptr<const clipboard_data_t> pending;  ///< Client clipboard waiting to be written to the host clipboard.
      clipboard_subscribers_t subscribers;  ///< One entry for each streaming client that opted in.
      std::jthread thread;  ///< Watcher thread.
    };

    /**
     * @brief Return the process-wide clipboard watcher state.
     *
     * @return Watcher state shared by subscribe, unsubscribe, and the watcher thread.
     */
    state_t &state() {
      static state_t value;
      return value;
    }

    /**
     * @brief Wake the watcher thread so it handles new state.
     *
     * @param clipboard Watcher state. The caller holds its lock.
     */
    void wake_watcher(const state_t &clipboard) {
      if (clipboard.window != nullptr && !PostMessageW(clipboard.window, WM_CLIPBOARD_WAKE, 0, 0)) {
        BOOST_LOG(debug) << "Clipboard watcher wakeup was not queued"sv;
      }
    }

    /**
     * @brief Queue a clipboard offer for each selected subscriber.
     *
     * @param delivery Subscribers and sequence chosen by clipboard_subscribers_t::publish().
     * @param data Clipboard to offer.
     */
    void raise_offers(const clipboard_delivery_t &delivery, const std::shared_ptr<const clipboard_data_t> &data) {
      for (const auto &queue : delivery.queues) {
        queue->raise(clipboard_message_t {clipboard_action_e::offer, delivery.sequence, {}, data});
      }
    }

    /**
     * @brief Offer a host clipboard to every subscriber that does not already have it.
     *
     * @param data Host clipboard.
     */
    void publish(const std::shared_ptr<const clipboard_data_t> &data) {
      clipboard_delivery_t delivery;
      {
        auto &clipboard = state();
        std::lock_guard lock {clipboard.mutex};
        delivery = clipboard.subscribers.publish(data);
      }
      raise_offers(delivery, data);
    }

    /**
     * @brief Open the clipboard for the lifetime of the object.
     *
     * Another application may hold the clipboard briefly, so opening is retried.
     */
    class clipboard_lock_t {
    public:
      /**
       * @brief Try to open the clipboard.
       *
       * @param owner Window that will own the clipboard if it is emptied.
       */
      explicit clipboard_lock_t(HWND owner) {
        for (int attempt = 0; attempt < 10; ++attempt) {
          if (OpenClipboard(owner)) {
            open_ = true;
            return;
          }
          Sleep(20);
        }
      }

      clipboard_lock_t(const clipboard_lock_t &) = delete;
      clipboard_lock_t &operator=(const clipboard_lock_t &) = delete;

      ~clipboard_lock_t() {
        if (open_) {
          CloseClipboard();
        }
      }

      /**
       * @brief Report whether the clipboard was opened.
       *
       * @return True when the clipboard is open.
       */
      bool is_open() const {
        return open_;
      }

    private:
      bool open_ = false;  ///< Whether OpenClipboard succeeded.
    };

    /**
     * @brief Copy one global-memory clipboard format.
     *
     * @param format Clipboard format. The clipboard must be open.
     * @param limit Largest size to accept.
     * @return The format's bytes, or empty when it is absent, unreadable, or too large.
     */
    std::optional<std::vector<std::uint8_t>> read_global(UINT format, std::size_t limit) {
      HANDLE handle = GetClipboardData(format);
      if (handle == nullptr) {
        return std::nullopt;
      }
      const SIZE_T size = GlobalSize(handle);
      if (size == 0 || size > limit) {
        return std::nullopt;
      }
      const auto *bytes = static_cast<const std::uint8_t *>(GlobalLock(handle));
      if (bytes == nullptr) {
        return std::nullopt;
      }
      std::vector<std::uint8_t> copy(bytes, bytes + size);
      GlobalUnlock(handle);
      return copy;
    }

    /**
     * @brief Read the host clipboard text.
     *
     * @return UTF-8 text with LF line endings, or empty when there is no text.
     */
    std::string read_text() {
      HANDLE handle = GetClipboardData(CF_UNICODETEXT);
      if (handle == nullptr) {
        return {};
      }
      const auto *chars = static_cast<const wchar_t *>(GlobalLock(handle));
      if (chars == nullptr) {
        return {};
      }
      const std::size_t length = wcsnlen(chars, GlobalSize(handle) / sizeof(wchar_t));
      std::wstring wide(chars, length);
      GlobalUnlock(handle);

      auto text = clipboard_crlf_to_lf(utf_utils::to_utf8(wide));
      if (text.size() > clipboard_max_bytes) {
        BOOST_LOG(warning) << "Host clipboard text is too large to sync"sv;
        return {};
      }
      return text;
    }

    /**
     * @brief Create a WIC imaging factory.
     *
     * @param factory Receives the factory.
     * @return True on success.
     */
    bool create_wic_factory(com_ptr_t<IWICImagingFactory> &factory) {
      const auto status = CoCreateInstance(wic_factory_clsid, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory), reinterpret_cast<void **>(factory.put()));
      if (FAILED(status)) {
        BOOST_LOG(warning) << "Clipboard sync could not create a WIC factory [0x"sv << util::hex(status).to_string_view() << ']';
        return false;
      }
      return true;
    }

    /**
     * @brief Copy the contents of a memory stream.
     *
     * @param stream Stream to read from its start.
     * @return The stream's bytes, or empty on failure.
     */
    std::vector<std::uint8_t> read_stream(IStream *stream) {
      STATSTG stat {};
      if (FAILED(stream->Stat(&stat, STATFLAG_NONAME)) || stat.cbSize.QuadPart > clipboard_max_bytes) {
        return {};
      }
      std::vector<std::uint8_t> bytes(static_cast<std::size_t>(stat.cbSize.QuadPart));
      LARGE_INTEGER zero {};
      ULONG read = 0;
      if (FAILED(stream->Seek(zero, STREAM_SEEK_SET, nullptr)) ||
          FAILED(stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &read)) ||
          read != bytes.size()) {
        return {};
      }
      return bytes;
    }

    /**
     * @brief Encode a CF_DIB as PNG.
     *
     * The DIB is wrapped in a BMP file header and decoded with the WIC BMP codec, which
     * handles every bit depth, palettes, and BI_BITFIELDS.
     *
     * @param dib Packed DIB from the clipboard.
     * @return PNG bytes, or empty on failure.
     */
    std::vector<std::uint8_t> dib_to_png(const std::vector<std::uint8_t> &dib) {
      if (dib.size() < sizeof(BITMAPINFOHEADER)) {
        return {};
      }
      BITMAPINFOHEADER header;
      std::memcpy(&header, dib.data(), sizeof(header));
      if (header.biSize < sizeof(BITMAPINFOHEADER) || header.biSize > dib.size()) {
        return {};
      }

      // Pixels follow the header, the three BI_BITFIELDS masks of a plain
      // BITMAPINFOHEADER, and the color table.
      std::size_t pixel_offset = header.biSize;
      if (header.biSize == sizeof(BITMAPINFOHEADER) && header.biCompression == BI_BITFIELDS) {
        pixel_offset += 3 * sizeof(DWORD);
      }
      std::size_t colors = header.biClrUsed;
      if (colors == 0 && header.biBitCount <= 8) {
        colors = std::size_t {1} << header.biBitCount;
      }
      pixel_offset += colors * sizeof(RGBQUAD);
      if (pixel_offset > dib.size()) {
        return {};
      }

      BITMAPFILEHEADER file {};
      file.bfType = 0x4D42;  // "BM"
      file.bfSize = static_cast<DWORD>(sizeof(file) + dib.size());
      file.bfOffBits = static_cast<DWORD>(sizeof(file) + pixel_offset);

      std::vector<std::uint8_t> bmp(sizeof(file) + dib.size());
      std::memcpy(bmp.data(), &file, sizeof(file));
      std::memcpy(bmp.data() + sizeof(file), dib.data(), dib.size());

      com_ptr_t<IWICImagingFactory> factory;
      if (!create_wic_factory(factory)) {
        return {};
      }

      com_ptr_t<IStream> input;
      *input.put() = SHCreateMemStream(bmp.data(), static_cast<UINT>(bmp.size()));
      com_ptr_t<IWICBitmapDecoder> decoder;
      com_ptr_t<IWICBitmapFrameDecode> frame;
      if (input.get() == nullptr ||
          FAILED(factory->CreateDecoderFromStream(input.get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.put())) ||
          FAILED(decoder->GetFrame(0, frame.put()))) {
        BOOST_LOG(debug) << "Host clipboard bitmap could not be decoded"sv;
        return {};
      }

      com_ptr_t<IStream> output;
      *output.put() = SHCreateMemStream(nullptr, 0);
      com_ptr_t<IWICBitmapEncoder> encoder;
      com_ptr_t<IWICBitmapFrameEncode> target;
      if (output.get() == nullptr ||
          FAILED(factory->CreateEncoder(wic_png_container, nullptr, encoder.put())) ||
          FAILED(encoder->Initialize(output.get(), WICBitmapEncoderNoCache)) ||
          FAILED(encoder->CreateNewFrame(target.put(), nullptr)) ||
          FAILED(target->Initialize(nullptr)) ||
          FAILED(target->WriteSource(frame.get(), nullptr)) ||
          FAILED(target->Commit()) ||
          FAILED(encoder->Commit())) {
        BOOST_LOG(debug) << "Host clipboard bitmap could not be encoded as PNG"sv;
        return {};
      }
      return read_stream(output.get());
    }

    /**
     * @brief Decode a PNG or JPEG into a packed 32 bpp CF_DIB.
     *
     * @param image Encoded image bytes.
     * @return Packed DIB, or empty on failure.
     */
    std::vector<std::uint8_t> image_to_dib(const std::vector<std::uint8_t> &image) {
      com_ptr_t<IWICImagingFactory> factory;
      if (image.empty() || !create_wic_factory(factory)) {
        return {};
      }

      com_ptr_t<IStream> input;
      *input.put() = SHCreateMemStream(image.data(), static_cast<UINT>(image.size()));
      com_ptr_t<IWICBitmapDecoder> decoder;
      com_ptr_t<IWICBitmapFrameDecode> frame;
      com_ptr_t<IWICFormatConverter> converter;
      if (input.get() == nullptr ||
          FAILED(factory->CreateDecoderFromStream(input.get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.put())) ||
          FAILED(decoder->GetFrame(0, frame.put())) ||
          FAILED(factory->CreateFormatConverter(converter.put())) ||
          FAILED(converter->Initialize(frame.get(), wic_bgra32_format, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        BOOST_LOG(debug) << "Client clipboard image could not be decoded"sv;
        return {};
      }

      UINT width = 0;
      UINT height = 0;
      if (FAILED(converter->GetSize(&width, &height)) || width == 0 || height == 0) {
        return {};
      }
      const std::size_t stride = std::size_t {width} * 4;
      const std::size_t pixels = stride * height;
      if (pixels > max_dib_bytes) {
        return {};
      }

      BITMAPINFOHEADER header {};
      header.biSize = sizeof(header);
      header.biWidth = static_cast<LONG>(width);
      header.biHeight = -static_cast<LONG>(height);  // Top-down rows, matching WIC's row order.
      header.biPlanes = 1;
      header.biBitCount = 32;
      header.biCompression = BI_RGB;
      header.biSizeImage = static_cast<DWORD>(pixels);

      std::vector<std::uint8_t> dib(sizeof(header) + pixels);
      std::memcpy(dib.data(), &header, sizeof(header));
      if (FAILED(converter->CopyPixels(nullptr, static_cast<UINT>(stride), static_cast<UINT>(pixels), dib.data() + sizeof(header)))) {
        return {};
      }
      return dib;
    }

    /**
     * @brief Read the host clipboard as text and one PNG image.
     *
     * @param window Watcher window used to open the clipboard.
     * @return The clipboard, or empty when the clipboard could not be opened.
     */
    std::optional<clipboard_data_t> read_clipboard(HWND window) {
      static const UINT png_format = RegisterClipboardFormatW(L"PNG");

      clipboard_data_t data;
      std::optional<std::vector<std::uint8_t>> dib;
      {
        clipboard_lock_t lock {window};
        if (!lock.is_open()) {
          return std::nullopt;
        }

        data.text = read_text();

        // Browsers and Office put a PNG on the clipboard. Prefer it to the DIB,
        // which loses transparency on most sources.
        if (png_format != 0) {
          if (auto png = read_global(png_format, clipboard_max_bytes)) {
            data.image_mime = SS_CLIPBOARD_MIME_PNG;
            data.image = std::move(*png);
          }
        }
        if (data.image.empty() && IsClipboardFormatAvailable(CF_DIB)) {
          dib = read_global(CF_DIB, max_dib_bytes);
        }
      }

      // Encode outside the clipboard lock so other applications are not blocked.
      if (dib) {
        auto png = dib_to_png(*dib);
        if (!png.empty()) {
          data.image_mime = SS_CLIPBOARD_MIME_PNG;
          data.image = std::move(png);
        }
      }
      return data;
    }

    /**
     * @brief Place a global-memory copy of some bytes on the open clipboard.
     *
     * @param format Clipboard format.
     * @param bytes Bytes to copy.
     * @return True when the clipboard took ownership of the memory.
     */
    bool set_global(UINT format, std::span<const std::uint8_t> bytes) {
      HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
      if (memory == nullptr) {
        return false;
      }
      void *target = GlobalLock(memory);
      if (target == nullptr) {
        GlobalFree(memory);
        return false;
      }
      std::memcpy(target, bytes.data(), bytes.size());
      GlobalUnlock(memory);
      if (SetClipboardData(format, memory) == nullptr) {
        GlobalFree(memory);
        return false;
      }
      return true;
    }

    /**
     * @brief Replace the host clipboard with a client clipboard.
     *
     * @param window Watcher window that becomes the clipboard owner.
     * @param data Client clipboard.
     * @return Clipboard sequence number after the write, or zero when nothing was written.
     */
    DWORD write_clipboard(HWND window, const clipboard_data_t &data) {
      static const UINT png_format = RegisterClipboardFormatW(L"PNG");

      // Convert before opening the clipboard so other applications are not blocked.
      std::wstring wide;
      if (!data.text.empty()) {
        wide = utf_utils::from_utf8(clipboard_lf_to_crlf(data.text));
      }
      std::vector<std::uint8_t> dib;
      if (!data.image.empty()) {
        dib = image_to_dib(data.image);
      }

      clipboard_lock_t lock {window};
      if (!lock.is_open()) {
        BOOST_LOG(warning) << "Clipboard sync could not open the clipboard to apply a client copy"sv;
        return 0;
      }
      if (!EmptyClipboard()) {
        return 0;
      }

      if (!wide.empty()) {
        const auto *chars = reinterpret_cast<const std::uint8_t *>(wide.c_str());
        set_global(CF_UNICODETEXT, {chars, (wide.size() + 1) * sizeof(wchar_t)});
      }
      if (data.image_mime == SS_CLIPBOARD_MIME_PNG && png_format != 0) {
        set_global(png_format, data.image);
      }
      if (!dib.empty()) {
        set_global(CF_DIB, dib);
      }
      return GetClipboardSequenceNumber();
    }

    /**
     * @brief Clipboard state owned by the watcher thread.
     */
    struct watcher_t {
      HWND window = nullptr;  ///< Message-only window that owns the clipboard listener.
      DWORD seen_sequence = 0;  ///< Last clipboard sequence number that was handled.
      DWORD own_sequence = 0;  ///< Sequence number produced by the last client clipboard write.

      /**
       * @brief Read and offer the host clipboard if it changed since the last check.
       *
       * @param force Read even when the sequence number did not change, for a new subscriber.
       */
      void check(bool force) {
        const DWORD sequence = GetClipboardSequenceNumber();
        if (!force && sequence == seen_sequence) {
          return;
        }
        if (!force && sequence == own_sequence) {
          // This is the client clipboard Sunshine just wrote. Its subscribers
          // were already offered it by clipboard_set().
          seen_sequence = sequence;
          return;
        }

        auto data = read_clipboard(window);
        if (!data) {
          // Another application holds the clipboard. The poll timer retries.
          return;
        }
        seen_sequence = sequence;
        if (data->text.empty() && data->image.empty()) {
          return;
        }
        publish(std::make_shared<const clipboard_data_t>(std::move(*data)));
      }

      /**
       * @brief Apply a waiting client clipboard and answer a new subscriber.
       */
      void wake() {
        std::shared_ptr<const clipboard_data_t> pending;
        bool pull = false;
        {
          auto &clipboard = state();
          std::lock_guard lock {clipboard.mutex};
          pending = std::move(clipboard.pending);
          clipboard.pending.reset();
          pull = clipboard.pull;
          clipboard.pull = false;
        }

        if (pending) {
          if (const DWORD sequence = write_clipboard(window, *pending); sequence != 0) {
            own_sequence = sequence;
            seen_sequence = sequence;
          }
        }
        if (pull) {
          check(true);
        }
      }
    };

    /**
     * @brief Window procedure of the watcher window.
     *
     * @param window Watcher window.
     * @param message Window message.
     * @param wparam Message parameter.
     * @param lparam Message parameter.
     * @return Message result.
     */
    LRESULT CALLBACK watcher_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
      auto *watcher = reinterpret_cast<watcher_t *>(GetWindowLongPtrW(window, GWLP_USERDATA));
      if (watcher != nullptr) {
        switch (message) {
          case WM_CLIPBOARDUPDATE:
          case WM_TIMER:
            watcher->check(false);
            return 0;
          case WM_CLIPBOARD_WAKE:
            watcher->wake();
            return 0;
          default:
            break;
        }
      }
      return DefWindowProcW(window, message, wparam, lparam);
    }

    /**
     * @brief Mark the watcher failed so subscribers stop waiting.
     */
    void fail_watcher() {
      auto &clipboard = state();
      std::lock_guard lock {clipboard.mutex};
      clipboard.failed = true;
      clipboard.ready_cv.notify_all();
    }

    /**
     * @brief Watch the Windows clipboard until the process exits.
     *
     * @param stop Stop request delivered when the watcher thread is destroyed.
     */
    void thread_main(std::stop_token stop) {
      const auto com_status = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
      const bool com_initialized = SUCCEEDED(com_status);

      const HINSTANCE instance = GetModuleHandleW(nullptr);
      WNDCLASSEXW window_class {};
      window_class.cbSize = sizeof(window_class);
      window_class.lpfnWndProc = watcher_proc;
      window_class.hInstance = instance;
      window_class.lpszClassName = L"SunshineClipboardWatcher";
      RegisterClassExW(&window_class);

      HWND window = CreateWindowExW(0, window_class.lpszClassName, L"Sunshine clipboard", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
      if (window == nullptr) {
        BOOST_LOG(warning) << "Clipboard sync could not create its window [0x"sv << util::hex(GetLastError()).to_string_view() << ']';
        fail_watcher();
        if (com_initialized) {
          CoUninitialize();
        }
        return;
      }

      watcher_t watcher;
      watcher.window = window;
      watcher.seen_sequence = GetClipboardSequenceNumber();
      SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&watcher));

      if (!AddClipboardFormatListener(window)) {
        BOOST_LOG(warning) << "Clipboard change notifications are unavailable, polling instead"sv;
      }
      SetTimer(window, poll_timer_id, poll_interval_ms, nullptr);

      {
        auto &clipboard = state();
        std::lock_guard lock {clipboard.mutex};
        clipboard.window = window;
        clipboard.ready = true;
        clipboard.ready_cv.notify_all();
        // A subscriber may have registered before the window existed.
        wake_watcher(clipboard);
      }

      const DWORD thread_id = GetCurrentThreadId();
      std::stop_callback on_stop {stop, [thread_id]() {
                                    PostThreadMessageW(thread_id, WM_QUIT, 0, 0);
                                  }};

      MSG message;
      while (!stop.stop_requested() && GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
      }

      {
        auto &clipboard = state();
        std::lock_guard lock {clipboard.mutex};
        clipboard.window = nullptr;
        clipboard.ready = false;
      }
      KillTimer(window, poll_timer_id);
      RemoveClipboardFormatListener(window);
      SetWindowLongPtrW(window, GWLP_USERDATA, 0);
      DestroyWindow(window);
      if (com_initialized) {
        CoUninitialize();
      }
    }
  }  // namespace

  void clipboard_set(clipboard_data_t data, const clipboard_queue_t &origin) {
    if (data.text.empty() && data.image.empty()) {
      return;
    }

    const auto shared = std::make_shared<const clipboard_data_t>(std::move(data));
    clipboard_delivery_t delivery;
    {
      auto &clipboard = state();
      std::lock_guard lock {clipboard.mutex};
      if (clipboard.failed || !clipboard.ready) {
        return;
      }
      clipboard.pending = shared;
      delivery = clipboard.subscribers.publish(shared, origin);
      wake_watcher(clipboard);
    }
    raise_offers(delivery, shared);
  }

  void clipboard_subscribe(clipboard_queue_t queue) {
    auto &clipboard = state();
    std::unique_lock lock {clipboard.mutex};
    if (!clipboard.started) {
      clipboard.started = true;
      clipboard.thread = std::jthread(thread_main);
    }
    clipboard.ready_cv.wait(lock, [&clipboard]() {
      return clipboard.ready || clipboard.failed;
    });
    if (!clipboard.ready || !clipboard.subscribers.add(std::move(queue))) {
      return;
    }
    clipboard.pull = true;
    wake_watcher(clipboard);
  }

  void clipboard_unsubscribe(const clipboard_queue_t &queue) {
    auto &clipboard = state();
    std::lock_guard lock {clipboard.mutex};
    clipboard.subscribers.remove(queue);
  }
}  // namespace platf
