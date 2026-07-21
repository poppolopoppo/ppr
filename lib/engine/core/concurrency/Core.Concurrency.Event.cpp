module;
#include "pP/Macros.h"
module engine.core;
import :concurrency.event;
import std;

namespace pP {

// ------------------------------------------------------------------
// Pulse event only calls notify() once, until reset() is called
// ------------------------------------------------------------------

TagPtr<ISignal> PulseEvent::subscribeEvent(const TagPtr<ISignal> signal) noexcept {
    PPR_ASSERT((signal.m_packed & signal_bit_v) == 0u);

    TagPtr<ISignal> parent;
    parent.m_packed = m_signal.exchange(signal.m_packed, std::memory_order_acq_rel);

    if (parent.m_packed & signal_bit_v) [[unlikely]] {
        parent.m_packed &= ~signal_bit_v;
        emitEvent();
    }

    return parent;
}

void PulseEvent::unsubscribeEvent(const TagPtr<ISignal> signal, const TagPtr<ISignal> restore) noexcept {
    [[maybe_unused]] const auto subscription = subscribeEvent(restore);
    PPR_ASSERT(subscription == signal);
}

bool PulseEvent::pollEvent() noexcept {
    return (m_signal.load(std::memory_order_acquire) & signal_bit_v) != 0u;
}

void PulseEvent::resetEvent() noexcept {
    m_signal.fetch_and(~signal_bit_v, std::memory_order_release);
}

void PulseEvent::emitEvent() noexcept {
    TagPtr<ISignal> handler;
    handler.m_packed = m_signal.fetch_or(signal_bit_v, std::memory_order_acq_rel);
    const bool should_notify = (handler.m_packed & signal_bit_v) == 0u;
    handler.m_packed &= ~signal_bit_v;

    if (should_notify && handler.isValid()) {
        const auto [signal, event_tag] = handler.unpack();
        signal->notify(event_tag);
    }
}

// ------------------------------------------------------------------
// Broadcast event has the same behavior than pulse, but supports multiple subscribers
// ------------------------------------------------------------------

TagPtr<ISignal> BroadcastEvent::subscribeEvent(const TagPtr<ISignal> signal) noexcept {
    const std::lock_guard scope_lock(m_subscriptions_mutex);
    PPR_ASSERT(m_subscriptions.find(signal) == m_subscriptions.end());
    m_subscriptions.pushBack(signal);
    return default_value_v;
}

void BroadcastEvent::unsubscribeEvent(const TagPtr<ISignal> signal, const TagPtr<ISignal> restore) noexcept {
    PPR_ASSERT(restore.isNull());

    const std::lock_guard scope_lock(m_subscriptions_mutex);
    const auto it = m_subscriptions.find(signal);
    if (PPR_ENSURE(it != m_subscriptions.end())) [[likely]] {
        m_subscriptions.erase(it);
    }
}

bool BroadcastEvent::pollEvent() noexcept {
    return m_signal.test(std::memory_order_acquire);
}

void BroadcastEvent::resetEvent() noexcept {
    m_signal.clear(std::memory_order_release);
}

void BroadcastEvent::emitEvent() noexcept {
    if (not m_signal.test_and_set(std::memory_order_acq_rel)) {
        StableVectorInplace<TagPtr<ISignal>> snapshot;
        {
            const std::lock_guard scope_lock(m_subscriptions_mutex);
            snapshot.append(m_subscriptions);
        }
        for (const TagPtr<ISignal> handler : snapshot) {
            const auto [signal, event_tag] = handler.unpack();
            signal->notify(event_tag);
        }
    }
}

}
