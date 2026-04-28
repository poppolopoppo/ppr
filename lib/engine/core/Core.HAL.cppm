// ReSharper disable CppNonExplicitConversionOperator
export module engine.core:hal;

import std;

export namespace pP {
    static_assert(__cplusplus >= 202302L, "C++ version too low: 2023 C++ standard is required");

#ifdef _DEBUG
    inline constexpr bool enable_debug = true;
#else
    inline constexpr bool enable_debug = false;
#endif

    // ------------------------------------------------------------------
    // misc. traits
    // ------------------------------------------------------------------

    template<typename... T>
    inline constexpr bool always_false_v = false; // for conditional static asserts

    template<typename T>
    using unwrap_ref_decay_t = std::decay_t<std::unwrap_reference_t<T> >; // this is weird.

    // ------------------------------------------------------------------
    // integer types
    // ------------------------------------------------------------------

    using u8 = std::uint8_t;
    using u16 = std::uint16_t;
    using u32 = std::uint32_t;
    using u64 = std::uint64_t;

    using i8 = std::int8_t;
    using i16 = std::int16_t;
    using i32 = std::int32_t;
    using i64 = std::int64_t;

    // ------------------------------------------------------------------
    // string character types
    // ------------------------------------------------------------------

    namespace details {
        template<typename T>
        struct string_character : std::bool_constant<
                    std::is_same_v<char, T> ||
                    std::is_same_v<wchar_t, T> ||
                    std::is_same_v<char8_t, T>> {
        };

        template<typename T>
        constexpr bool is_string_character_v = string_character<T>::value;

        // list of allowed string character types
        template<typename T>
        concept TChar = is_string_character_v<T>;
    }

    // ------------------------------------------------------------------
    // impostor types
    // ------------------------------------------------------------------

    struct DefaultValue final {
        // Constrain both conversion and comparison to default-constructible types.
        template<typename T> requires std::is_default_constructible_v<T>
        // ReSharper disable once CppNonExplicitConversionOperator
        [[nodiscard]] constexpr operator T() const
            noexcept(std::is_nothrow_default_constructible_v<T>) {
            return T{};
        }

        template<typename T> requires std::is_default_constructible_v<T>
        [[nodiscard]] friend constexpr bool operator==(DefaultValue, T rhs)
            noexcept(std::is_nothrow_default_constructible_v<T> &&
                     noexcept(T{} == rhs)) {
            return T{} == rhs;
        }
    };

    struct ZeroValue final {
        // Constrain both conversion and comparison to int-constructible types.
        template<typename T> requires std::is_constructible_v<T, int>
        [[nodiscard]] constexpr operator T() const
            noexcept(std::is_nothrow_constructible_v<T, int>) {
            return T{0};
        }

        template<typename T> requires std::is_constructible_v<T, int>
        [[nodiscard]] friend constexpr bool operator==(ZeroValue, T rhs)
            noexcept(std::is_nothrow_constructible_v<T, int> &&
                     noexcept(T{0} == rhs)) {
            return T{} == rhs;
        }
    };

    struct UnsignedMax final {
        // Constrain to unsigned integral types only — the ~T(0) trick is
        // undefined behavior on signed types, so reject them at the constraint level.
        template<std::unsigned_integral T>
        [[nodiscard]] constexpr operator T() const noexcept {
            return ~T{0};
        }

        template<std::unsigned_integral T>
        [[nodiscard]] friend constexpr bool operator==(UnsignedMax lhs, T rhs) noexcept {
            return T{lhs} == rhs;
        }

        template<std::unsigned_integral T>
        [[nodiscard]] friend constexpr std::strong_ordering operator<=>(UnsignedMax lhs, T rhs) noexcept {
            return T{lhs} <=> rhs;
        }
    };

    inline constexpr DefaultValue default_value_v;
    inline constexpr ZeroValue zero_v;
    inline constexpr UnsignedMax none_v;
    inline constexpr UnsignedMax umax_v;

    // ------------------------------------------------------------------
    // alignment helpers
    // ------------------------------------------------------------------

    template<std::integral T>
    [[nodiscard]] constexpr auto divideRoundUp(T value, T div) noexcept {
        return (value + div - 1) / div;
    }

    template<std::integral T>
    [[nodiscard]] constexpr auto alignBackward(T value, T mod) noexcept {
        return (value / mod) * mod;
    }

    template<std::integral T>
    [[nodiscard]] constexpr auto alignForward(T value, T mod) noexcept {
        return divideRoundUp(value, mod) * mod;
    }

    template<std::integral T>
    [[nodiscard]] constexpr auto alignForward(T value, std::align_val_t alignment) noexcept {
        return alignForward(value, static_cast<T>(alignment));
    }

    template<typename T>
    [[nodiscard]] T *alignForward(T *ptr, std::align_val_t alignment) noexcept {
        return std::bit_cast<T *>(alignForward(std::bit_cast<std::uintptr_t>(ptr), static_cast<std::uintptr_t>(alignment)));
    }

    template<typename T>
    inline constexpr std::align_val_t alignof_v{alignof(T)};
    inline constexpr std::align_val_t max_align_v = alignof_v<std::max_align_t>;
    inline constexpr std::align_val_t simd_align_v = std::align_val_t{16u};

    // ------------------------------------------------------------------
    // bit count needed to store any type
    // ------------------------------------------------------------------

    template<typename T>
    inline constexpr std::size_t bit_count_v = sizeof(std::unwrap_ref_decay_t<T>) * 8;

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
    // memory debugging
    // ------------------------------------------------------------------

    namespace mem {
        enum class Poison : u8 {
            reserved = 0xAA,
            uninitialized = 0xCC,
            destroyed = 0xDD,
        };

        constexpr void poison(const Poison phase, void *const ptr, const std::size_t size) noexcept {
            if !consteval {
                std::memset(ptr, static_cast<u8>(phase), size);
            }
        }

        template<typename T>
        constexpr void poison(const Poison phase, T *const ptr, const std::size_t n = 1u) noexcept
            requires (!std::is_const_v<T>) {
            if !consteval {
                std::memset(ptr, static_cast<u8>(phase), sizeof(T) * n);
            }
        }

        template<typename T>
        constexpr void poisonIfDebug(const Poison phase, T *const ptr, const std::size_t n = 1u) noexcept
            requires (!std::is_const_v<T>) {
            if constexpr (enable_debug) {
                poison(phase, ptr, n);
            }
        }
    }

    // ------------------------------------------------------------------
    // defer block mock syntax, because it's a better pattern than RAII (Zig <3)
    // ------------------------------------------------------------------

    template<typename CallbackT> requires std::is_invocable_v<CallbackT>
    class [[nodiscard]] Deferred {
    public:
        Deferred(CallbackT &&callback) noexcept
            : m_callback(std::forward<CallbackT>(callback)) {
        }

        ~Deferred() noexcept(std::is_nothrow_invocable_v<CallbackT>) {
            // invoke the deferred callback when this object is destroyed
            m_callback();
        }

        Deferred(const Deferred &) = delete;

        Deferred &operator=(const Deferred &) = delete;

        Deferred(Deferred &&) noexcept = default;

        Deferred &operator=(Deferred &&) noexcept = default;

    private:
        CallbackT m_callback;
    };

    template<typename CallbackT> requires std::is_invocable_v<CallbackT>
    Deferred(CallbackT &&) -> Deferred<std::remove_cvref_t<CallbackT> >;

    template<typename CallbackT>
    [[nodiscard]] auto defer(CallbackT &&callback) noexcept requires std::is_invocable_v<CallbackT> {
        return Deferred(std::forward<CallbackT>(callback));
    }

    // -----------------------------------------------------------------------------
    // Cross‑platform enumerate view
    // -----------------------------------------------------------------------------
    // libc++ (as of LLVM 20) does not yet implement std::views::enumerate,
    // while MSVC STL and libstdc++ do. To provide a uniform API across all
    // standard libraries, we detect whether the real enumerate_view is available
    // and fall back to a portable implementation.
    // -----------------------------------------------------------------------------
    namespace views {
#if defined(__cpp_lib_ranges_enumerate) && __cpp_lib_ranges_enumerate >= 202302L

        // Use the real thing
        using std::views::enumerate;

#else

        // Fallback: enumerate = zip(iota, range)
        inline constexpr auto enumerate = []<std::ranges::input_range R>(R &&r) {
            using std::views::iota;
            using std::views::zip;

            return zip(iota(std::size_t{0}), std::forward<R>(r));
        };

#endif
    } // namespace views
}

// ------------------------------------------------------------------
// Hardware Abstraction Layer
// ------------------------------------------------------------------

export namespace pP::hal {
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
    inline constexpr std::size_t cacheline_size = std::hardware_destructive_interference_size;
#else
    inline constexpr std::size_t cacheline_size = 64u; // conservative fallback for older compilers
#endif

    struct PageProtection {
        bool read: 1 = true;
        bool write: 1 = true;
        bool execute: 1 = false;
    };

    extern const std::size_t page_size;

    extern const std::align_val_t page_granularity;

    [[nodiscard]] std::allocation_result<void *> pageAlloc(std::size_t size, bool commit = true, PageProtection allowed = {});

    void pageCommit(void *ptr, std::size_t size, PageProtection allowed = {});

    void pageDecommit(void *ptr, std::size_t size);

    void pageProtect(void *ptr, std::size_t size, PageProtection allowed);

    void pageOfferToOS(void *ptr, std::size_t size);

    [[nodiscard]] bool pageReclaimFromOS(const void *ptr, std::size_t size);

    void pageFree(void *ptr, std::size_t size);

    // ------------------------------------------------------------------
    // native strings
    // ------------------------------------------------------------------

    [[nodiscard]] std::size_t transcode(std::string_view ansi, char8_t *p_dst, std::size_t capacity) noexcept;

    [[nodiscard]] std::size_t transcode(std::string_view ansi, wchar_t *p_dst, std::size_t capacity) noexcept;

    [[nodiscard]] std::size_t transcode(std::wstring_view wide, char8_t *p_dst, std::size_t capacity) noexcept;

    [[nodiscard]] std::size_t transcode(std::u8string_view utf8, wchar_t *p_dst, std::size_t capacity) noexcept;

    [[nodiscard]] std::size_t transcode(std::wstring_view wide, char *p_dst, std::size_t capacity) noexcept;

    [[nodiscard]] std::size_t transcode(std::u8string_view utf8, char *p_dst, std::size_t capacity) noexcept;

    template<details::TChar DstCharT, details::TChar SrcCharT, typename AllocatorT = std::basic_string<DstCharT>::allocator_type>
    [[nodiscard]] decltype(auto) toString(const std::basic_string_view<SrcCharT> src, [[maybe_unused]] AllocatorT &&alloc = {})
        noexcept(std::is_same_v<SrcCharT, DstCharT>) {
        if constexpr (std::is_same_v<DstCharT, SrcCharT>) {
            return src;
        }

        const std::size_t cap = transcode(src, static_cast<DstCharT *>(nullptr), 0u);
        std::basic_string dst(cap, DstCharT{}, std::forward<AllocatorT>(alloc));
        [[maybe_unused]] const std::size_t len = transcode(src, dst.data(), dst.size());
        return dst;
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
#endif
}
