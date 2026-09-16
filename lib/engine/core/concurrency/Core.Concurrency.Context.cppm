module;
export module engine.core:concurrency.context;

import :concurrency.event;
import :opaque;
import :strings;
import :timer;

import std;

export namespace pP {
    class IContext : public IEvent {
    public:
        virtual ~IContext() = default;

        [[nodiscard]] virtual std::error_code
        error() const noexcept = 0;

        [[nodiscard]] virtual std::optional<const opaque::Block::Value *>
        value(string_literal user_key) const noexcept = 0;
    };

    using SharedContext = std::shared_ptr<IContext>;

    // ------------------------------------------------------------------
    // context helpers
    // ------------------------------------------------------------------

    namespace context {
        [[nodiscard]] SharedContext background();

        // --------------------------------------------------------------
        // cancelable context, with or without an explicit error code
        // --------------------------------------------------------------
        class CancelContext;

        struct CancelFunc {
            std::weak_ptr<CancelContext> m_context{};

            [[nodiscard]] bool isValid() const noexcept;

            void operator()() const noexcept;
        };

        struct CancelClauseFunc {
            std::weak_ptr<CancelContext> m_context{};

            [[nodiscard]] bool isValid() const noexcept;

            void operator()(std::error_code clause) const noexcept;
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

        using AfterFunc = std::move_only_function<void(const IContext &) noexcept>;

        [[nodiscard]] SharedContext withAfterFunc(SharedContext parent, AfterFunc &&execute_after);

        // --------------------------------------------------------------
        // associate opaque data with the context
        // --------------------------------------------------------------

        [[nodiscard]] SharedContext withValue(SharedContext parent, string_literal user_key, const opaque::Value &value);

        [[nodiscard]] SharedContext withValues(SharedContext parent, const opaque::Dict &values);

        // --------------------------------------------------------------
        // cancels on timeout or parent cancel
        // --------------------------------------------------------------

        [[nodiscard]] SharedContext withDeadlineCause(SharedContext parent, TimePoint deadline, std::error_code cause, TimerManager &timer);

        [[nodiscard]] SharedContext withDeadline(SharedContext parent, TimePoint deadline, TimerManager &timer);

        [[nodiscard]] SharedContext withTimeoutCause(SharedContext parent, TimeSpan delay, std::error_code cause, TimerManager &timer);

        [[nodiscard]] SharedContext withTimeout(SharedContext parent, TimeSpan delay, TimerManager &timer);
    }
}
