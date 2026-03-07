#pragma once

// ------------------------------------------------------------------
// macro helpers
// ------------------------------------------------------------------

#define PPR_EXPAND(x) x
#define PPR_EXPAND_VA(X, ...)  X, ##__VA_ARGS__

#define PPR_COMMA_3 ,
#define PPR_COMMA_2 PPR_EXPAND( PPR_COMMA_3 )
#define PPR_COMMA_1 PPR_EXPAND( PPR_COMMA_2 )
#define PPR_COMMA PPR_COMMA_1
#define PPR_COMMA_PROTECT(...) __VA_ARGS__

#define PPR_STRINGIZE_2(...) # __VA_ARGS__
#define PPR_STRINGIZE_1(...) PPR_EXPAND( PPR_STRINGIZE_2(__VA_ARGS__) )
#define PPR_STRINGIZE_0(...) PPR_EXPAND( PPR_STRINGIZE_1(__VA_ARGS__) )
#define PPR_STRINGIZE(...) PPR_EXPAND( PPR_STRINGIZE_0(__VA_ARGS__) )

#define PPR_CONCAT_I(_X, _Y) _X ## _Y
#define PPR_CONCAT_OO(_ARGS) PPR_CONCAT_I ## _ARGS
#define PPR_CONCAT(_X, _Y) PPR_CONCAT_I(_X, _Y)
#define PPR_CONCAT3(_X, _Y, _Z) PPR_CONCAT(_X, PPR_CONCAT(_Y, _Z))

// ------------------------------------------------------------------
// compiler specific attributes
// ------------------------------------------------------------------

#ifdef _MSC_VER
#   define PPR_ATTRIBUTE_CODE_SEGMENT(_NAME) __declspec(code_seg(_NAME))
#   define PPR_ASSUME(...) __assume(__VA_ARGS__)
#   define PPR_EMPTY_BASES __declspec(empty_bases)
#   define PPR_FLATTEN [[msvc::flatten]]
#   define PPR_FORCE_INLINE [[msvc::forceinline]]
#   define PPR_LIFETIME_BOUND [[msvc::lifetimebound]]
#   define PPR_NO_INLINE [[msvc::noinline]]
#   define PPR_PRAGMA_WARNING_PUSH() __pragma(warning(push))
#   define PPR_PRAGMA_WARNING_DISABLE_MSVC(_WARNING_CODE) __pragma(warning(disable: _WARNING_CODE))
#   define PPR_PRAGMA_WARNING_DISABLE_GCC_CLANG(_WARNING_ID)
#   define PPR_PRAGMA_WARNING_POP() __pragma(warning(pop))
#elif defined(__clang__) || defined(__GNUC__)
#   define PPR_ATTRIBUTE_CODE_SEGMENT(_NAME) __attribute((code_seg(_NAME)))
#   define PPR_ASSUME(...) __builtin_assume(__VA_ARGS__)
#   define PPR_EMPTY_BASES
#   define PPR_FLATTEN [[gnu::flatten]]
#   define PPR_FORCE_INLINE [[gnu::always_inline]] inline
#if defined(__clang__)
#   define PPR_LIFETIME_BOUND [[clang::lifetimebound]]
#   define PPR_PRAGMA_WARNING_PUSH() __pragma(clang diagnostic push)
#   define PPR_PRAGMA_WARNING_DISABLE_GCC_CLANG(_WARNING_ID) __pragma(clang diagnostic ignored #_WARNING_ID)
#   define PPR_PRAGMA_WARNING_POP() __pragma(clang diagnostic pop)
#else
#   define PPR_LIFETIME_BOUND [[gcc::lifetimebound]]
#   define PPR_PRAGMA_WARNING_PUSH() __pragma(gcc diagnostic push)
#   define PPR_PRAGMA_WARNING_DISABLE_GCC_CLANG(_WARNING_ID) __pragma(gcc diagnostic ignored #_WARNING_ID)
#   define PPR_PRAGMA_WARNING_POP() __pragma(gcc diagnostic pop)
#endif
#   define PPR_NO_INLINE [[gnu::noinline]]
#   define PPR_PRAGMA_WARNING_DISABLE_MSVC(_WARNING_CODE)
#else
#   define PPR_ATTRIBUTE_CODE_SEGMENT(_NAME)
#   define PPR_ASSUME(...)
#   define PPR_EMPTY_BASES
#   define PPR_FLATTEN
#   define PPR_FORCE_INLINE
#   define PPR_LIFETIME_BOUND
#   define PPR_NO_INLINE
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
#       define TEXT(quote) L##quote
#   else
#       define TEXT(quote) quote
#   endif
#endif

#define PPR_LITERAL_FOR(_CharT, _Text) \
    ([] [[nodiscard]] () consteval noexcept -> decltype(auto) { \
        if constexpr (std::is_same_v<_CharT, char>) return _Text; \
        else if constexpr (std::is_same_v<_CharT, wchar_t>) return L##_Text; \
        else if constexpr (std::is_same_v<_CharT, char8_t>) return u8##_Text; \
        else std::unreachable(); \
    }())

// ------------------------------------------------------------------
// RAII helpers
// ------------------------------------------------------------------

#define PPR_ANONYMIZE(_X) PPR_CONCAT(_X, __LINE__)

#define PPR_DEFER const pP::Deferred PPR_ANONYMIZE(deferred_) = [&]() -> void

// ------------------------------------------------------------------
// assertions
// ------------------------------------------------------------------

#ifdef _DEBUG
#   ifdef NDEBUG
#       error "NDEBUG should not be defined in debug builds"
#   endif

#   define PPR_ENABLE_ASSERTIONS 1
#   define PPR_DECL_IF_DEBUG(...) __VA_ARGS__
#   define PPR_EXPR_IF_DEBUG(...) __VA_ARGS__
#else
#   define PPR_ENABLE_ASSERTIONS 0
#   define PPR_DECL_IF_DEBUG(...)
#   define PPR_EXPR_IF_DEBUG(...) (void)0
#endif

#if PPR_ENABLE_ASSERTIONS
#   define PPR_DETAILS_ASSERTION_IMPL(_TYPE, ...)  do { \
        if consteval { \
            [[maybe_unused]] const auto PPR_ANONYMIZE(condition) = (__VA_ARGS__); \
            PPR_ASSUME(PPR_ANONYMIZE(condition)); \
        } else { \
            if (!(__VA_ARGS__)) [[unlikely]] [PPR_ANONYMIZE(assertion_site){std::source_location::current()}]() \
                PPR_ATTRIBUTE_CODE_SEGMENT(".ppr_dbg") { \
                ::pP::Assertion::onFailure( \
                    ::pP::Assertion::_TYPE, \
                    PPR_STRINGIZE(__VA_ARGS__), \
                    PPR_ANONYMIZE(assertion_site) ); \
            }(); \
        } \
    } while (0)

#   define PPR_ASSERT(...) PPR_DETAILS_ASSERTION_IMPL(require, __VA_ARGS__)
#   define PPR_VERIFY(...) PPR_DETAILS_ASSERTION_IMPL(verify, __VA_ARGS__)

#   define PPR_ENSURE(...) ((__VA_ARGS__) ? true : ( \
    []() PPR_ATTRIBUTE_CODE_SEGMENT(".ppr_dbg") { \
        pP::Assertion::onFailure( \
            pP::Assertion::ensure, \
            PPR_STRINGIZE(__VA_ARGS__), \
            std::source_location::current() ); \
    }(), false ))

#else
#   define PPR_ASSERT(...) PPR_ASSUME(__VA_ARGS__)
#   define PPR_ENSURE(...) (PPR_ASSUME(__VA_ARGS__), __VA_ARGS__)
#   define PPR_VERIFY(...) do { \
        [[maybe_unused]] const auto PPR_ANONYMIZE(condition) = (__VA_ARGS__); \
        PPR_ASSUME(PPR_ANONYMIZE(condition)); \
    } while (0)
#endif

// ------------------------------------------------------------------
// unit tests
// ------------------------------------------------------------------

#define PPR_ENABLE_UNIT_TESTS 1

#define PPR_UNIT_TEST(_NAME, ...) \
    inline constexpr auto _NAME = pP::UnitTest::Named(PPR_STRINGIZE(_NAME), {__VA_ARGS__}) / \
        []([[maybe_unused]] pP::UnitTest::IRun &_) -> void
