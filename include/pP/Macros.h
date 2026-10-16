#pragma once

// ------------------------------------------------------------------
// detects debug builds conservatively
// ------------------------------------------------------------------

#if defined(_DEBUG) || !defined(NDEBUG)
#define PPR_ENABLE_DEBUG 1
#define PPR_DECL_IF_DEBUG(...) __VA_ARGS__
#define PPR_EXPR_IF_DEBUG(...) __VA_ARGS__
#else
#define PPR_ENABLE_DEBUG 0
#define PPR_DECL_IF_DEBUG(...)
#define PPR_EXPR_IF_DEBUG(...) (void) 0
#endif

// ------------------------------------------------------------------
// safe-object tracking is ASAN-only; memory poisoning is DEBUG-only
// ------------------------------------------------------------------

#if defined(PPR_ENABLE_SANITIZER_ADDRESS)
#define PPR_ENABLE_SAFE_OBJECT_TRACKING 1
#else
#define PPR_ENABLE_SAFE_OBJECT_TRACKING 0
#endif

#if PPR_ENABLE_DEBUG
#define PPR_ENABLE_MEMORY_POISONING 1
#else
#define PPR_ENABLE_MEMORY_POISONING 0
#endif

// ------------------------------------------------------------------
// detects 32-bits vs 64-bits builds, note that 32-bits are deprecated.
// ------------------------------------------------------------------

#if INTPTR_MAX == INT64_MAX
#define PPR_64BIT 1
#define PPR_32BIT 0
#define PPR_32BIT_OR_64BIT(_32BIT, _64BIT) _64BIT
#elif INTPTR_MAX == INT32_MAX
#define PPR_64BIT 0
#define PPR_32BIT 1
#define PPR_32BIT_OR_64BIT(_32BIT, _64BIT) _32BIT
#else
#error "Unknown pointer size: cannot determine 32-bit vs 64-bit build"
#endif

// ------------------------------------------------------------------
// macro helpers
// ------------------------------------------------------------------

#define PPR_EXPAND(x) x
#define PPR_EXPAND_VA(X, ...) X, ##__VA_ARGS__

#define PPR_COMMA_3 ,
#define PPR_COMMA_2 PPR_EXPAND(PPR_COMMA_3)
#define PPR_COMMA_1 PPR_EXPAND(PPR_COMMA_2)
#define PPR_COMMA PPR_COMMA_1
#define PPR_COMMA_PROTECT(...) __VA_ARGS__

#define PPR_STRINGIZE_2(...) #__VA_ARGS__
#define PPR_STRINGIZE_1(...) PPR_EXPAND(PPR_STRINGIZE_2(__VA_ARGS__))
#define PPR_STRINGIZE_0(...) PPR_EXPAND(PPR_STRINGIZE_1(__VA_ARGS__))
#define PPR_STRINGIZE(...) PPR_EXPAND(PPR_STRINGIZE_0(__VA_ARGS__))

#define PPR_CONCAT_I(_X, _Y) _X##_Y
#define PPR_CONCAT_OO(_ARGS) PPR_CONCAT_I##_ARGS
#define PPR_CONCAT(_X, _Y) PPR_CONCAT_I(_X, _Y)
#define PPR_CONCAT3(_X, _Y, _Z) PPR_CONCAT(_X, PPR_CONCAT(_Y, _Z))

// ------------------------------------------------------------------
// compiler specific attributes
// ------------------------------------------------------------------

#ifdef _MSC_VER
extern "C" void _ReadWriteBarrier();
#pragma intrinsic(_ReadWriteBarrier)

#define PPR_ATTRIBUTE_CODE_SEGMENT(_NAME) __declspec(code_seg(_NAME))
#define PPR_ASSUME(...) __assume(__VA_ARGS__)
#define PPR_COMPILER_READWRITE_BARRIER() ::_ReadWriteBarrier()
#define PPR_EMPTY_BASES __declspec(empty_bases)
#define PPR_FLATTEN [[msvc::flatten]]
#define PPR_FORCE_INLINE [[msvc::forceinline]]
#define PPR_LIFETIME_BOUND [[msvc::lifetimebound]]
#define PPR_NO_INLINE [[msvc::noinline]]
#define PPR_OFFSETOF(_STRUCT, _MEMBER) __builtin_offsetof(_STRUCT, _MEMBER)
#define PPR_PRAGMA_SYSTEM_HEADER()
#define PPR_PRAGMA_WARNING_PUSH() __pragma(warning(push))
#define PPR_PRAGMA_WARNING_DISABLE_MSVC(_WARNING_CODE) __pragma(warning(disable : _WARNING_CODE))
#define PPR_PRAGMA_WARNING_DISABLE_GCC_CLANG(_WARNING_ID)
#define PPR_PRAGMA_WARNING_POP() __pragma(warning(pop))
#elif defined(__clang__) || defined(__GNUC__)
#define PPR_ATTRIBUTE_CODE_SEGMENT(_NAME)
#define PPR_ASSUME(...) ::__builtin_assume(__VA_ARGS__)
#define PPR_COMPILER_READWRITE_BARRIER() asm volatile("" ::: "memory")
#define PPR_EMPTY_BASES
#define PPR_FLATTEN [[gnu::flatten]]
#define PPR_FORCE_INLINE [[gnu::always_inline]] inline
#if defined(__clang__)
#define PPR_LIFETIME_BOUND [[clang::lifetimebound]]
#define PPR_PRAGMA_WARNING_PUSH() _Pragma("clang diagnostic push")
#define PPR_PRAGMA_WARNING_DISABLE_GCC_CLANG(_WARNING_ID) _Pragma(PPR_STRINGIZE(clang diagnostic ignored #_WARNING_ID))
#define PPR_PRAGMA_WARNING_POP() _Pragma("clang diagnostic pop")
#define PPR_PRAGMA_SYSTEM_HEADER() _Pragma("clang system_header")
#else
#define PPR_LIFETIME_BOUND [[gcc::lifetimebound]]
#define PPR_PRAGMA_WARNING_PUSH() _Pragma("GCC diagnostic push")
#define PPR_PRAGMA_WARNING_DISABLE_GCC_CLANG(_WARNING_ID) _Pragma(PPR_STRINGIZE(GCC diagnostic ignored #_WARNING_ID))
#define PPR_PRAGMA_WARNING_POP() _Pragma("GCC diagnostic pop")
#define PPR_PRAGMA_SYSTEM_HEADER() _Pragma("GCC system_header")
#endif
#define PPR_NO_INLINE [[gnu::noinline]]
#define PPR_OFFSETOF(_STRUCT, _MEMBER) __builtin_offsetof(_STRUCT, _MEMBER)
#define PPR_PRAGMA_WARNING_DISABLE_MSVC(_WARNING_CODE)
#else
#define PPR_ATTRIBUTE_CODE_SEGMENT(_NAME)
#define PPR_ASSUME(...) ((void) 0)
#define PPR_COMPILER_READWRITE_BARRIER() ((void) 0)
#define PPR_EMPTY_BASES
#define PPR_FLATTEN
#define PPR_FORCE_INLINE
#define PPR_LIFETIME_BOUND
#define PPR_NO_INLINE
#define PPR_OFFSETOF(_STRUCT, _MEMBER) ((::size_t) &reinterpret_cast<char const volatile &>((((_STRUCT *) 0)->_MEMBER)))
#define PPR_PRAGMA_WARNING_PUSH()
#define PPR_PRAGMA_WARNING_DISABLE_MSVC(_WARNING_CODE)
#define PPR_PRAGMA_WARNING_DISABLE_GCC_CLANG(_WARNING_ID)
#define PPR_PRAGMA_WARNING_POP()
#define PPR_PRAGMA_SYSTEM_HEADER()
#endif

// ------------------------------------------------------------------
// wide char handling
// ------------------------------------------------------------------

#ifndef TEXT
#if defined(_MSC_VER)
#define TEXT(quote) L##quote
#else
#define TEXT(quote) quote
#endif
#endif

#define PPR_LITERAL_FOR(_CharT, _Text)                                                                                 \
    ([] [[nodiscard]] () consteval noexcept -> decltype(auto) {                                                        \
        if constexpr (std::is_same_v<_CharT, char>)                                                                    \
            return _Text;                                                                                              \
        else if constexpr (std::is_same_v<_CharT, wchar_t>)                                                            \
            return L##_Text;                                                                                           \
        else if constexpr (std::is_same_v<_CharT, char8_t>)                                                            \
            return u8##_Text;                                                                                          \
        else                                                                                                           \
            std::unreachable();                                                                                        \
    }())

// ------------------------------------------------------------------
// RAII helpers
// ------------------------------------------------------------------

#define PPR_ANONYMIZE(_X) PPR_CONCAT(_X, __LINE__)

#define PPR_DEFER const pP::Deferred PPR_ANONYMIZE(deferred_) = [&]() -> void

// ------------------------------------------------------------------
// assertions
// ------------------------------------------------------------------

#define PPR_ENABLE_ASSERTIONS PPR_ENABLE_DEBUG

#if PPR_ENABLE_ASSERTIONS
#define PPR_DETAILS_ASSERTION_IMPL(_TYPE, ...)                                                                         \
    do {                                                                                                               \
        const bool PPR_ANONYMIZE(predicate) = (__VA_ARGS__);                                                           \
        if consteval {                                                                                                 \
            PPR_ASSUME(PPR_ANONYMIZE(predicate));                                                                      \
        } else {                                                                                                       \
            static constexpr auto PPR_ANONYMIZE(assertion_site) = std::source_location::current();                     \
                                                                                                                       \
            if (not PPR_ANONYMIZE(predicate)) [[unlikely]]                                                             \
                [&]() PPR_ATTRIBUTE_CODE_SEGMENT(".ppr_dbg") {                                                         \
                    ::pP::Assertion::onFailure(                                                                        \
                        ::pP::Assertion::_TYPE, PPR_STRINGIZE(__VA_ARGS__), PPR_ANONYMIZE(assertion_site));            \
                }();                                                                                                   \
        }                                                                                                              \
    } while (0)

#define PPR_ASSERT(...) PPR_DETAILS_ASSERTION_IMPL(require, __VA_ARGS__)
#define PPR_VERIFY(...) PPR_DETAILS_ASSERTION_IMPL(verify, __VA_ARGS__)

#define PPR_ENSURE(...)                                                                                                \
    ((__VA_ARGS__) ? true                                                                                              \
                   : (                                                                                                 \
                         []() PPR_ATTRIBUTE_CODE_SEGMENT(".ppr_dbg") {                                                 \
                             pP::Assertion::onFailure(                                                                 \
                                 pP::Assertion::ensure, PPR_STRINGIZE(__VA_ARGS__), std::source_location::current());  \
                         }(),                                                                                          \
                         false))

#else
#define PPR_ASSERT(...) PPR_ASSUME(__VA_ARGS__)
#define PPR_ENSURE(...) (PPR_ASSUME(__VA_ARGS__), __VA_ARGS__)
#define PPR_VERIFY(...)                                                                                                \
    do {                                                                                                               \
        [[maybe_unused]] const auto PPR_ANONYMIZE(condition) = (__VA_ARGS__);                                          \
        PPR_ASSUME(PPR_ANONYMIZE(condition));                                                                          \
    } while (0)
#endif

// ------------------------------------------------------------------
// logging
// ------------------------------------------------------------------

// Authoritative compile-time logging threshold, fed by the PPR_LOGGING_LEVEL
// CMake option (OFF, DEBUG, VERBOSE, INFO, EMPHASIS, WARNING, ERROR, FATAL)
// via the numeric PPR_LOG_LEVEL_THRESHOLD compile definition. Severity order
// matches pP::Log::ELevel: debug=0, verbose=1, info=2, emphasis=3, warning=4,
// error=5, fatal=6. OFF (7) compiles out every PPR_LOG. The caller-side gate
// below erases sub-threshold PPR_LOG/PPR_LOG_RAW calls with their argument
// expressions (no evaluation below threshold). PPR_ENABLE_LOGGING stays as
// the coarse backward-compatible switch derived from the threshold.
#define PPR_LOG_LEVEL_DEBUG 0
#define PPR_LOG_LEVEL_VERBOSE 1
#define PPR_LOG_LEVEL_INFO 2
#define PPR_LOG_LEVEL_EMPHASIS 3
#define PPR_LOG_LEVEL_WARNING 4
#define PPR_LOG_LEVEL_ERROR 5
#define PPR_LOG_LEVEL_FATAL 6
#define PPR_LOG_LEVEL_OFF 7

#ifndef PPR_LOG_LEVEL_THRESHOLD
#define PPR_LOG_LEVEL_THRESHOLD PPR_LOG_LEVEL_WARNING
#endif

#if PPR_LOG_LEVEL_THRESHOLD > PPR_LOG_LEVEL_OFF
#error "PPR_LOG_LEVEL_THRESHOLD out of range (expected 0..7)"
#endif

#if PPR_LOG_LEVEL_THRESHOLD >= PPR_LOG_LEVEL_OFF
#define PPR_ENABLE_LOGGING 0
#else
#define PPR_ENABLE_LOGGING 1
#endif

#if PPR_ENABLE_LOGGING
#define PPR_DECLARE_LOG_CATEGORY(_NAME)                                                                                \
    namespace details::log {                                                                                           \
        [[nodiscard]] pP::Log::Category &_NAME() noexcept;                                                             \
    }

// ReSharper disable CppUseInternalLinkage
#define PPR_DEFINE_LOG_CATEGORY(_NAME, _VERBOSITY, _FLAGS)                                                             \
    namespace details::log {                                                                                           \
        [[nodiscard]] pP::Log::Category &_NAME() noexcept {                                                            \
            static pP::Log::Category g_instance{                                                                       \
                PPR_STRINGIZE(_NAME),                                                                                  \
                (pP::Log::ELevel::_VERBOSITY),                                                                         \
                (pP::Log::Category::EFlags::_FLAGS),                                                                   \
            };                                                                                                         \
            return g_instance;                                                                                         \
        }                                                                                                              \
    }
// ReSharper restore CppUseInternalLinkage

// Caller-side compile-out gate. Call sites always pass a bare lowercase
// severity, so each level gets a lowercase-to-numeric alias and a
// preprocessor-selected body: levels below PPR_LOG_LEVEL_THRESHOLD expand
// to (void) 0, and their argument expressions (Dicts, typeid, opaqueValue,
// ...) vanish entirely instead of reaching the async sink.
#define PPR_LOG_LEVEL_debug PPR_LOG_LEVEL_DEBUG
#define PPR_LOG_LEVEL_verbose PPR_LOG_LEVEL_VERBOSE
#define PPR_LOG_LEVEL_info PPR_LOG_LEVEL_INFO
#define PPR_LOG_LEVEL_emphasis PPR_LOG_LEVEL_EMPHASIS
#define PPR_LOG_LEVEL_warning PPR_LOG_LEVEL_WARNING
#define PPR_LOG_LEVEL_error PPR_LOG_LEVEL_ERROR
#define PPR_LOG_LEVEL_fatal PPR_LOG_LEVEL_FATAL

#if PPR_LOG_LEVEL_debug >= PPR_LOG_LEVEL_THRESHOLD
#define PPR_DETAILS_LOG_debug(_CATEGORY, _MESSAGE, ...)                                                                \
    do {                                                                                                               \
        const ::pP::Log::Emitter _ppr_emitter{details::log::_CATEGORY(), ::pP::Log::ELevel::debug};                    \
        if (::pP::Log::isVisible(_ppr_emitter) || _ppr_emitter.shouldBreakDebugger())                                  \
            ::pP::Log::log(_ppr_emitter, (_MESSAGE), ##__VA_ARGS__);                                                   \
    } while (0)
#define PPR_DETAILS_LOG_RAW_debug(_CATEGORY, _MESSAGE, ...)                                                            \
    do {                                                                                                               \
        const ::pP::Log::Emitter _ppr_emitter{details::log::_CATEGORY(), ::pP::Log::ELevel::debug};                    \
        if (::pP::Log::isVisible(_ppr_emitter) || _ppr_emitter.shouldBreakDebugger())                                  \
            ::pP::Log::logRaw(_ppr_emitter, (_MESSAGE), ##__VA_ARGS__);                                                \
    } while (0)
#else
#define PPR_DETAILS_LOG_debug(_CATEGORY, _MESSAGE, ...) (void) 0
#define PPR_DETAILS_LOG_RAW_debug(_CATEGORY, _MESSAGE, ...) (void) 0
#endif

#if PPR_LOG_LEVEL_verbose >= PPR_LOG_LEVEL_THRESHOLD
#define PPR_DETAILS_LOG_verbose(_CATEGORY, _MESSAGE, ...)                                                              \
    do {                                                                                                               \
        const ::pP::Log::Emitter _ppr_emitter{details::log::_CATEGORY(), ::pP::Log::ELevel::verbose};                  \
        if (::pP::Log::isVisible(_ppr_emitter) || _ppr_emitter.shouldBreakDebugger())                                  \
            ::pP::Log::log(_ppr_emitter, (_MESSAGE), ##__VA_ARGS__);                                                   \
    } while (0)
#define PPR_DETAILS_LOG_RAW_verbose(_CATEGORY, _MESSAGE, ...)                                                          \
    do {                                                                                                               \
        const ::pP::Log::Emitter _ppr_emitter{details::log::_CATEGORY(), ::pP::Log::ELevel::verbose};                  \
        if (::pP::Log::isVisible(_ppr_emitter) || _ppr_emitter.shouldBreakDebugger())                                  \
            ::pP::Log::logRaw(_ppr_emitter, (_MESSAGE), ##__VA_ARGS__);                                                \
    } while (0)
#else
#define PPR_DETAILS_LOG_verbose(_CATEGORY, _MESSAGE, ...) (void) 0
#define PPR_DETAILS_LOG_RAW_verbose(_CATEGORY, _MESSAGE, ...) (void) 0
#endif

#if PPR_LOG_LEVEL_info >= PPR_LOG_LEVEL_THRESHOLD
#define PPR_DETAILS_LOG_info(_CATEGORY, _MESSAGE, ...)                                                                 \
    do {                                                                                                               \
        const ::pP::Log::Emitter _ppr_emitter{details::log::_CATEGORY(), ::pP::Log::ELevel::info};                     \
        if (::pP::Log::isVisible(_ppr_emitter) || _ppr_emitter.shouldBreakDebugger())                                  \
            ::pP::Log::log(_ppr_emitter, (_MESSAGE), ##__VA_ARGS__);                                                   \
    } while (0)
#define PPR_DETAILS_LOG_RAW_info(_CATEGORY, _MESSAGE, ...)                                                             \
    do {                                                                                                               \
        const ::pP::Log::Emitter _ppr_emitter{details::log::_CATEGORY(), ::pP::Log::ELevel::info};                     \
        if (::pP::Log::isVisible(_ppr_emitter) || _ppr_emitter.shouldBreakDebugger())                                  \
            ::pP::Log::logRaw(_ppr_emitter, (_MESSAGE), ##__VA_ARGS__);                                                \
    } while (0)
#else
#define PPR_DETAILS_LOG_info(_CATEGORY, _MESSAGE, ...) (void) 0
#define PPR_DETAILS_LOG_RAW_info(_CATEGORY, _MESSAGE, ...) (void) 0
#endif

#if PPR_LOG_LEVEL_emphasis >= PPR_LOG_LEVEL_THRESHOLD
#define PPR_DETAILS_LOG_emphasis(_CATEGORY, _MESSAGE, ...)                                                             \
    do {                                                                                                               \
        const ::pP::Log::Emitter _ppr_emitter{details::log::_CATEGORY(), ::pP::Log::ELevel::emphasis};                 \
        if (::pP::Log::isVisible(_ppr_emitter) || _ppr_emitter.shouldBreakDebugger())                                  \
            ::pP::Log::log(_ppr_emitter, (_MESSAGE), ##__VA_ARGS__);                                                   \
    } while (0)
#define PPR_DETAILS_LOG_RAW_emphasis(_CATEGORY, _MESSAGE, ...)                                                         \
    do {                                                                                                               \
        const ::pP::Log::Emitter _ppr_emitter{details::log::_CATEGORY(), ::pP::Log::ELevel::emphasis};                 \
        if (::pP::Log::isVisible(_ppr_emitter) || _ppr_emitter.shouldBreakDebugger())                                  \
            ::pP::Log::logRaw(_ppr_emitter, (_MESSAGE), ##__VA_ARGS__);                                                \
    } while (0)
#else
#define PPR_DETAILS_LOG_emphasis(_CATEGORY, _MESSAGE, ...) (void) 0
#define PPR_DETAILS_LOG_RAW_emphasis(_CATEGORY, _MESSAGE, ...) (void) 0
#endif

#if PPR_LOG_LEVEL_warning >= PPR_LOG_LEVEL_THRESHOLD
#define PPR_DETAILS_LOG_warning(_CATEGORY, _MESSAGE, ...)                                                              \
    do {                                                                                                               \
        const ::pP::Log::Emitter _ppr_emitter{details::log::_CATEGORY(), ::pP::Log::ELevel::warning};                  \
        if (::pP::Log::isVisible(_ppr_emitter) || _ppr_emitter.shouldBreakDebugger())                                  \
            ::pP::Log::log(_ppr_emitter, (_MESSAGE), ##__VA_ARGS__);                                                   \
    } while (0)
#define PPR_DETAILS_LOG_RAW_warning(_CATEGORY, _MESSAGE, ...)                                                          \
    do {                                                                                                               \
        const ::pP::Log::Emitter _ppr_emitter{details::log::_CATEGORY(), ::pP::Log::ELevel::warning};                  \
        if (::pP::Log::isVisible(_ppr_emitter) || _ppr_emitter.shouldBreakDebugger())                                  \
            ::pP::Log::logRaw(_ppr_emitter, (_MESSAGE), ##__VA_ARGS__);                                                \
    } while (0)
#else
#define PPR_DETAILS_LOG_warning(_CATEGORY, _MESSAGE, ...) (void) 0
#define PPR_DETAILS_LOG_RAW_warning(_CATEGORY, _MESSAGE, ...) (void) 0
#endif

#if PPR_LOG_LEVEL_error >= PPR_LOG_LEVEL_THRESHOLD
#define PPR_DETAILS_LOG_error(_CATEGORY, _MESSAGE, ...)                                                                \
    do {                                                                                                               \
        const ::pP::Log::Emitter _ppr_emitter{details::log::_CATEGORY(), ::pP::Log::ELevel::error};                    \
        if (::pP::Log::isVisible(_ppr_emitter) || _ppr_emitter.shouldBreakDebugger())                                  \
            ::pP::Log::log(_ppr_emitter, (_MESSAGE), ##__VA_ARGS__);                                                   \
    } while (0)
#define PPR_DETAILS_LOG_RAW_error(_CATEGORY, _MESSAGE, ...)                                                            \
    do {                                                                                                               \
        const ::pP::Log::Emitter _ppr_emitter{details::log::_CATEGORY(), ::pP::Log::ELevel::error};                    \
        if (::pP::Log::isVisible(_ppr_emitter) || _ppr_emitter.shouldBreakDebugger())                                  \
            ::pP::Log::logRaw(_ppr_emitter, (_MESSAGE), ##__VA_ARGS__);                                                \
    } while (0)
#else
#define PPR_DETAILS_LOG_error(_CATEGORY, _MESSAGE, ...) (void) 0
#define PPR_DETAILS_LOG_RAW_error(_CATEGORY, _MESSAGE, ...) (void) 0
#endif

#if PPR_LOG_LEVEL_fatal >= PPR_LOG_LEVEL_THRESHOLD
#define PPR_DETAILS_LOG_fatal(_CATEGORY, _MESSAGE, ...)                                                                \
    do {                                                                                                               \
        const ::pP::Log::Emitter _ppr_emitter{details::log::_CATEGORY(), ::pP::Log::ELevel::fatal};                    \
        if (::pP::Log::isVisible(_ppr_emitter) || _ppr_emitter.shouldBreakDebugger())                                  \
            ::pP::Log::log(_ppr_emitter, (_MESSAGE), ##__VA_ARGS__);                                                   \
    } while (0)
#define PPR_DETAILS_LOG_RAW_fatal(_CATEGORY, _MESSAGE, ...)                                                            \
    do {                                                                                                               \
        const ::pP::Log::Emitter _ppr_emitter{details::log::_CATEGORY(), ::pP::Log::ELevel::fatal};                    \
        if (::pP::Log::isVisible(_ppr_emitter) || _ppr_emitter.shouldBreakDebugger())                                  \
            ::pP::Log::logRaw(_ppr_emitter, (_MESSAGE), ##__VA_ARGS__);                                                \
    } while (0)
#else
#define PPR_DETAILS_LOG_fatal(_CATEGORY, _MESSAGE, ...) (void) 0
#define PPR_DETAILS_LOG_RAW_fatal(_CATEGORY, _MESSAGE, ...) (void) 0
#endif

#define PPR_LOG(_CATEGORY, _LEVEL, _MESSAGE, ...)                                                                      \
    PPR_CONCAT(PPR_DETAILS_LOG_, _LEVEL)(_CATEGORY, (_MESSAGE), ##__VA_ARGS__)

#define PPR_LOG_RAW(_CATEGORY, _LEVEL, _MESSAGE, ...)                                                                  \
    PPR_CONCAT(PPR_DETAILS_LOG_RAW_, _LEVEL)(_CATEGORY, (_MESSAGE), ##__VA_ARGS__)

#if PPR_LOG_LEVEL_debug >= PPR_LOG_LEVEL_THRESHOLD
#define PPR_DETAILS_LOG_ONCE_debug(_CATEGORY, _MESSAGE, ...)                                                           \
    do {                                                                                                               \
        if (::pP::Log::Once::claim(__FILE__, static_cast<unsigned>(__LINE__)))                                         \
            PPR_LOG(_CATEGORY, debug, (_MESSAGE), ##__VA_ARGS__);                                                      \
    } while (0)
#define PPR_DETAILS_LOG_ONCE_KEY_debug(_CATEGORY, _KEY, _MESSAGE, ...)                                                 \
    do {                                                                                                               \
        if (::pP::Log::Once::claim(_KEY))                                                                              \
            PPR_LOG(_CATEGORY, debug, (_MESSAGE), ##__VA_ARGS__);                                                      \
    } while (0)
#else
#define PPR_DETAILS_LOG_ONCE_debug(_CATEGORY, _MESSAGE, ...) (void) 0
#define PPR_DETAILS_LOG_ONCE_KEY_debug(_CATEGORY, _KEY, _MESSAGE, ...) (void) 0
#endif

#if PPR_LOG_LEVEL_verbose >= PPR_LOG_LEVEL_THRESHOLD
#define PPR_DETAILS_LOG_ONCE_verbose(_CATEGORY, _MESSAGE, ...)                                                         \
    do {                                                                                                               \
        if (::pP::Log::Once::claim(__FILE__, static_cast<unsigned>(__LINE__)))                                         \
            PPR_LOG(_CATEGORY, verbose, (_MESSAGE), ##__VA_ARGS__);                                                    \
    } while (0)
#define PPR_DETAILS_LOG_ONCE_KEY_verbose(_CATEGORY, _KEY, _MESSAGE, ...)                                               \
    do {                                                                                                               \
        if (::pP::Log::Once::claim(_KEY))                                                                              \
            PPR_LOG(_CATEGORY, verbose, (_MESSAGE), ##__VA_ARGS__);                                                    \
    } while (0)
#else
#define PPR_DETAILS_LOG_ONCE_verbose(_CATEGORY, _MESSAGE, ...) (void) 0
#define PPR_DETAILS_LOG_ONCE_KEY_verbose(_CATEGORY, _KEY, _MESSAGE, ...) (void) 0
#endif

#if PPR_LOG_LEVEL_info >= PPR_LOG_LEVEL_THRESHOLD
#define PPR_DETAILS_LOG_ONCE_info(_CATEGORY, _MESSAGE, ...)                                                            \
    do {                                                                                                               \
        if (::pP::Log::Once::claim(__FILE__, static_cast<unsigned>(__LINE__)))                                         \
            PPR_LOG(_CATEGORY, info, (_MESSAGE), ##__VA_ARGS__);                                                       \
    } while (0)
#define PPR_DETAILS_LOG_ONCE_KEY_info(_CATEGORY, _KEY, _MESSAGE, ...)                                                  \
    do {                                                                                                               \
        if (::pP::Log::Once::claim(_KEY))                                                                              \
            PPR_LOG(_CATEGORY, info, (_MESSAGE), ##__VA_ARGS__);                                                       \
    } while (0)
#else
#define PPR_DETAILS_LOG_ONCE_info(_CATEGORY, _MESSAGE, ...) (void) 0
#define PPR_DETAILS_LOG_ONCE_KEY_info(_CATEGORY, _KEY, _MESSAGE, ...) (void) 0
#endif

#if PPR_LOG_LEVEL_emphasis >= PPR_LOG_LEVEL_THRESHOLD
#define PPR_DETAILS_LOG_ONCE_emphasis(_CATEGORY, _MESSAGE, ...)                                                        \
    do {                                                                                                               \
        if (::pP::Log::Once::claim(__FILE__, static_cast<unsigned>(__LINE__)))                                         \
            PPR_LOG(_CATEGORY, emphasis, (_MESSAGE), ##__VA_ARGS__);                                                   \
    } while (0)
#define PPR_DETAILS_LOG_ONCE_KEY_emphasis(_CATEGORY, _KEY, _MESSAGE, ...)                                              \
    do {                                                                                                               \
        if (::pP::Log::Once::claim(_KEY))                                                                              \
            PPR_LOG(_CATEGORY, emphasis, (_MESSAGE), ##__VA_ARGS__);                                                   \
    } while (0)
#else
#define PPR_DETAILS_LOG_ONCE_emphasis(_CATEGORY, _MESSAGE, ...) (void) 0
#define PPR_DETAILS_LOG_ONCE_KEY_emphasis(_CATEGORY, _KEY, _MESSAGE, ...) (void) 0
#endif

#if PPR_LOG_LEVEL_warning >= PPR_LOG_LEVEL_THRESHOLD
#define PPR_DETAILS_LOG_ONCE_warning(_CATEGORY, _MESSAGE, ...)                                                         \
    do {                                                                                                               \
        if (::pP::Log::Once::claim(__FILE__, static_cast<unsigned>(__LINE__)))                                         \
            PPR_LOG(_CATEGORY, warning, (_MESSAGE), ##__VA_ARGS__);                                                    \
    } while (0)
#define PPR_DETAILS_LOG_ONCE_KEY_warning(_CATEGORY, _KEY, _MESSAGE, ...)                                               \
    do {                                                                                                               \
        if (::pP::Log::Once::claim(_KEY))                                                                              \
            PPR_LOG(_CATEGORY, warning, (_MESSAGE), ##__VA_ARGS__);                                                    \
    } while (0)
#else
#define PPR_DETAILS_LOG_ONCE_warning(_CATEGORY, _MESSAGE, ...) (void) 0
#define PPR_DETAILS_LOG_ONCE_KEY_warning(_CATEGORY, _KEY, _MESSAGE, ...) (void) 0
#endif

#if PPR_LOG_LEVEL_error >= PPR_LOG_LEVEL_THRESHOLD
#define PPR_DETAILS_LOG_ONCE_error(_CATEGORY, _MESSAGE, ...)                                                           \
    do {                                                                                                               \
        if (::pP::Log::Once::claim(__FILE__, static_cast<unsigned>(__LINE__)))                                         \
            PPR_LOG(_CATEGORY, error, (_MESSAGE), ##__VA_ARGS__);                                                      \
    } while (0)
#define PPR_DETAILS_LOG_ONCE_KEY_error(_CATEGORY, _KEY, _MESSAGE, ...)                                                 \
    do {                                                                                                               \
        if (::pP::Log::Once::claim(_KEY))                                                                              \
            PPR_LOG(_CATEGORY, error, (_MESSAGE), ##__VA_ARGS__);                                                      \
    } while (0)
#else
#define PPR_DETAILS_LOG_ONCE_error(_CATEGORY, _MESSAGE, ...) (void) 0
#define PPR_DETAILS_LOG_ONCE_KEY_error(_CATEGORY, _KEY, _MESSAGE, ...) (void) 0
#endif

#if PPR_LOG_LEVEL_fatal >= PPR_LOG_LEVEL_THRESHOLD
#define PPR_DETAILS_LOG_ONCE_fatal(_CATEGORY, _MESSAGE, ...)                                                           \
    do {                                                                                                               \
        if (::pP::Log::Once::claim(__FILE__, static_cast<unsigned>(__LINE__)))                                         \
            PPR_LOG(_CATEGORY, fatal, (_MESSAGE), ##__VA_ARGS__);                                                      \
    } while (0)
#define PPR_DETAILS_LOG_ONCE_KEY_fatal(_CATEGORY, _KEY, _MESSAGE, ...)                                                 \
    do {                                                                                                               \
        if (::pP::Log::Once::claim(_KEY))                                                                              \
            PPR_LOG(_CATEGORY, fatal, (_MESSAGE), ##__VA_ARGS__);                                                      \
    } while (0)
#else
#define PPR_DETAILS_LOG_ONCE_fatal(_CATEGORY, _MESSAGE, ...) (void) 0
#define PPR_DETAILS_LOG_ONCE_KEY_fatal(_CATEGORY, _KEY, _MESSAGE, ...) (void) 0
#endif

// First-occurrence gate. Compiled-in severities keep the existing
// claim-before-runtime-filter ordering; compiled-out severities erase the
// claim (mutex, set lookup) together with the log and its arguments.
#define PPR_LOG_ONCE(_CATEGORY, _LEVEL, _MESSAGE, ...)                                                                 \
    PPR_CONCAT(PPR_DETAILS_LOG_ONCE_, _LEVEL)(_CATEGORY, (_MESSAGE), ##__VA_ARGS__)

#define PPR_LOG_ONCE_KEY(_CATEGORY, _LEVEL, _KEY, _MESSAGE, ...)                                                       \
    PPR_CONCAT(PPR_DETAILS_LOG_ONCE_KEY_, _LEVEL)(_CATEGORY, (_KEY), (_MESSAGE), ##__VA_ARGS__)

#define PPR_FLUSH_LOG(...) std::ignore = pP::Log::flush(__VA_ARGS__)

#else
#define PPR_DECLARE_LOG_CATEGORY(_NAME)
#define PPR_DEFINE_LOG_CATEGORY(_NAME, _VERBOSITY, _FLAGS)
#define PPR_LOG(_CATEGORY, _LEVEL, _MESSAGE, ...) (void) 0
#define PPR_LOG_RAW(_CATEGORY, _LEVEL, _MESSAGE, ...) (void) 0
#define PPR_LOG_ONCE(_CATEGORY, _LEVEL, _MESSAGE, ...) (void) 0
#define PPR_LOG_ONCE_KEY(_CATEGORY, _LEVEL, _KEY, _MESSAGE, ...) (void) 0
#define PPR_FLUSH_LOG(...) (void) 0
#endif

// ------------------------------------------------------------------
// error handling -- return-on-failure with logging
// works with std::error_code and rhi::Result via pP::failed()
// ------------------------------------------------------------------

#if PPR_ENABLE_LOGGING
#define PPR_THROW(_CATEGORY, ...)                                                                                      \
    do {                                                                                                               \
        static_assert(std::derived_from<std::remove_cvref_t<decltype((__VA_ARGS__))>, std::exception>);                \
        try {                                                                                                          \
            throw __VA_ARGS__;                                                                                         \
        } catch (const std::exception &PPR_ANONYMIZE(_ppr_e)) {                                                        \
            PPR_LOG(                                                                                                   \
                _CATEGORY, error, "throw exception",                                                                   \
                {{"what", PPR_ANONYMIZE(_ppr_e).what()}, {"type", typeid(PPR_ANONYMIZE(_ppr_e)).name()}});             \
            throw;                                                                                                     \
        }                                                                                                              \
    } while (0)

#define PPR_RETHROW(_CATEGORY)                                                                                         \
    do {                                                                                                               \
        try {                                                                                                          \
            throw;                                                                                                     \
        } catch (const std::exception &PPR_ANONYMIZE(_ppr_e)) {                                                        \
            PPR_LOG(                                                                                                   \
                _CATEGORY, error, "rethrow exception",                                                                 \
                {{"what", PPR_ANONYMIZE(_ppr_e).what()}, {"type", typeid(PPR_ANONYMIZE(_ppr_e)).name()}});             \
            throw;                                                                                                     \
        } catch (...) {                                                                                                \
            PPR_LOG(_CATEGORY, error, "rethrow foreign exception");                                                    \
            throw;                                                                                                     \
        }                                                                                                              \
    } while (0)
#else
#define PPR_THROW(_CATEGORY, ...) throw __VA_ARGS__
#define PPR_RETHROW(_CATEGORY) throw
#endif

#define PPR_RETURN_ON_FAIL(_CATEGORY, ...)                                                                             \
    if (auto const PPR_ANONYMIZE(_ppr_result) = (__VA_ARGS__); hasFailed(PPR_ANONYMIZE(_ppr_result))) [[unlikely]] {   \
        PPR_LOG(_CATEGORY, error, "FAILED: " PPR_STRINGIZE(__VA_ARGS__), pP::opaque::Dict{});                          \
        return PPR_ANONYMIZE(_ppr_result);                                                                             \
    }

#define PPR_RETURN_ERROR_ON_FAIL(_CATEGORY, ...)                                                                       \
    if (auto const PPR_ANONYMIZE(_errc) = make_error_code(__VA_ARGS__); PPR_ANONYMIZE(_errc)) [[unlikely]] {           \
        PPR_LOG(                                                                                                       \
            _CATEGORY, error, "FAILED: " PPR_STRINGIZE(__VA_ARGS__),                                                   \
            pP::opaque::Dict{                                                                                          \
                {"category", PPR_ANONYMIZE(_errc).category().name()}, {"value", PPR_ANONYMIZE(_errc).value()},         \
                {"message", PPR_ANONYMIZE(_errc).message()}});                                                         \
        return PPR_ANONYMIZE(_errc);                                                                                   \
    }

#define PPR_RETAIN_ERROR_ON_FAIL(_CATEGORY, _RETAINED_ERRC, ...)                                                       \
    if (auto const PPR_ANONYMIZE(_errc) = make_error_code(__VA_ARGS__); PPR_ANONYMIZE(_errc)) [[unlikely]] {           \
        PPR_LOG(                                                                                                       \
            _CATEGORY, error, "FAILED: " PPR_STRINGIZE(__VA_ARGS__),                                                   \
            pP::opaque::Dict{                                                                                          \
                {"category", PPR_ANONYMIZE(_errc).category().name()}, {"value", PPR_ANONYMIZE(_errc).value()},         \
                {"message", PPR_ANONYMIZE(_errc).message()}});                                                         \
        if (not (_RETAINED_ERRC)) {                                                                                    \
            _RETAINED_ERRC = PPR_ANONYMIZE(_errc);                                                                     \
        }                                                                                                              \
    }

#define PPR_RETURN_UNEXPECTED_ON_FAIL(_CATEGORY, ...)                                                                  \
    if (auto const PPR_ANONYMIZE(_errc) = make_error_code(__VA_ARGS__); PPR_ANONYMIZE(_errc)) [[unlikely]] {           \
        PPR_LOG(                                                                                                       \
            _CATEGORY, error, "FAILED: " PPR_STRINGIZE(__VA_ARGS__),                                                   \
            pP::opaque::Dict{                                                                                          \
                {"category", PPR_ANONYMIZE(_errc).category().name()}, {"value", PPR_ANONYMIZE(_errc).value()},         \
                {"message", PPR_ANONYMIZE(_errc).message()}});                                                         \
        return std::unexpected{PPR_ANONYMIZE(_errc)};                                                                  \
    }

#define PPR_LOG_WARNING_ON_FAIL(_CATEGORY, ...)                                                                        \
    if (auto const PPR_ANONYMIZE(_errc) = make_error_code(__VA_ARGS__); PPR_ANONYMIZE(_errc)) [[unlikely]] {           \
        PPR_LOG(                                                                                                       \
            _CATEGORY, warning, "FAILED: " PPR_STRINGIZE(__VA_ARGS__),                                                 \
            pP::opaque::Dict{                                                                                          \
                {"category", PPR_ANONYMIZE(_errc).category().name()}, {"value", PPR_ANONYMIZE(_errc).value()},         \
                {"message", PPR_ANONYMIZE(_errc).message()}});                                                         \
    }
