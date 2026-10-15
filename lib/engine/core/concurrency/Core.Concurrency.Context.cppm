module;
export module engine.core:concurrency.context;

import :concurrency.event;
import :function.ref;
import :opaque;
import :strings;
import :timer;

import std;

export namespace pP {
    class IContext : public IEvent {
    public:
        virtual ~IContext() = default;

        [[nodiscard]] virtual std::error_code error() const noexcept = 0;

        [[nodiscard]] virtual std::optional<const opaque::Block::Value *> value(
            string_literal user_key) const noexcept = 0;
    };

    using SharedContext = std::shared_ptr<IContext>;

    // ------------------------------------------------------------------
    // context helpers
    // ------------------------------------------------------------------

    namespace context {
        [[nodiscard]] SharedContext background();

        [[nodiscard]] SharedContext application() noexcept;
        SharedContext setApplication(SharedContext app_context);
        // --------------------------------------------------------------
        // background context is the default pass-through / noop
        // --------------------------------------------------------------

        class BackgroundContext final : public IContext {
            NeverEvent m_done{};

        public:
            BackgroundContext() noexcept = default;

            [[nodiscard]] std::error_code error() const noexcept override {
                return default_value_v;
            }

            [[nodiscard]] std::optional<const opaque::Block::Value *> value(
                const string_literal) const noexcept override {
                return std::nullopt;
            }

            // IEvent interface:

            TagPtr<ISignal> subscribeEvent(TagPtr<ISignal> signal) noexcept override {
                return m_done.subscribeEvent(std::move(signal));
            }

            void unsubscribeEvent(TagPtr<ISignal> signal, TagPtr<ISignal> restore) noexcept override {
                m_done.unsubscribeEvent(std::move(signal), std::move(restore));
            }

            [[nodiscard]] bool pollEvent() noexcept override {
                return m_done.pollEvent();
            }

            void resetEvent() noexcept override {
                m_done.resetEvent();
            }
        };

        // --------------------------------------------------------------
        // cancelable context, with or without an explicit error code
        // --------------------------------------------------------------
        class CancelContext;

        struct CancelFunc {
            std::weak_ptr<CancelContext> m_context{};

            [[nodiscard]] bool isValid() const noexcept;

            void operator()() const noexcept;

            void reset();
        };

        struct CancelClauseFunc {
            std::weak_ptr<CancelContext> m_context{};

            [[nodiscard]] bool isValid() const noexcept;

            void operator()(std::error_code clause) const noexcept;

            void reset();
        };

        [[nodiscard]] std::pair<SharedContext, CancelFunc> withCancel(SharedContext parent);

        [[nodiscard]] std::pair<SharedContext, CancelClauseFunc> withCancelClause(SharedContext parent);

        // --------------------------------------------------------------
        // withoutCancel: Severs cancellation, preserves values
        // --------------------------------------------------------------

        [[nodiscard]] SharedContext withoutCancel(SharedContext parent);

        // --------------------------------------------------------------
        // execute a callback after cancel or destruction
        // --------------------------------------------------------------

        using AfterFunc = std23::move_only_function<void(const IContext &)>;

        [[nodiscard]] SharedContext withAfterFunc(SharedContext parent, AfterFunc &&execute_after);

        // --------------------------------------------------------------
        // associate opaque data with the context
        // --------------------------------------------------------------

        [[nodiscard]] SharedContext withValue(
            SharedContext parent, string_literal user_key, const opaque::Value &value);

        [[nodiscard]] SharedContext withValues(SharedContext parent, const opaque::Dict &values);

        // --------------------------------------------------------------
        // cancels on timeout or parent cancel
        // --------------------------------------------------------------

        [[nodiscard]] SharedContext withDeadlineCause(
            SharedContext parent, TimePoint deadline, std::error_code cause, TimerManager &timer);

        [[nodiscard]] SharedContext withDeadline(SharedContext parent, TimePoint deadline, TimerManager &timer);

        [[nodiscard]] SharedContext withTimeoutCause(
            SharedContext parent, TimeSpan delay, std::error_code cause, TimerManager &timer);

        [[nodiscard]] SharedContext withTimeout(SharedContext parent, TimeSpan delay, TimerManager &timer);
    }
}
