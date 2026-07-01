module;

#include "pP/Macros.h"

export module engine.core:hal;

import :types;
export import :types;
import std;

export import :utility;

export namespace pP {

    struct alignas(16u) simd_128_t {
        u32 m_data[4u]{};
    };

    inline constexpr std::align_val_t simd_align_v = alignof_v<simd_128_t>;

    // ------------------------------------------------------------------
    // base split mix hash functions (from boost)
    // ------------------------------------------------------------------

    namespace hash {
        // hash_mix for 64 bit size_t
        //
        // The general "xmxmx" form of state of the art 64 bit mixers originates
        // from Murmur3 by Austin Appleby, which uses the following function as
        // its "final mix":
        //
        //	k ^= k >> 33;
        //	k *= 0xff51afd7ed558ccd;
        //	k ^= k >> 33;
        //	k *= 0xc4ceb9fe1a85ec53;
        //	k ^= k >> 33;
        //
        // (https://github.com/aappleby/smhasher/blob/master/src/MurmurHash3.cpp)
        //
        // It has subsequently been improved multiple times by different authors
        // by changing the constants. The most well known improvement is the
        // so-called "variant 13" function by David Stafford:
        //
        //	k ^= k >> 30;
        //	k *= 0xbf58476d1ce4e5b9;
        //	k ^= k >> 27;
        //	k *= 0x94d049bb133111eb;
        //	k ^= k >> 31;
        //
        // (https://zimbry.blogspot.com/2011/09/better-bit-mixing-improving-on.html)
        //
        // This mixing function is used in the splitmix64 RNG:
        // http://xorshift.di.unimi.it/splitmix64.c
        //
        // We use Jon Maiga's implementation from
        // http://jonkagstrom.com/mx3/mx3_rev2.html
        //
        // 	x ^= x >> 32;
        //	x *= 0xe9846af9b1a615d;
        //	x ^= x >> 32;
        //	x *= 0xe9846af9b1a615d;
        //	x ^= x >> 28;
        //
        // An equally good alternative is Pelle Evensen's Moremur:
        //
        //	x ^= x >> 27;
        //	x *= 0x3C79AC492BA7B653;
        //	x ^= x >> 33;
        //	x *= 0x1C69B3F74AC4AE35;
        //	x ^= x >> 27;
        //
        // (https://mostlymangling.blogspot.com/2019/12/stronger-better-morer-moremur-better.html)

        [[nodiscard]] constexpr u64 mix(u64 x) noexcept {
            constexpr std::uint64_t m = 0xe9846af9b1a615d;

            x ^= x >> 32;
            x *= m;
            x ^= x >> 32;
            x *= m;
            x ^= x >> 28;

            return x;
        }

        // hash_mix for 32 bit size_t
        //
        // We use the "best xmxmx" implementation from
        // https://github.com/skeeto/hash-prospector/issues/19

        [[nodiscard]] constexpr u32 mix(u32 x) noexcept {
            constexpr std::uint32_t m1 = 0x21f0aaad;
            constexpr std::uint32_t m2 = 0x735a2d97;

            x ^= x >> 16;
            x *= m1;
            x ^= x >> 15;
            x *= m2;
            x ^= x >> 15;

            return x;
        }

        // https://github.com/boostorg/container_hash/blob/e3cbbebc8a1f9833287c8eb52fb0484ba744646b/include/boost/container_hash/hash.hpp#L470
        [[nodiscard]] constexpr u32 combine(const u32 s, const u32 h) noexcept {
            return mix(s + 0x9e3779b9 + h);
        }

        // https://github.com/boostorg/container_hash/blob/e3cbbebc8a1f9833287c8eb52fb0484ba744646b/include/boost/container_hash/hash.hpp#L470
        [[nodiscard]] constexpr u64 combine(const u64 s, const u64 h) noexcept {
            return mix(s + 0x9e3779b9 + h);
        }
    }

    // ------------------------------------------------------------------
    // variant visitor helper (TM)
    // ------------------------------------------------------------------

    // helper type for the visitor #4
    template<class... Ts>
    struct overloaded : Ts... {
        using Ts::operator()...;
    };

    // explicit deduction guide (not needed as of C++20)
    template<class... Ts>
    overloaded(Ts...) -> overloaded<Ts...>;

    // ------------------------------------------------------------------
    // random number generator from hardware seed
    // ------------------------------------------------------------------

    [[nodiscard]] std::mt19937_64 randomNumberGenerator() noexcept;

    // ------------------------------------------------------------------
    // defer block
    // ------------------------------------------------------------------

    template<typename CallbackT> requires std::is_invocable_v<CallbackT>
    class [[nodiscard]] Deferred {
    public:
        // ReSharper disable once CppNonExplicitConvertingConstructor
        constexpr Deferred(CallbackT &&callback)
            noexcept(std::is_nothrow_move_constructible_v<CallbackT>)
            : m_callback(std::forward<CallbackT>(callback)) {
        }

        constexpr ~Deferred()
            noexcept(std::is_nothrow_invocable_v<CallbackT> &&
                     std::is_nothrow_destructible_v<CallbackT>) {
            // invoke the deferred callback when this object is destroyed
            m_callback();
        }

        Deferred(const Deferred &) = delete;

        Deferred &operator=(const Deferred &) = delete;

        constexpr Deferred(Deferred &&) noexcept = default;

        constexpr Deferred &operator=(Deferred &&) noexcept = default;

    private:
        CallbackT m_callback;
    };

    template<typename CallbackT> requires std::is_invocable_v<CallbackT>
    Deferred(CallbackT &&) -> Deferred<std::remove_cvref_t<CallbackT> >;

    template<typename CallbackT>
    [[nodiscard]] constexpr auto defer(CallbackT &&callback)
        noexcept(Deferred(std::forward<CallbackT>(callback)))
        requires std::is_invocable_v<CallbackT> {
        return Deferred(std::forward<CallbackT>(callback));
    }

    // ------------------------------------------------------------------
    // Hardware Abstraction Layer
    // ------------------------------------------------------------------

    namespace hal {
        [[nodiscard]] std::string_view platformName() noexcept;

        [[nodiscard]] std::string_view userName();

        // ------------------------------------------------------------------
        // file-system
        // ------------------------------------------------------------------

        [[nodiscard]] const std::filesystem::directory_entry &homeDir();

        [[nodiscard]] const std::filesystem::directory_entry &systemDir();

        [[nodiscard]] const std::filesystem::directory_entry &appDataLocalDir();

        [[nodiscard]] const std::filesystem::directory_entry &appDataRoamingDir();

        // ------------------------------------------------------------------
        // memory pages
        // ------------------------------------------------------------------

#if defined(__cpp_lib_hardware_interference_size)
        inline constexpr std::size_t cacheline_size_v = std::hardware_destructive_interference_size;
#else
        inline constexpr std::size_t cacheline_size_v = 64u; // conservative fallback for older compilers
#endif

        struct PageProtection {
            bool read: 1 = true;
            bool write: 1 = true;
            bool execute: 1 = false;
        };

        extern const std::size_t page_size;

        extern const std::align_val_t page_granularity;

        [[nodiscard]] std::allocation_result<void *> pageAlloc(
            std::size_t size,
            bool commit = true,
            PageProtection allowed = {},
            std::align_val_t alignment = page_granularity) noexcept(false);

        void pageCommit(void *ptr, std::size_t size, PageProtection allowed = {}) noexcept(false);

        void pageDecommit(void *ptr, std::size_t size) noexcept(false);

        void pageProtect(void *ptr, std::size_t size, PageProtection allowed) noexcept(false);

        void pageOfferToOS(void *ptr, std::size_t size) noexcept(false);

        [[nodiscard]] bool pageReclaimFromOS(const void *ptr, std::size_t size) noexcept;

        void pageFree(void *ptr, std::size_t size) noexcept(false);

        // ------------------------------------------------------------------
        // magic ring-buffer backed by contiguous pages mapping the same memory twice
        // ------------------------------------------------------------------

        [[nodiscard]] void *ringBufferAlloc(const std::size_t buffer_size) noexcept(false);

        void ringBufferFree(const void *ring_buffer, const std::size_t buffer_size) noexcept(false);

        // ------------------------------------------------------------------
        // native strings
        // ------------------------------------------------------------------

        [[nodiscard]] std::size_t transcode(std::string_view ansi, char8_t *p_dst, std::size_t capacity) noexcept;

        [[nodiscard]] std::size_t transcode(std::string_view ansi, wchar_t *p_dst, std::size_t capacity) noexcept;

        [[nodiscard]] std::size_t transcode(std::wstring_view wide, char8_t *p_dst, std::size_t capacity) noexcept;

        [[nodiscard]] std::size_t transcode(std::u8string_view utf8, wchar_t *p_dst, std::size_t capacity) noexcept;

        [[nodiscard]] std::size_t transcode(std::wstring_view wide, char *p_dst, std::size_t capacity) noexcept;

        [[nodiscard]] std::size_t transcode(std::u8string_view utf8, char *p_dst, std::size_t capacity) noexcept;

        [[nodiscard]] std::size_t transcode(std::string_view ansi, char *p_dst, std::size_t capacity) noexcept;

        template<details::TChar DstCharT, details::TChar SrcCharT, typename AllocatorT = std::basic_string<DstCharT>::allocator_type>
        [[nodiscard]] auto toString(const std::basic_string_view<SrcCharT> src, [[maybe_unused]] AllocatorT &&alloc = {})
            -> std::basic_string<DstCharT, std::char_traits<DstCharT>, AllocatorT> {
            if constexpr (std::is_same_v<DstCharT, SrcCharT>) {
                return std::basic_string<DstCharT, std::char_traits<DstCharT>, AllocatorT>(src, std::forward<AllocatorT>(alloc));
            } else {
                const std::size_t cap = transcode(src, static_cast<DstCharT *>(nullptr), 0u);
                std::basic_string dst(cap, DstCharT{}, std::forward<AllocatorT>(alloc));
                [[maybe_unused]] const std::size_t len = transcode(src, dst.data(), dst.size());
                return dst;
            }
        }

        namespace native {
            using string = std::filesystem::path::string_type;
            using char_t = string::value_type;
            using string_view = std::basic_string_view<char_t>;

            inline constexpr bool is_wchar_v = std::is_same_v<char_t, wchar_t>;

            template<typename... ArgsT>
            using format_string = std::conditional_t<is_wchar_v, std::wformat_string<ArgsT...>, std::format_string<ArgsT...> >;
            using format_context = std::conditional_t<is_wchar_v, std::wformat_context, std::format_context>;
            using format_args = std::basic_format_args<format_context>;

            [[nodiscard]] inline std::size_t ansi(const string_view &native_str, char *out_buffer, std::size_t buffer_size) noexcept {
                return transcode(native_str, out_buffer, buffer_size);
            }

            [[nodiscard]] inline std::size_t utf8(const string_view &native_str, char8_t *out_buffer, std::size_t buffer_size) noexcept {
                return transcode(native_str, out_buffer, buffer_size);
            }

            [[nodiscard]] inline std::size_t from(const std::string_view &ansi_str, char_t *out_buffer, std::size_t buffer_size) noexcept {
                return transcode(ansi_str, out_buffer, buffer_size);
            }

            [[nodiscard]] inline std::size_t from(const std::u8string_view &utf8_str, char_t *out_buffer, std::size_t buffer_size) noexcept {
                return transcode(utf8_str, out_buffer, buffer_size);
            }

            [[nodiscard]] inline decltype(auto) ansi(const string_view &native_str) {
                return toString<char>(native_str);
            }

            [[nodiscard]] inline decltype(auto) utf8(const string_view &native_str) {
                return toString<char8_t>(native_str);
            }

            [[nodiscard]] inline decltype(auto) from(const std::string_view &ansi_str) {
                return toString<char_t>(ansi_str);
            }

            [[nodiscard]] inline decltype(auto) from(const std::u8string_view &utf8_str) {
                return toString<char_t>(utf8_str);
            }

            template<details::TChar CharT>
            [[nodiscard]] decltype(auto) format(const string_view &native_str) noexcept(std::is_same_v<char_t, CharT>) {
                if constexpr (std::is_same_v<char_t, CharT>) {
                    return native_str;
                } else {
                    static_assert(std::is_same_v<char, CharT>);
                    return utf8(native_str);
                }
            }
        }

        // ------------------------------------------------------------------
        // debugger
        // ------------------------------------------------------------------

        void outputDebug(const char *ansi_msg) noexcept;

        void outputDebug(const native::char_t *native_msg) noexcept;

        [[nodiscard]] bool isDebuggerPresent() noexcept;

        void breakpoint() noexcept;

        void breakpointIfDebugging() noexcept;

        void disableSystemErrorReporting() noexcept;

        void installDebugAssertHooks() noexcept;

        // ------------------------------------------------------------------
        // process
        // ------------------------------------------------------------------

        namespace process {
            [[nodiscard]] std::filesystem::path currentExecutablePath() noexcept(false);

            [[nodiscard]] int spawnAndWait(const std::filesystem::path &executable, std::span<const std::string> args) noexcept(false);

            [[noreturn]] void terminateProcess(int exit_code) noexcept;
        }

        // ------------------------------------------------------------------
        // deadline timers (used for test timeout enforcement)
        // ------------------------------------------------------------------

        namespace timer {
            struct DeadlineHandle {
                void *m_data{nullptr};
            };

            [[nodiscard]] DeadlineHandle setDeadline(std::chrono::milliseconds ms, std::function<void()> callback) noexcept(false);

            void cancelDeadline(DeadlineHandle &handle) noexcept;
        }

        // ------------------------------------------------------------------
        // asynchronous I/O
        // ------------------------------------------------------------------

        namespace io {
            using IoHandle = void *;
            using FileHandle = void *;
            using MapHandle = void *;

            enum class Opcode : u8 {
                read,
                write,
            };

            // Minimum storage needed for platform-specific OVERLAPPED extension.
            // Must be ≥ sizeof(OverlappedExt) on Windows (currently 40 bytes on x64).
            inline constexpr std::size_t overlapped_storage_size_v = 64u;

            struct OpenFlags {
                enum : u32 {
                    read     = 1u << 0,
                    write    = 1u << 1,
                    create   = 1u << 2,
                    truncate = 1u << 3,
                };
                u32 m_bits{read};

                constexpr OpenFlags() noexcept = default;
                explicit constexpr OpenFlags(const u32 bits) noexcept
                    : m_bits(bits) {
                }

                [[nodiscard]] friend constexpr OpenFlags operator|(const OpenFlags a, const OpenFlags b) noexcept {
                    return OpenFlags(a.m_bits | b.m_bits);
                }

                constexpr OpenFlags &operator|=(const OpenFlags other) noexcept {
                    m_bits |= other.m_bits;
                    return *this;
                }

                [[nodiscard]] friend constexpr bool operator==(const OpenFlags a, const OpenFlags b) noexcept {
                    return a.m_bits == b.m_bits;
                }
            };

            struct SubmitEntry {
                FileHandle  m_file;
                void       *m_buffer;
                u64         m_buffer_size;
                u64         m_file_offset;
                Opcode      m_opcode;
                void       *m_user_data;     // → IoRequest *
                void       *m_overlapped;    // → embedded storage (or null for heap fallback)
            };

            struct CompletionEntry {
                void          *m_user_data;  // → IoRequest *
                u64            m_bytes_transferred;
                std::error_code m_error;
            };

            // lifecycle
            [[nodiscard]] IoHandle init() noexcept(false);
            void deinit(IoHandle handle) noexcept;

            // file operations
            [[nodiscard]] FileHandle openFile(IoHandle io, const std::filesystem::path &path, OpenFlags flags) noexcept(false);
            void closeFile(IoHandle io, FileHandle file) noexcept;

            // submit & drain
            [[nodiscard]] std::size_t submit(IoHandle io, std::span<SubmitEntry> entries) noexcept;
            [[nodiscard]] std::size_t poll(IoHandle io, std::span<CompletionEntry> entries) noexcept;
            [[nodiscard]] std::size_t wait(IoHandle io, std::span<CompletionEntry> entries) noexcept;
            void wake(IoHandle io) noexcept;
            void cancelIo(FileHandle file, void *overlapped) noexcept;

            // memory-mapped files
            [[nodiscard]] MapHandle mapFile(const std::filesystem::path &path, OpenFlags flags) noexcept(false);
            void unmapFile(MapHandle map) noexcept;
            [[nodiscard]] void *mapData(MapHandle map) noexcept;
            [[nodiscard]] std::size_t mapSize(MapHandle map) noexcept;

            // directory watching
            using WatchHandle = void *;

            struct WatchEvent {
                enum class Action : u8 {
                    added,
                    removed,
                    modified,
                    renamed_old,
                    renamed_new,
                };
                Action m_action;
                u32    m_name_offset{0u};
            };

            [[nodiscard]] WatchHandle openWatch(const std::filesystem::path &dir, bool recursive) noexcept(false);
            void closeWatch(WatchHandle watch) noexcept;

            // pollWatch: non-blocking read of raw platform events into buffer.
            // Returns bytes written to buffer, or 0 if nothing pending / on error.
            // ec receives std::errc::result_out_of_range if the platform's internal buffer overflowed.
            [[nodiscard]] std::size_t pollWatch(WatchHandle watch, std::span<std::byte> buffer, std::error_code &ec) noexcept;

            // waitWatch: blocking variant of pollWatch.
            [[nodiscard]] std::size_t waitWatch(WatchHandle watch, std::span<std::byte> buffer, std::error_code &ec) noexcept;

            // Parse raw platform-specific event data into normalized WatchEvent records.
            // out_names receives concatenated null-terminated filenames relative to watched directory.
            [[nodiscard]] std::size_t parseWatchEvents(std::span<const std::byte> raw, std::span<WatchEvent> out_events, std::span<char> out_names) noexcept;
        }

        // ------------------------------------------------------------------
        // cross-platform helpers
        // ------------------------------------------------------------------

#ifdef _DEBUG
        template<typename... ArgsT>
        void outputDebugFmt(const std::format_string<ArgsT...> &fmt, ArgsT &&... args) noexcept {
            char buffer[2048]; // #TODO: use something that can fallback to a dynamic allocation when buffer is too small
            const auto [out, size] = std::format_to_n(buffer, std::size(buffer) - 1, fmt, std::forward<ArgsT>(args)...);
            *out = char{0};
            outputDebug(buffer);
        }

        template<typename... ArgsT>
        void outputDebugFmt(const native::format_string<ArgsT...> &fmt, ArgsT &&... args) noexcept {
            native::char_t buffer[2048]; // #TODO: use something that can fallback to a dynamic allocation when buffer is too small
            const auto [out, size] = std::format_to_n(buffer, std::size(buffer) - 1, fmt, std::forward<ArgsT>(args)...);
            *out = native::char_t{0};
            outputDebug(buffer);
        }
#else
        template<typename... ArgsT>
        constexpr void outputDebugFmt(const std::format_string<ArgsT...> &, ArgsT &&...) noexcept {
        }

        template<typename... ArgsT>
        constexpr void outputDebugFmt(const native::format_string<ArgsT...> &, ArgsT &&...) noexcept {
        }
#endif
    }
}
