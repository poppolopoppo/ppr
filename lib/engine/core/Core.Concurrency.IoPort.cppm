module;
#include "pP/Macros.h"
export module engine.core:io_port;

import :assert;
import :event;
import :hal;

import std;

export namespace pP {
    // ------------------------------------------------------------------
    // IoFile — RAII file handle for async I/O (move-only)
    // ------------------------------------------------------------------

    class IoFile {
        hal::io::IoHandle   m_port_handle{nullptr};
        hal::io::FileHandle m_file{nullptr};

    public:
        IoFile() noexcept = default;

        IoFile(IoFile &&other) noexcept
            : m_port_handle(std::exchange(other.m_port_handle, nullptr)),
              m_file(std::exchange(other.m_file, nullptr)) {
        }

        IoFile &operator=(IoFile &&other) noexcept {
            if (this != &other) {
                close_();
                m_port_handle = std::exchange(other.m_port_handle, nullptr);
                m_file = std::exchange(other.m_file, nullptr);
            }
            return *this;
        }

        IoFile(const IoFile &) = delete;
        IoFile &operator=(const IoFile &) = delete;

        ~IoFile() noexcept {
            close_();
        }

        [[nodiscard]] bool isValid() const noexcept {
            return m_file != nullptr;
        }

        void close() noexcept {
            close_();
        }

    private:
        friend class IoPort;

        IoFile(hal::io::IoHandle port, hal::io::FileHandle file) noexcept
            : m_port_handle(port), m_file(file) {
        }

        void close_() noexcept {
            if (m_file != nullptr) {
                hal::io::closeFile(m_port_handle, m_file);
                m_file = nullptr;
                m_port_handle = nullptr;
            }
        }
    };

    // ------------------------------------------------------------------
    // MappedFile — memory-mapped file (RAII move-only, no IEvent)
    // ------------------------------------------------------------------

    class MappedFile {
        hal::io::IoHandle  m_port_handle{nullptr};
        hal::io::MapHandle m_map{nullptr};

    public:
        MappedFile() noexcept = default;

        MappedFile(MappedFile &&other) noexcept
            : m_port_handle(std::exchange(other.m_port_handle, nullptr)),
              m_map(std::exchange(other.m_map, nullptr)) {
        }

        MappedFile &operator=(MappedFile &&other) noexcept {
            if (this != &other) {
                unmap_();
                m_port_handle = std::exchange(other.m_port_handle, nullptr);
                m_map = std::exchange(other.m_map, nullptr);
            }
            return *this;
        }

        MappedFile(const MappedFile &) = delete;
        MappedFile &operator=(const MappedFile &) = delete;

        ~MappedFile() noexcept {
            unmap_();
        }

        [[nodiscard]] std::span<const std::byte> span() const noexcept {
            if (m_map == nullptr) {
                return {};
            }
            return std::span(
                static_cast<const std::byte *>(hal::io::mapData(m_map)),
                hal::io::mapSize(m_map));
        }

        [[nodiscard]] std::size_t size() const noexcept {
            return m_map != nullptr ? hal::io::mapSize(m_map) : 0u;
        }

        [[nodiscard]] bool isValid() const noexcept {
            return m_map != nullptr;
        }

    private:
        friend class IoPort;

        MappedFile(hal::io::IoHandle port, hal::io::MapHandle map) noexcept
            : m_port_handle(port), m_map(map) {
        }

        void unmap_() noexcept {
            if (m_map != nullptr) {
                hal::io::unmapFile(m_port_handle, m_map);
                m_map = nullptr;
                m_port_handle = nullptr;
            }
        }
    };

    // ------------------------------------------------------------------
    // IoRequest — per-operation async I/O event
    // ------------------------------------------------------------------

    class IoRequest final : public IEvent {
        friend class IoPort;

        static constexpr std::size_t kOverlappedSize = hal::io::overlapped_storage_size_v;
        static_assert(kOverlappedSize >= sizeof(void *));

        PulseEvent      m_completed{};
        u64             m_bytes{};
        std::error_code m_error{};
        std::atomic<u8> m_state{0}; // 0=idle, 1=inflight, 2=done
        hal::io::FileHandle m_active_file{nullptr};
        alignas(max_align_v) std::byte m_overlapped_storage[kOverlappedSize]{};

        void complete_(const u64 bytes, const std::error_code ec) noexcept {
            u8 expected = 1u;
            if (m_state.compare_exchange_strong(expected, 2u, std::memory_order_acq_rel)) {
                m_bytes = bytes;
                m_error = ec;
                m_completed.emitEvent();
            }
        }

    public:
        IoRequest() noexcept;

        IoRequest(const IoRequest &) = delete;
        IoRequest &operator=(const IoRequest &) = delete;
        IoRequest(IoRequest &&) = delete;
        IoRequest &operator=(IoRequest &&) = delete;

        ~IoRequest() noexcept {
            if (isPending()) {
                (void)cancel();
            }
            PPR_VERIFY(not isPending());
        }

        // IEvent interface
        TagPtr<ISignal> subscribeEvent(const TagPtr<ISignal> signal) noexcept override {
            return m_completed.subscribeEvent(signal);
        }

        void unsubscribeEvent(const TagPtr<ISignal> signal, const TagPtr<ISignal> restore) noexcept override {
            m_completed.unsubscribeEvent(signal, restore);
        }

        [[nodiscard]] bool pollEvent() noexcept override {
            return m_completed.pollEvent();
        }

        void resetEvent() noexcept override {
            m_state.store(0, std::memory_order_release);
            m_completed.resetEvent();
        }

        [[nodiscard]] u64 bytesTransferred() const noexcept {
            PPR_ASSERT(m_state.load(std::memory_order_acquire) == 2u);
            return m_bytes;
        }

        [[nodiscard]] std::error_code error() const noexcept {
            PPR_ASSERT(m_state.load(std::memory_order_acquire) == 2u);
            return m_error;
        }

        [[nodiscard]] bool isPending() const noexcept {
            return m_state.load(std::memory_order_acquire) == 1u;
        }

        [[nodiscard]] bool cancel() noexcept;
    };

    // ------------------------------------------------------------------
    // IoPort — async I/O driver with background completion thread
    // ------------------------------------------------------------------

    class IoPort final {
        hal::io::IoHandle m_handle{nullptr};
        std::jthread      m_poller;

        static void pollerLoop_(std::stop_token stop, IoPort &self) noexcept;

    public:
        IoPort();
        ~IoPort() noexcept;

        IoPort(const IoPort &) = delete;
        IoPort &operator=(const IoPort &) = delete;
        IoPort(IoPort &&) = delete;
        IoPort &operator=(IoPort &&) = delete;

        // file operations
        [[nodiscard]] IoFile open(const std::filesystem::path &path,
                                   const hal::io::OpenFlags flags = {}) noexcept(false);

        // async I/O submission
        void read(IoRequest &req, const IoFile &file,
                  std::span<std::byte> buffer, const u64 file_offset) noexcept;

        void write(IoRequest &req, const IoFile &file,
                   std::span<const std::byte> buffer, const u64 file_offset) noexcept;

        // memory-mapped file (synchronous)
        [[nodiscard]] MappedFile map(const std::filesystem::path &path,
                                      const hal::io::OpenFlags flags = hal::io::OpenFlags::read) noexcept(false);
    };

    template<> struct details::relocatable<IoFile> : std::true_type {};
    template<> struct details::relocatable<MappedFile> : std::true_type {};
}
