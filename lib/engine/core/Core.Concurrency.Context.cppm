module;
#include "pP/Macros.h"
export module engine.core:context;

import :assert;
import :event;
import :opaque;
import :timer;

import std;

export namespace pP {
    // ------------------------------------------------------------------
    // context interface — done() fires on cancellation
    // ------------------------------------------------------------------

    class IContext {
    public:
        virtual ~IContext() = default;

        [[nodiscard]] virtual IEvent &
        done() noexcept = 0;

        [[nodiscard]] virtual std::optional<std::error_code>
        error() const noexcept = 0;

        [[nodiscard]] virtual std::optional<const opaque::Block::Value *>
        value(string_literal user_key) const noexcept = 0;
    };

    using SharedContext = std::shared_ptr<IContext>;

    // ------------------------------------------------------------------
    // context helpers
    // ------------------------------------------------------------------

    namespace context {
        // --------------------------------------------------------------
        // background empty context
        // --------------------------------------------------------------

        class BackgroundContext final : public IContext {
            NeverEvent m_done{};

        public:
            BackgroundContext() noexcept = default;

            [[nodiscard]] IEvent &
            done() noexcept override {
                return m_done;
            }

            [[nodiscard]] std::optional<std::error_code>
            error() const noexcept override {
                return std::nullopt;
            }

            [[nodiscard]] std::optional<const opaque::Block::Value *>
            value(const string_literal) const noexcept override {
                return std::nullopt;
            }
        };

        [[nodiscard]] SharedContext background() {
            return std::make_shared<BackgroundContext>();
        }

        // --------------------------------------------------------------
        // cancelable context, with or without an explicit error code
        // --------------------------------------------------------------

        // using CancelFunc = std23::function_ref<void()>;
        class CancelContext;

        struct CancelFunc {
            std::weak_ptr<CancelContext> m_context{};
            void operator()() const noexcept;
        };

        struct CancelClauseFunc {
            std::weak_ptr<CancelContext> m_context{};
            void operator()(const std::error_code error) const noexcept;
        };

        class CancelContext : public IContext, protected ISignal {
            const SharedContext m_parent;
            BroadcastEvent m_done{};
            const TagPtr<ISignal> m_restore;
            std::atomic<int> m_error{0u};

        protected: // ISignal interface
            void notify(const std::size_t) noexcept final {
                m_done.emitEvent();
            }

            void wait() noexcept final {
            }

        public:
            explicit CancelContext(SharedContext parent) noexcept
                : m_parent(std::move(parent)),
                  m_restore(m_parent->done().subscribeEvent(TagPtr<ISignal>(this, 0u))) {
                PPR_ASSERT(m_parent.get());
            }

            ~CancelContext() noexcept override {
                m_parent->done().unsubscribeEvent(TagPtr<ISignal>(this, 0u), m_restore);
            }

            void cancel() noexcept {
                cancelCause(std::make_error_code(std::errc::operation_canceled));
            }

            void cancelCause(const std::error_code err) noexcept {
                PPR_ASSERT(err.category() == std::generic_category());

                if (int expect_no_error = 0; m_error.compare_exchange_strong(
                    expect_no_error, err.value(), std::memory_order_acq_rel)) {
                    m_done.emitEvent();
                }
            }

        public: // IContext interface
            [[nodiscard]] IEvent &done() noexcept override {
                return m_done;
            }

            [[nodiscard]] std::optional<std::error_code>
            error() const noexcept override {
                if (const int err = m_error.load(std::memory_order_acquire); err != 0u) {
                    return std::error_code(err, std::generic_category());
                }
                return m_parent->error();
            }

            [[nodiscard]] std::optional<const opaque::Block::Value *>
            value(const string_literal user_key) const noexcept override {
                return m_parent->value(user_key);
            }
        };

        void CancelFunc::operator()() const noexcept {
            if (const std::shared_ptr<CancelContext> ctx = m_context.lock()) [[likely]] {
                ctx->cancel();
            }
        }

        void CancelClauseFunc::operator()(const std::error_code error) const noexcept {
            if (const std::shared_ptr<CancelContext> ctx = m_context.lock()) [[likely]] {
                ctx->cancelCause(error);
            }
        }

        [[nodiscard]] std::pair<SharedContext, CancelFunc> withCancel(SharedContext parent) {
            std::shared_ptr context = std::make_shared<CancelContext>(parent);
            CancelFunc cancel{context};
            return std::make_pair(std::move(context), std::move(cancel));
        }

        [[nodiscard]] std::pair<SharedContext, CancelClauseFunc> withCancelClause(SharedContext parent) {
            std::shared_ptr context = std::make_shared<CancelContext>(parent);
            CancelClauseFunc cancelClause{context};
            return std::make_pair(std::move(context), std::move(cancelClause));
        }

        // --------------------------------------------------------------
        // withoutCancel: Severs cancellation, preserves values
        // --------------------------------------------------------------

        class WithoutCancelContext final : public IContext {
            const SharedContext m_parent{};
            NeverEvent m_done{}; // Never fires

        public:
            explicit WithoutCancelContext(SharedContext parent) noexcept
                : m_parent(std::move(parent)) {
                PPR_ASSERT(m_parent.get());
                // Notice: We specifically DO NOT subscribe to m_parent->done()
            }

            [[nodiscard]] IEvent &done() noexcept override {
                return m_done;
            }

            [[nodiscard]] std::optional<std::error_code> error() const noexcept override {
                return std::nullopt; // Never has an error
            }

            [[nodiscard]] std::optional<const opaque::Block::Value *> value(const string_literal user_key) const noexcept override {
                return m_parent->value(user_key); // Values still flow down
            }
        };

        [[nodiscard]] SharedContext withoutCancel(SharedContext parent) {
            return std::make_shared<WithoutCancelContext>(parent);
        }

        // --------------------------------------------------------------
        // execute a callback after cancel or destruction
        // --------------------------------------------------------------

        using AfterFunc = std::move_only_function<void(const IContext &) noexcept>;

        class AfterContext final : public IContext {
            const SharedContext m_parent{};
            AfterFunc m_execute_after;

        public:
            AfterContext(SharedContext parent, AfterFunc &&execute_after) noexcept
                : m_parent(std::move(parent)),
                  m_execute_after(std::move(execute_after)) {
                PPR_ASSERT(m_parent.get());
            }

            ~AfterContext() noexcept override {
                m_execute_after(*this);
            }

            [[nodiscard]] IEvent &done() noexcept override {
                return m_parent->done();
            }

            [[nodiscard]] std::optional<std::error_code>
            error() const noexcept override {
                return m_parent->error();
            }

            [[nodiscard]] std::optional<const opaque::Block::Value *>
            value(const string_literal user_key) const noexcept override {
                return m_parent->value(user_key);
            }
        };

        [[nodiscard]] SharedContext withAfterFunc(SharedContext parent, AfterFunc &&execute_after) {
            return std::make_shared<AfterContext>(parent, std::move(execute_after));
        }

        // --------------------------------------------------------------
        // associate opaque data with the context
        // --------------------------------------------------------------

        class ValueContext final : public IContext {
            const SharedContext m_parent;
            const opaque::Unique<mem::GPA> m_opaque;

        public:
            ValueContext(SharedContext parent, const opaque::Dict &values)
                : m_parent(std::move(parent)),
                  m_opaque(opaque::makeUnique<mem::GPA>(values)) {
                PPR_ASSERT(m_parent.get());
            }

            [[nodiscard]] IEvent &done() noexcept override {
                return m_parent->done();
            }

            [[nodiscard]] std::optional<std::error_code>
            error() const noexcept override {
                return m_parent->error();
            }

            [[nodiscard]] std::optional<const opaque::Block::Value *>
            value(const string_literal user_key) const noexcept override {
                if (const opaque::Block::Value *const p_value = m_opaque->tryGet(user_key)) {
                    return p_value;
                }
                return m_parent->value(user_key);
            }
        };

        [[nodiscard]] SharedContext withValue(SharedContext parent, const string_literal user_key, const opaque::Value &value) {
            return std::make_shared<ValueContext>(
                parent, opaque::Dict{
                    {user_key, value}
                });
        }

        [[nodiscard]] SharedContext withValues(SharedContext parent, const opaque::Dict &values) {
            return std::make_shared<ValueContext>(parent, values);
        }

        // --------------------------------------------------------------
        // cancels on timeout or parent cancel
        // --------------------------------------------------------------

        class DeadlineContext : public CancelContext {
            const TimePoint m_deadline;
            const std::error_code m_cause;

        public:
            DeadlineContext(SharedContext parent, const TimePoint deadline, const std::error_code cause) noexcept
                : CancelContext(std::move(parent)),
                  m_deadline(deadline),
                  m_cause(cause) {
            }

            [[nodiscard]] static SharedContext schedule(SharedContext parent, const TimePoint deadline, const std::error_code cause,
                                                 TimerManager &timer = TimerManager::mainTimer()) {
                std::shared_ptr<DeadlineContext> ctx = std::make_shared<DeadlineContext>(
                    std::move(parent), deadline, cause);

                timer.schedule(
                    deadline, [weak_self(std::weak_ptr(ctx))](const TimePoint now) noexcept {
                        if (const std::shared_ptr<DeadlineContext> self = weak_self.lock()) [[likely]] {
                            self->cancelDeadline(now);
                        }
                    });

                return std::move(ctx);
            }

            [[nodiscard]] TimePoint deadline() const noexcept {
                return m_deadline;
            }

            void cancelDeadline(const TimePoint now) noexcept {
                if (PPR_ENSURE(now >= m_deadline)) [[likely]] {
                    cancelCause(m_cause);
                }
            }
        };

        [[nodiscard]] SharedContext withDeadlineCause(SharedContext parent, const TimePoint deadline, std::error_code cause,
                                                      TimerManager &timer = TimerManager::mainTimer()) {
            return DeadlineContext::schedule(std::move(parent), deadline, cause, timer);
        }

        [[nodiscard]] SharedContext withDeadline(SharedContext parent, const TimePoint deadline,
                                                 TimerManager &timer = TimerManager::mainTimer()) {
            return withDeadlineCause(std::move(parent), deadline, std::make_error_code(std::errc::timed_out), timer);
        }

        [[nodiscard]] SharedContext withTimeoutCause(SharedContext parent, const TimeDuration delay, std::error_code cause,
                                                     TimerManager &timer = TimerManager::mainTimer()) {
            return withDeadlineCause(std::move(parent), timer.now() + delay, cause, timer);
        }

        [[nodiscard]] SharedContext withTimeout(SharedContext parent, const TimeDuration delay,
                                                TimerManager &timer = TimerManager::mainTimer()) {
            return withDeadlineCause(std::move(parent), timer.now() + delay, std::make_error_code(std::errc::timed_out), timer);
        }
    }
}
