#pragma once

// ------------------------------------------------------------------
// detects debug builds conservatively
// ------------------------------------------------------------------

#if defined(_DEBUG) || !defined(NDEBUG)
#   define PPR_ENABLE_DEBUG 1
#   define PPR_DECL_IF_DEBUG(...) __VA_ARGS__
#   define PPR_EXPR_IF_DEBUG(...) __VA_ARGS__
#else
#   define PPR_ENABLE_DEBUG 0
#   define PPR_DECL_IF_DEBUG(...)
#   define PPR_EXPR_IF_DEBUG(...) (void)0
#endif

// ------------------------------------------------------------------
// memory poisoning is enabled when ASAN or debug builds are active
// ------------------------------------------------------------------

#if defined(PPR_ENABLE_SANITIZER_ADDRESS) || PPR_ENABLE_DEBUG
#   define PPR_ENABLE_MEMORY_POISONING      1
#   define PPR_ENABLE_SAFE_OBJECT_TRACKING  1
#else
#   define PPR_ENABLE_MEMORY_POISONING      0
#   define PPR_ENABLE_SAFE_OBJECT_TRACKING  0
#endif

// ------------------------------------------------------------------
// detects 32-bits vs 64-bits builds, note that 32-bits are deprecated.
// ------------------------------------------------------------------

#if INTPTR_MAX == INT64_MAX
#   define PPR_64BIT 1
#   define PPR_32BIT 0
#   define PPR_32BIT_OR_64BIT(_32BIT, _64BIT) _64BIT
#elif INTPTR_MAX == INT32_MAX
#   define PPR_64BIT 0
#   define PPR_32BIT 1
#   define PPR_32BIT_OR_64BIT(_32BIT, _64BIT) _32BIT
#else
#   error "Unknown pointer size: cannot determine 32-bit vs 64-bit build"
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
#   pragma intrinsic(_ReadWriteBarrier)

#   define PPR_ATTRIBUTE_CODE_SEGMENT(_NAME) __declspec(code_seg(_NAME))
#   define PPR_ASSUME(...) [[assume(__VA_ARGS__)]]
#   define PPR_COMPILER_READWRITE_BARRIER() ::_ReadWriteBarrier()
#   define PPR_EMPTY_BASES __declspec(empty_bases)
#   define PPR_FLATTEN [[msvc::flatten]]
#   define PPR_FORCE_INLINE [[msvc::forceinline]]
#   define PPR_LIFETIME_BOUND [[msvc::lifetimebound]]
#   define PPR_NO_INLINE [[msvc::noinline]]
#   define PPR_OFFSETOF(_STRUCT, _MEMBER) __builtin_offsetof(_STRUCT, _MEMBER)
#   define PPR_PRAGMA_WARNING_PUSH() __pragma(warning(push))
#   define PPR_PRAGMA_WARNING_DISABLE_MSVC(_WARNING_CODE) __pragma(warning(disable : _WARNING_CODE))
#   define PPR_PRAGMA_WARNING_DISABLE_GCC_CLANG(_WARNING_ID)
#   define PPR_PRAGMA_WARNING_POP() __pragma(warning(pop))
#elif defined(__clang__) || defined(__GNUC__)
#   define PPR_ATTRIBUTE_CODE_SEGMENT(_NAME) __attribute((code_seg(_NAME)))
#   define PPR_ASSUME(...) __builtin_assume(__VA_ARGS__)
#   define PPR_COMPILER_READWRITE_BARRIER() asm volatile("" ::: "memory")
#   define PPR_EMPTY_BASES
#   define PPR_FLATTEN [[gnu::flatten]]
#   define PPR_FORCE_INLINE [[gnu::always_inline]] inline
#   if defined(__clang__)
#      define PPR_LIFETIME_BOUND [[clang::lifetimebound]]
#      define PPR_PRAGMA_WARNING_PUSH() __pragma(clang diagnostic push)
#      define PPR_PRAGMA_WARNING_DISABLE_GCC_CLANG(_WARNING_ID) __pragma(clang diagnostic ignored #_WARNING_ID)
#      define PPR_PRAGMA_WARNING_POP() __pragma(clang diagnostic pop)
#   else
#      define PPR_LIFETIME_BOUND [[gcc::lifetimebound]]
#      define PPR_PRAGMA_WARNING_PUSH() __pragma(gcc diagnostic push)
#      define PPR_PRAGMA_WARNING_DISABLE_GCC_CLANG(_WARNING_ID) __pragma(gcc diagnostic ignored #_WARNING_ID)
#      define PPR_PRAGMA_WARNING_POP() __pragma(gcc diagnostic pop)
#   endif
#   define PPR_NO_INLINE [[gnu::noinline]]
#   define PPR_OFFSETOF(_STRUCT, _MEMBER) __builtin_offsetof(_STRUCT, _MEMBER)
#   define PPR_PRAGMA_WARNING_DISABLE_MSVC(_WARNING_CODE)
#else
#   define PPR_ATTRIBUTE_CODE_SEGMENT(_NAME)
#   define PPR_ASSUME(...) ((void)0)
#   define PPR_COMPILER_READWRITE_BARRIER() ((void)0)
#   define PPR_EMPTY_BASES
#   define PPR_FLATTEN
#   define PPR_FORCE_INLINE
#   define PPR_LIFETIME_BOUND
#   define PPR_NO_INLINE
#   define PPR_OFFSETOF(_STRUCT, _MEMBER) ((::size_t)&reinterpret_cast<char const volatile&>((((_STRUCT*)0)->_MEMBER)))
#   define PPR_PRAGMA_WARNING_PUSH()
#   define PPR_PRAGMA_WARNING_DISABLE_MSVC(_WARNING_CODE)
#   define PPR_PRAGMA_WARNING_DISABLE_GCC_CLANG(_WARNING_ID)
#   define PPR_PRAGMA_WARNING_POP()
#endif

// ------------------------------------------------------------------
// wide char handling
// ------------------------------------------------------------------

#ifndef TEXT
#   if defined(_MSC_VER)
#      define TEXT(quote) L##quote
#   else
#      define TEXT(quote) quote
#   endif
#endif

#define PPR_LITERAL_FOR(_CharT, _Text)                          \
    ([] [[nodiscard]] () consteval noexcept -> decltype(auto) { \
        if constexpr (std::is_same_v<_CharT, char>)             \
            return _Text;                                       \
        else if constexpr (std::is_same_v<_CharT, wchar_t>)     \
            return L##_Text;                                    \
        else if constexpr (std::is_same_v<_CharT, char8_t>)     \
            return u8##_Text;                                   \
        else                                                    \
            std::unreachable();                                 \
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
#   define PPR_DETAILS_ASSERTION_IMPL(_TYPE, ...)                                                                                    \
       do {                                                                                                                          \
           const bool PPR_ANONYMIZE(predicate) = (__VA_ARGS__);                                                                      \
           if consteval {                                                                                                            \
               PPR_ASSUME(PPR_ANONYMIZE(predicate));                                                                                 \
           } else {                                                                                                                  \
               static constexpr auto PPR_ANONYMIZE(assertion_site) = std::source_location::current();                                \
                                                                                                                                     \
               if (not PPR_ANONYMIZE(predicate)) [[unlikely]]                                                                        \
                   [&]() PPR_ATTRIBUTE_CODE_SEGMENT(".ppr_dbg") {                                                                    \
                      ::pP::Assertion::onFailure(::pP::Assertion::_TYPE, PPR_STRINGIZE(__VA_ARGS__), PPR_ANONYMIZE(assertion_site)); \
                   }();                                                                                                              \
           }                                                                                                                         \
       } while (0)

#   define PPR_ASSERT(...) PPR_DETAILS_ASSERTION_IMPL(require, __VA_ARGS__)
#   define PPR_VERIFY(...) PPR_DETAILS_ASSERTION_IMPL(verify, __VA_ARGS__)

#   define PPR_ENSURE(...) \
    ((__VA_ARGS__) ? true  : ( \
        []() PPR_ATTRIBUTE_CODE_SEGMENT(".ppr_dbg") {                                                                    \
           pP::Assertion::onFailure(pP::Assertion::ensure, PPR_STRINGIZE(__VA_ARGS__), std::source_location::current()); \
        }(),                                                                                                             \
        false))

#else
#   define PPR_ASSERT(...) PPR_ASSUME(__VA_ARGS__)
#   define PPR_ENSURE(...) (PPR_ASSUME(__VA_ARGS__), __VA_ARGS__)
#   define PPR_VERIFY(...)                                                       \
       do {                                                                      \
           [[maybe_unused]] const auto PPR_ANONYMIZE(condition) = (__VA_ARGS__); \
           PPR_ASSUME(PPR_ANONYMIZE(condition));                                 \
       } while (0)
#endif

// ------------------------------------------------------------------
// logging
// ------------------------------------------------------------------

#define PPR_ENABLE_LOGGING 1

#if PPR_ENABLE_LOGGING
#   define PPR_DECLARE_LOG_CATEGORY(_NAME)                    \
       namespace details::log {                               \
           [[nodiscard]] pP::Log::Category &_NAME() noexcept; \
       }

#   define PPR_DEFINE_LOG_CATEGORY(_NAME, _VERBOSITY, _FLAGS)  \
       namespace details::log {                                \
           [[nodiscard]] pP::Log::Category &_NAME() noexcept { \
               static pP::Log::Category g_instance{            \
                   PPR_STRINGIZE(_NAME),                       \
                   (pP::Log::ELevel::_VERBOSITY),              \
                   (pP::Log::Category::EFlags::_FLAGS),        \
               };                                              \
               return g_instance;                              \
           }                                                   \
       }

#   define PPR_LOG(_CATEGORY, _LEVEL, _MESSAGE, ...) \
       pP::Log::log(pP::Log::Emitter(details::log::_CATEGORY(), pP::Log::ELevel::_LEVEL), (_MESSAGE), __VA_ARGS__)

#   define PPR_LOG_RAW(_CATEGORY, _LEVEL, _MESSAGE, ...) \
        pP::Log::logRaw(pP::Log::Emitter(details::log::_CATEGORY(), pP::Log::ELevel::_LEVEL), (_MESSAGE), __VA_ARGS__)

#   define PPR_FLUSH_LOG() pP::Log::flush()

#else
#   define PPR_DECLARE_LOG_CATEGORY(_NAME)
#   define PPR_DEFINE_LOG_CATEGORY(_NAME, _VERBOSITY, _FLAGS)
#   define PPR_LOG(_CATEGORY, _LEVEL, _MESSAGE, ...) (void)0
#   define PPR_FLUSH_LOG() (void)0
#endif

// ------------------------------------------------------------------
// error handling -- return-on-failure with logging
// works with std::error_code and rhi::Result via pP::failed()
// ------------------------------------------------------------------

#define PPR_RETURN_ON_FAIL(_CATEGORY, ...)                                  \
    if (auto const PPR_ANONYMIZE(_ppr_result) = (__VA_ARGS__);              \
        hasFailed(PPR_ANONYMIZE(_ppr_result))) [[unlikely]] {               \
        PPR_LOG(_CATEGORY, error,                                           \
            "FAILED: " PPR_STRINGIZE(__VA_ARGS__), {                        \
        });                                                                 \
        return PPR_ANONYMIZE(_ppr_result);                                  \
    }

#define PPR_RETURN_ERROR_ON_FAIL(_CATEGORY, ...)                            \
    if (auto const PPR_ANONYMIZE(_errc) = make_error_code(__VA_ARGS__);     \
        hasFailed(PPR_ANONYMIZE(_errc))) [[unlikely]] {                     \
        PPR_LOG(_CATEGORY, error,                                           \
            "FAILED: " PPR_STRINGIZE(__VA_ARGS__), {                        \
            {"category", PPR_ANONYMIZE(_errc).category().name()},           \
            {"value", PPR_ANONYMIZE(_errc).value()},                        \
            {"message", PPR_ANONYMIZE(_errc).message()}                     \
        });                                                                 \
        return PPR_ANONYMIZE(_errc);                                        \
    }

#define PPR_RETURN_UNEXPECTED_ON_FAIL(_CATEGORY, ...)                       \
    if (auto const PPR_ANONYMIZE(_errc) = make_error_code(__VA_ARGS__);     \
        hasFailed(PPR_ANONYMIZE(_errc))) [[unlikely]] {                     \
        PPR_LOG(_CATEGORY, error,                                           \
            "FAILED: " PPR_STRINGIZE(__VA_ARGS__), {                        \
            {"category", PPR_ANONYMIZE(_errc).category().name()},           \
            {"value", PPR_ANONYMIZE(_errc).value()},                        \
            {"message", PPR_ANONYMIZE(_errc).message()}                     \
        });                                                                 \
        return std::unexpected{PPR_ANONYMIZE(_errc)};                       \
    }

#define RHI_RETURN_ERROR_ON_FAIL(_CATEGORY, ...)                              \
    PPR_RETURN_ERROR_ON_FAIL(_CATEGORY, pP::rhi::result(__VA_ARGS__))

// ------------------------------------------------------------------
// unit tests
// ------------------------------------------------------------------

#define PPR_ENABLE_UNIT_TESTS 1

#define PPR_UNIT_TEST(_NAME, ...) \
    inline constexpr auto _NAME = \
        pP::UnitTest::Named(PPR_STRINGIZE(_NAME), {__VA_ARGS__}) / \
            []([[maybe_unused]] pP::UnitTest::IRun & _) -> void
