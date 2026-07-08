module;
#include "pP/Macros.h"
module engine.core;
import :assert;
import :concurrency.context;
import :concurrency.event;
import :opaque;
import :timer;
import std;

namespace pP::context {

// BackgroundContext
class BackgroundContext final : public IContext {
    NeverEvent m_done{};

public:
    BackgroundContext() noexcept = default;

    [[nodiscard]] std::optional<std::error_code>
    error() const noexcept override {
        return std::nullopt;
    }

    [[nodiscard]] std::optional<const opaque::Block::Value *>
    value(const string_literal) const noexcept override {
        return std::nullopt;
    }

    // IEvent interface:

    TagPtr<ISignal> subscribeEvent(const TagPtr<ISignal> signal) noexcept override {
        return m_done.subscribeEvent(signal);
    }

    void unsubscribeEvent(const TagPtr<ISignal> signal, const TagPtr<ISignal> restore) noexcept override {
        m_done.unsubscribeEvent(signal, restore);
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

class CancelContext : public IContext, protected ISignal {
    const SharedContext m_parent;
    BroadcastEvent m_done{};
    const TagPtr<ISignal> m_restore;
    std::atomic<int> m_error{0u};

protected: // ISignal interface
    // ReSharper disable once CppOverrideWithDifferentVisibility
    void notify(const std::size_t) noexcept final {
        m_done.emitEvent();
    }

    // ReSharper disable once CppOverrideWithDifferentVisibility
    void wait() noexcept final {
    }

public:
    explicit CancelContext(SharedContext parent) noexcept
        : m_parent(std::move(parent)),
          m_restore(m_parent->subscribeEvent(TagPtr<ISignal>(this, 0u))) {
        PPR_ASSERT(m_parent.get());
    }

    ~CancelContext() noexcept override {
        m_parent->unsubscribeEvent(TagPtr<ISignal>(this, 0u), m_restore);
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

public: // IContext interface:
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

    // IEvent interface:

    TagPtr<ISignal> subscribeEvent(const TagPtr<ISignal> signal) noexcept override {
        return m_done.subscribeEvent(signal);
    }

    void unsubscribeEvent(const TagPtr<ISignal> signal, const TagPtr<ISignal> restore) noexcept override {
        m_done.unsubscribeEvent(signal, restore);
    }

    [[nodiscard]] bool pollEvent() noexcept override {
        return m_done.pollEvent();
    }

    void resetEvent() noexcept override {
        m_done.resetEvent();
    }
};

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

    // IContext interface:

    [[nodiscard]] std::optional<std::error_code> error() const noexcept override {
        return std::nullopt; // Never has an error
    }

    [[nodiscard]] std::optional<const opaque::Block::Value *> value(const string_literal user_key) const noexcept override {
        return m_parent->value(user_key); // Values still flow down
    }

    // IEvent interface:

    TagPtr<ISignal> subscribeEvent(const TagPtr<ISignal> signal) noexcept override {
        return m_done.subscribeEvent(signal);
    }

    void unsubscribeEvent(const TagPtr<ISignal> signal, const TagPtr<ISignal> restore) noexcept override {
        m_done.unsubscribeEvent(signal, restore);
    }

    [[nodiscard]] bool pollEvent() noexcept override {
        return m_done.pollEvent();
    }

    void resetEvent() noexcept override {
        m_done.resetEvent();
    }
};

// --------------------------------------------------------------
// execute a callback after cancel or destruction
// --------------------------------------------------------------

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

    // IContext interface:

    [[nodiscard]] std::optional<std::error_code>
    error() const noexcept override {
        return m_parent->error();
    }

    [[nodiscard]] std::optional<const opaque::Block::Value *>
    value(const string_literal user_key) const noexcept override {
        return m_parent->value(user_key);
    }

    // IEvent interface:

    TagPtr<ISignal> subscribeEvent(const TagPtr<ISignal> signal) noexcept override {
        return m_parent->subscribeEvent(signal);
    }

    void unsubscribeEvent(const TagPtr<ISignal> signal, const TagPtr<ISignal> restore) noexcept override {
        m_parent->unsubscribeEvent(signal, restore);
    }

    [[nodiscard]] bool pollEvent() noexcept override {
        return m_parent->pollEvent();
    }

    void resetEvent() noexcept override {
        m_parent->resetEvent();
    }
};

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

    // IContext interface:

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

    // IEvent interface:

    TagPtr<ISignal> subscribeEvent(const TagPtr<ISignal> signal) noexcept override {
        return m_parent->subscribeEvent(signal);
    }

    void unsubscribeEvent(const TagPtr<ISignal> signal, const TagPtr<ISignal> restore) noexcept override {
        m_parent->unsubscribeEvent(signal, restore);
    }

    [[nodiscard]] bool pollEvent() noexcept override {
        return m_parent->pollEvent();
    }

    void resetEvent() noexcept override {
        m_parent->resetEvent();
    }
};

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

SharedContext background() {
    return std::make_shared<BackgroundContext>();
}

std::pair<SharedContext, CancelFunc> withCancel(SharedContext parent) {
    std::shared_ptr context = std::make_shared<CancelContext>(parent);
    CancelFunc cancel{context};
    return std::make_pair(std::move(context), std::move(cancel));
}

std::pair<SharedContext, CancelClauseFunc> withCancelClause(SharedContext parent) {
    std::shared_ptr context = std::make_shared<CancelContext>(parent);
    CancelClauseFunc cancelClause{context};
    return std::make_pair(std::move(context), std::move(cancelClause));
}

SharedContext withoutCancel(SharedContext parent) {
    return std::make_shared<WithoutCancelContext>(parent);
}

SharedContext withAfterFunc(SharedContext parent, AfterFunc &&execute_after) {
    return std::make_shared<AfterContext>(parent, std::move(execute_after));
}

SharedContext withValue(SharedContext parent, const string_literal user_key, const opaque::Value &value) {
    return std::make_shared<ValueContext>(
        parent, opaque::Dict{
            {user_key, value}
        });
}

SharedContext withValues(SharedContext parent, const opaque::Dict &values) {
    return std::make_shared<ValueContext>(parent, values);
}

SharedContext withDeadlineCause(SharedContext parent, const TimePoint deadline, std::error_code cause,
                                 TimerManager &timer) {
    return DeadlineContext::schedule(std::move(parent), deadline, cause, timer);
}

SharedContext withDeadline(SharedContext parent, const TimePoint deadline, TimerManager &timer) {
    return withDeadlineCause(std::move(parent), deadline, std::make_error_code(std::errc::timed_out), timer);
}

SharedContext withTimeoutCause(SharedContext parent, const TimeSpan delay, std::error_code cause,
                                TimerManager &timer) {
    return withDeadlineCause(std::move(parent), timer.now() + delay, cause, timer);
}

SharedContext withTimeout(SharedContext parent, const TimeSpan delay, TimerManager &timer) {
    return withDeadlineCause(std::move(parent), timer.now() + delay, std::make_error_code(std::errc::timed_out), timer);
}

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
}
