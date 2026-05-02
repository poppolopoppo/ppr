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

        static Policy &&setFailurePolicy(Policy &&on_assert_failure) noexcept {
            return Handler::get().setFailurePolicy(std::move(on_assert_failure));
        }

    private:
        class Handler {
            Handler() noexcept = default;

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

                // change this value with the debugger to survive the assert
                static volatile bool g_throw_exception = true;
                if (g_throw_exception) {
                    throw std::logic_error(buffer);
                }
            }

            std::mutex m_barrier{};
            Policy m_on_assert_failure{&defaultAssertFailure_};

        public:
            Handler(const Handler &) = delete;

            Handler &operator =(const Handler &) = delete;

            Handler(Handler &&) = delete;

            Handler &operator =(Handler &&) = delete;

            [[nodiscard]] static Handler &get() noexcept {
                alignas(hal::cacheline_size_v) static Handler g_handler;
                return g_handler;
            }

            // returns previous policy
            Policy &&setFailurePolicy(Policy &&on_assert_failure) noexcept {
                const std::lock_guard guard(m_barrier);
                std::swap(on_assert_failure, m_on_assert_failure);
                return std::move(on_assert_failure);
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

    // ------------------------------------------------------------------
    // Unit test helper
    // ------------------------------------------------------------------

    export class UnitTest {
    protected:
        struct RunImpl;

    public:
        enum EFlags : u32 {
            none = 0,
            expect_fail = 1 << 0,
        };

        enum EStatus : u8 {
            fail = 0,
            pass = 1,
        };

        struct Id {
            const RunImpl *m_run{nullptr};

            explicit constexpr Id(const RunImpl &run) noexcept
                : m_run(&run) {
            }

            template<details::TChar CharT, typename OutT>
            auto format(OutT &outp) const -> OutT {
                if (m_run->m_parent) {
                    outp = Id(*m_run->m_parent).format<CharT>(outp);
                    *outp++ = CharT('/');
                }
                const std::string_view name{m_run->m_test.m_name};
                return std::format_to(outp, "{}", name);
            }
        };

        struct [[nodiscard]] IRun {
        protected:
            ~IRun() = default;

        public:
            [[nodiscard]] virtual Id getTestId() const noexcept = 0;

            virtual void log(const char *msg) = 0;

            virtual void failWith(const char *msg) = 0;

            virtual void recurse(const UnitTest &test) = 0;

            virtual void success() = 0;

            template<typename... ArgsT>
            void logFmt(const std::format_string<ArgsT...> &fmt, ArgsT &&... args) noexcept {
                char buffer[2048];
                const auto [out, size] = std::format_to_n(buffer, std::size(buffer) - 1, fmt, std::forward<ArgsT>(args)...);
                *out = char{0};
                log(buffer);
            }

            template<typename... ArgsT>
            void failFmt(const std::format_string<ArgsT...> &fmt, ArgsT &&... args) noexcept {
                char buffer[2048];
                const auto [out, size] = std::format_to_n(buffer, std::size(buffer) - 1, fmt, std::forward<ArgsT>(args)...);
                *out = char{0};
                failWith(buffer);
            }
        };

        using test_func_t = void (*)(IRun &run);

        consteval UnitTest(const char *const name, test_func_t test, const EFlags flags = none) noexcept
            : m_name(name), m_run(test), m_flags(flags) {
        }

        void run(IRun &run) const noexcept {
            try {
                checked_cast<RunImpl>(&run)->start();
                m_run(run);
                run.success();
            } catch (std::exception &e) {
                run.failWith(e.what());
            }
        }

        static void run(const UnitTest &test) noexcept {
            RunImpl first_run{test};
            test.run(first_run);
        }

        struct Named {
            const char *m_name{nullptr};
            EFlags m_flags{none};

            template<std::size_t nLen>
            explicit consteval Named(
                const char (&name)[nLen],
                const std::initializer_list<EFlags> flags = {}) noexcept
                : m_name(name), m_flags(aggregateFlags(flags)) {
            }

            template<std::convertible_to<test_func_t> CallbackT>
            consteval UnitTest operator /(CallbackT &&callback) const noexcept {
                return UnitTest(m_name, callback, m_flags);
            }
        };

        static constexpr EFlags aggregateFlags(std::initializer_list<EFlags> flags) noexcept {
            u32 result = 0u;
            for (const auto flag: flags) {
                result |= flag;
            }
            return static_cast<EFlags>(result);
        }

    protected:
        const char *m_name{nullptr};
        test_func_t m_run{nullptr};
        EFlags m_flags{none};

        struct RunImpl final : IRun {
            const UnitTest &m_test;
            RunImpl *m_parent{nullptr};

#if PPR_ENABLE_ASSERTIONS
            std::optional<Assertion::Policy> m_prev_assert_policy;
#endif

            const char *m_failure{"unknown"};

            std::chrono::steady_clock::time_point m_start_time;

            u32 m_num_passed{0u};
            u32 m_num_failed{0u};

            u32 m_depth{0u};

            EStatus m_status{pass};

            explicit RunImpl(const UnitTest &test) noexcept
                : m_test(test) {
            }

            explicit RunImpl(const UnitTest &test, RunImpl &parent) noexcept
                : m_test(test), m_parent(&parent), m_depth(parent.m_depth + 1u) {
            }

            ~RunImpl();

            void start() noexcept;

            [[nodiscard]] const RunImpl& getFirstRunImpl() const noexcept {
                if (m_parent) {
                    return m_parent->getFirstRunImpl();
                }
                return *this;
            }

            [[nodiscard]] Id getTestId() const noexcept override {
                return Id(*this);
            }

            void log(const char *msg) override {
                hal::outputDebugFmt("{}: {}\n",
                                    std::string_view(m_test.m_name),
                                    std::string_view(msg));
            }

            void failWith(const char *msg) override {
                if (m_num_failed++ == 0u) {
                    m_failure = msg;
                    m_status = fail;
                }
                if (m_parent != nullptr) {
                    m_parent->failWith(msg);
                }
            }

            void recurse(const UnitTest &test) override {
                RunImpl new_run{test, *this};
                test.run(new_run);
            }

            void success() override {
                ++m_num_passed;
                if (m_parent != nullptr) {
                    m_parent->success();
                }
            }

#if PPR_ENABLE_ASSERTIONS
            void onAssertFailure(const Assertion &condition) const;
#endif
        };
    };
}

template<pP::details::TChar CharT>
struct std::formatter<pP::UnitTest::Id, CharT> {
    template<typename FormatParseContextT>
    static constexpr auto parse(FormatParseContextT &ctx) -> decltype(ctx.begin()) {
        return ctx.begin();
    }

    template<typename FormatContextT>
    auto format(const pP::UnitTest::Id &value, FormatContextT &ctx) const -> decltype(ctx.out()) {
        auto output = ctx.out();
        return value.format<CharT>(output);
    }
};

void pP::UnitTest::RunImpl::start() noexcept {
#if PPR_ENABLE_ASSERTIONS
    // install a new assertion policy to catch error messages
    Assertion::Policy new_policy(std23::nontype<&RunImpl::onAssertFailure>, this);
    m_prev_assert_policy = Assertion::setFailurePolicy(std::move(new_policy));
#endif

    m_start_time = std::chrono::steady_clock::now();
}

pP::UnitTest::RunImpl::~RunImpl() {
    const std::chrono::steady_clock::time_point end_time = std::chrono::steady_clock::now();
    const auto duration_from_start = std::chrono::duration_cast<std::chrono::microseconds>(end_time - m_start_time);

#if PPR_ENABLE_ASSERTIONS
    PPR_DEFER {
        if (m_prev_assert_policy.has_value()) [[likely]] {
            // restore previous assertion policy
            Assertion::setFailurePolicy(std::move(m_prev_assert_policy.value()));
            m_prev_assert_policy.reset();
        }
    };
#endif

    const Id test_id{*this};
    const bool is_close_group = (m_num_passed + m_num_failed) > 1u;
    const char bullet_ch = is_close_group ? '{' : '|';

    if (m_status == pass) {
        hal::outputDebugFmt("TEST [OK] {:2}/{:2} pass\t{} {}\t({})\n",
                            m_num_passed, m_num_passed + m_num_failed,
                            bullet_ch, test_id, duration_from_start);
    } else {
        hal::outputDebugFmt("TEST *KO* {:2}/{:2} pass\t{} {} -> {}\t({})\n",
                            m_num_passed, m_num_passed + m_num_failed,
                            bullet_ch, test_id, std::string_view{m_failure}, duration_from_start);
    }
    if (is_close_group) {
        hal::outputDebug("--------------------------------------------------------------------------------\n");
    }
}

#if PPR_ENABLE_ASSERTIONS
void pP::UnitTest::RunImpl::onAssertFailure(const Assertion &condition) const {
    const std::stacktrace backtrace = std::stacktrace::current(9);

    hal::outputDebugFmt("{}({}): Assertion failed with \"{}\"\n"
           "\tin function: {}\n"
           "\tin test: {}\n\n"
           "Callstack:\n{}\n",
           std::string_view(condition.m_site.file_name()),
           condition.m_site.line(),
           std::string_view(condition.m_message),
           std::string_view(condition.m_site.function_name()),
           getTestId(),
           backtrace);

    throw std::logic_error(condition.m_message);
}
#endif
