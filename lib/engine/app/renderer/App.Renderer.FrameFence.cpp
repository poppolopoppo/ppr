module;
#include "pP/Macros.h"
module engine.app;

import :renderer.frame_fence;

import engine.core;
import engine.rhi;
import std;

namespace pP {
    // ReSharper disable once CppUseInternalLinkage
    PPR_DEFINE_LOG_CATEGORY(FrameFence, info, none)

    // ------------------------------------------------------------------
    // fence-governed gpu reuse
    // ------------------------------------------------------------------

    u32 selectRingSlot(
        const FenceCompletionValue completed, const std::span<const FenceCompletionValue> slot_values,
        const u32 cursor) noexcept {
        if (slot_values.empty()) {
            return cursor;
        }
        const u32 slot_count = safe_narrowing(slot_values.size());
        for (u32 step = 1u; step <= slot_count; ++step) {
            const u32 slot = (cursor + step) % slot_count;
            if (slotFenceComplete(completed, slot_values[slot])) {
                return slot;
            }
        }
        // Every slot still in flight: the caller must wait for this slot's
        // value before writing. Returning the next slot (rather than failing)
        // keeps the cursor rotation deterministic; safety comes from the
        // mandatory wait, never from skipping the write.
        return (cursor + 1u) % slot_count;
    }

    FenceCompletionValue maxSlotValue(const std::span<const FenceCompletionValue> slot_values) noexcept {
        FenceCompletionValue youngest = kFenceValueNone;
        for (const FenceCompletionValue value: slot_values) {
            youngest = std::max(youngest, value);
        }
        return youngest;
    }

    // ------------------------------------------------------------------
    // frame credit ledger
    // ------------------------------------------------------------------

    std::error_code FrameCreditLedger::acquire() noexcept {
        if (m_outstanding >= kMaxOutstandingFrames) [[unlikely]] {
            return std::make_error_code(std::errc::device_or_resource_busy);
        }
        ++m_outstanding;
        return default_value_v;
    }

    std::error_code FrameCreditLedger::release() noexcept {
        if (m_outstanding == 0u) [[unlikely]] {
            return std::make_error_code(std::errc::invalid_argument);
        }
        --m_outstanding;
        return default_value_v;
    }

    void FrameCreditLedger::reset() noexcept {
        m_outstanding = 0u;
    }

    // ------------------------------------------------------------------
    // deferred release
    // ------------------------------------------------------------------

    void DeferredReleaseQueue::defer(
        const FenceCompletionValue last_use_value, std23::move_only_function<void()> release) noexcept {
        m_entries.push_back(
            DeferredEntry{
                .m_last_use = last_use_value,
                .m_release = std::move(release),
            });
    }

    u32 DeferredReleaseQueue::reclaim(const FenceCompletionValue completed) noexcept {
        u32 reclaimed = 0u;
        std::size_t kept = 0u;
        for (std::size_t index = 0u; index < m_entries.size(); ++index) {
            if (m_entries[index].m_last_use <= completed) {
                if (m_entries[index].m_release) {
                    std::move(m_entries[index].m_release)();
                }
                ++reclaimed;
            } else {
                if (kept != index) {
                    m_entries[kept] = std::move(m_entries[index]);
                }
                ++kept;
            }
        }
        m_entries.resize(kept);
        return reclaimed;
    }

    u32 DeferredReleaseQueue::drainAll() noexcept {
        const u32 count = safe_narrowing(m_entries.size());
        for (DeferredEntry &entry: m_entries) {
            if (entry.m_release) {
                std::move(entry.m_release)();
            }
        }
        m_entries.clear();
        return count;
    }

    u32 DeferredReleaseQueue::pendingCount() const noexcept {
        return safe_narrowing(m_entries.size());
    }

    // ------------------------------------------------------------------
    // app-owned monotonic fence
    // ------------------------------------------------------------------

    std::error_code FrameFence::initialize(rhi::IDevice &device) {
        if (m_fence) [[unlikely]] {
            return std::make_error_code(std::errc::device_or_resource_busy);
        }
        rhi::FenceDesc fence_desc{};
        fence_desc.initialValue = kFenceValueNone;
        fence_desc.isShared = false;
        fence_desc.label = "frame completion fence";
        PPR_RETURN_ERROR_ON_FAIL(FrameFence, make_error_code(device.createFence(fence_desc, m_fence.writeRef())));
        m_device = &device;
        m_next_value = 1u;
        m_last_submitted = kFenceValueNone;
        m_completed = kFenceValueNone;
        m_terminal = false;
        return default_value_v;
    }

    std::error_code FrameFence::shutdown() {
        std::error_code first_err{};
        if (m_terminal) {
            // Confirmed loss already: nothing worth waiting for.
            m_fence.setNull();
            m_device = nullptr;
            return first_err;
        }
        if (m_fence and m_device and m_last_submitted != kFenceValueNone) {
            // Shutdown under load: the last submission may still be
            // GPU-pending. Wait it out (bounded by the two-credit bound in
            // practice) so no release below races in-flight work. A generic
            // wait failure here is UNKNOWN completion RETAINED at teardown
            // — the fence stays alive (no abandon, no second wait, no
            // reset) and the error reports for process exit; the OS
            // reclaims retained objects. Only a positively-confirmed
            // removal (no_such_device_or_address) abandons wait-free.
            // Mid-frame waits never retain-or-escalate: they propagate.
            rhi::IFence *fences[] = {m_fence.get()};
            const u64 values[] = {m_last_submitted};
            if (const std::error_code wait_err = make_error_code(
                    m_device->waitForFences(1u, fences, values, true, rhi::kTimeoutInfinite))) {
                first_err = wait_err;
                if (wait_err == std::make_error_code(std::errc::no_such_device_or_address)) {
                    abandon();
                }
                return first_err;
            }
        }
        m_fence.setNull();
        m_device = nullptr;
        m_next_value = 1u;
        m_last_submitted = kFenceValueNone;
        m_completed = kFenceValueNone;
        return first_err;
    }

    void FrameFence::abandon() noexcept {
        m_fence.setNull();
        m_device = nullptr;
        m_next_value = 1u;
        m_last_submitted = kFenceValueNone;
        m_completed = kFenceValueNone;
        m_terminal = true;
    }

    void FrameFence::commitSubmitted(const FenceCompletionValue value) noexcept {
        m_last_submitted = value;
        m_next_value = value + 1u;
    }

    Expected<FenceCompletionValue> FrameFence::pollCompleted() noexcept {
        if (not m_fence) [[unlikely]] {
            return std::unexpected{std::make_error_code(std::errc::not_connected)};
        }
        if (m_terminal) [[unlikely]] {
            // Confirmed removal latches the device-loss code — distinct from
            // both never-initialized and capacity-busy, up to teardown.
            return std::unexpected{std::make_error_code(std::errc::no_such_device_or_address)};
        }
        u64 current = kFenceValueNone;
        if (const std::error_code err = make_error_code(m_fence->getCurrentValue(&current))) [[unlikely]] {
            // Never invent completion: the previous value stands and the
            // caller keeps every credit and every deferred entry.
            return std::unexpected{err};
        }
        if (current == std::numeric_limits<u64>::max()) [[unlikely]] {
            // Device-removal sentinel: completion can never arrive.
            // Latch the terminal state and report the loss; the previous
            // completion still stands for the caller's accounting.
            m_terminal = true;
            return std::unexpected{std::make_error_code(std::errc::no_such_device_or_address)};
        }
        // Monotonic guard: the backend must never report completion going
        // backwards; a stale read keeps the high-water mark.
        m_completed = std::max(m_completed, current);
        return m_completed;
    }

    std::error_code FrameFence::waitFor(const FenceCompletionValue value, const u64 timeout_ns) {
        if (not m_fence or m_device == nullptr) [[unlikely]] {
            return std::make_error_code(std::errc::not_connected);
        }
        if (m_terminal) [[unlikely]] {
            return std::make_error_code(std::errc::no_such_device_or_address);
        }
        if (value == kFenceValueNone or value <= m_completed) {
            return default_value_v;
        }
        rhi::IFence *fences[] = {m_fence.get()};
        const u64 values[] = {value};
        if (const std::error_code wait_err = make_error_code(
                m_device->waitForFences(1u, fences, values, true, timeout_ns))) [[unlikely]] {
            // UNKNOWN completion, never confirmed loss: the high-water mark
            // stands, nothing is released or reused, and the error propagates
            // (the escalation policy in the header owns what happens next).
            // Deliberately no terminal latch and no second wait here.
            return wait_err;
        }
        // A successful wait implies completion, but the value is still
        // observed, never assumed: a post-wait poll failure (including the
        // removal sentinel, which latches terminal) propagates INSTEAD of
        // success, so a wait that raced removal never reports completion.
        if (const Expected<FenceCompletionValue> observed = pollCompleted(); not observed) [[unlikely]] {
            return observed.error();
        }
        return default_value_v;
    }
} // namespace pP
