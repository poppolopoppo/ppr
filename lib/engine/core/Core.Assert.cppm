module;
#include "pP/Macros.h"
export module engine.core:assert;

import :function_ref;
import :hal;

import std;

namespace pP {
#if PPR_ENABLE_ASSERTIONS
    export class Assertion {
    public:
        enum EType {
            assume,
            crash,
            ensure,
            require,
            verify,
        };

        static constexpr std::string_view typeName(const EType type) noexcept {
            switch (type) {
                case assume: return "assume";
                case crash: return "crash";
                case ensure: return "ensure";
                case require: return "require";
                case verify: return "verify";
            }
            std::unreachable();
        }

        std::source_location m_site;
        const char *m_message;
        EType m_type;

        using Policy = std23::function_ref<void(const Assertion &assert)>;

        template<std::size_t nLenP1>
        static void onFailure(const EType type, const char (&message)[nLenP1], const std::source_location &site) {
            Handler::get().onAssertFailure(Assertion{
                .m_site = site,
                .m_message = message,
                .m_type = type
            });
        }

        static Policy setFailurePolicy(Policy &&on_assert_failure) noexcept {
            return Handler::get().setFailurePolicy(std::move(on_assert_failure));
        }

    private:
        class Handler {
            static void defaultAssertFailure_(const Assertion &condition) {
                char buffer[2048];
                const auto [out, size] = std::format_to_n(
                    buffer, sizeof(buffer) - 1,
                    "{}({}): {} assert failed: \"{}\"\n"
                    "\tin function: {}\n",
                    condition.m_site.file_name(),
                    condition.m_site.line(),
                    typeName(condition.m_type),
                    condition.m_message,
                    condition.m_site.function_name());
                // adds terminator to the buffer
                *out = zero_v;

                hal::outputDebug(buffer);
                hal::breakpointIfDebugging();

                // change this value with the debugger to survive the assertion
                static volatile bool g_throw_exception = true;
                if (g_throw_exception) {
                    throw std::logic_error(buffer);
                }
            }

            std::mutex m_barrier{};
            Policy m_on_assert_failure{&defaultAssertFailure_};

        public:
            Handler() noexcept = default;

            Handler(const Handler &) = delete;

            Handler &operator =(const Handler &) = delete;

            Handler(Handler &&) = delete;

            Handler &operator =(Handler &&) = delete;

            [[nodiscard]] static Handler &get() noexcept {
                alignas(hal::cacheline_size_v) static Handler g_handler;
                return g_handler;
            }

            // returns previous policy
            Policy setFailurePolicy(Policy &&on_assert_failure) noexcept {
                const std::lock_guard guard(m_barrier);
                std::swap(on_assert_failure, m_on_assert_failure);
                return on_assert_failure;
            }

            void onAssertFailure(const Assertion &condition) {
                const std::lock_guard guard(m_barrier);
                return m_on_assert_failure(condition);
            }
        };
    };
#endif

    // ------------------------------------------------------------------
    // checked_cast — safe narrowing / down-casting with assertion guards
    // ------------------------------------------------------------------

    // Integer narrowing/widening cast
    // Asserts value survives the round-trip AND is non-negative when
    // crossing the signed→unsigned boundary.
    template<std::integral ToT, std::integral FromT>
        requires std::is_convertible_v<FromT, ToT>
    [[nodiscard]] constexpr ToT checked_cast(FromT value) noexcept {
        // Reject negative values being cast to unsigned types
        if constexpr (std::is_signed_v<FromT> && std::is_unsigned_v<ToT>)
            PPR_ASSERT(value >= 0);

        const ToT result = static_cast<ToT>(value);

        // Reject truncation (catches unsigned→signed overflow too)
        PPR_ASSERT(static_cast<FromT>(result) == value);

        return result;
    }

    // Downcast pointer: Base* → Derived*
    // Null propagates safely; non-null input asserts type correctness.
    template<typename ToT, typename FromT>
        requires std::is_base_of_v<FromT, ToT>
    [[nodiscard]] ToT *checked_cast(FromT *value) noexcept {
#if PPR_ENABLE_ASSERTIONS
        if (value != nullptr) {
            const ToT *result = dynamic_cast<ToT *>(value);
            PPR_ASSERT(result != nullptr); // type mismatch
            return const_cast<ToT *>(result);
        }
        return nullptr;
#else
        return static_cast<ToT *>(value);
#endif
    }

    // Downcast reference: Base& → Derived&
    // References cannot be null — asserts type correctness unconditionally.
    template<typename ToT, typename FromT>
        requires std::is_base_of_v<FromT, ToT>
    [[nodiscard]] ToT &checked_cast(FromT &value) noexcept {
#if PPR_ENABLE_ASSERTIONS
        PPR_ASSERT(dynamic_cast<ToT*>(std::addressof(value)) != nullptr);
#endif
        return static_cast<ToT &>(value);
    }

    template<std::integral IntT>
    struct safe_narrowing {
        IntT m_value{};

        PPR_FORCE_INLINE explicit constexpr safe_narrowing(const IntT value) noexcept
            : m_value(value) {
        }

        // ReSharper disable once CppNonExplicitConversionOperator
        [[nodiscard]] PPR_FORCE_INLINE constexpr operator IntT() const noexcept {
            return m_value;
        }

        template<std::integral OtherIntT> requires (not std::is_same_v<IntT, OtherIntT>)
        // ReSharper disable once CppNonExplicitConversionOperator
        [[nodiscard]] PPR_FORCE_INLINE constexpr operator OtherIntT() const noexcept {
            return checked_cast<OtherIntT>(m_value);
        }
    };

    template<std::integral IntT>
    safe_narrowing(IntT) -> safe_narrowing<IntT>;
}
