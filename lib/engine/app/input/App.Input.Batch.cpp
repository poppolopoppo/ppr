module;

#include "pP/Macros.h"

module engine.app;

import :input.batch;

import std;

namespace pP {
    // ReSharper disable once CppUseInternalLinkage
    PPR_DEFINE_LOG_CATEGORY(InputBatch, debug, none)

    void PendingInputBatch::postChar(const safe_ptr<const InputContext> context, const hal::native::char_t codepoint) {
        m_events.push_back(PendingCharInput{context, codepoint});
    }

    void PendingInputBatch::postKey(
        const safe_ptr<const InputContext> context, const EKeyboardKey key, const bool pressed) {
        m_events.push_back(PendingKeyInput{context, key, pressed});
    }

    void PendingInputBatch::postMouseButton(
        const safe_ptr<const InputContext> context, const EMouseButton button, const bool pressed) {
        m_events.push_back(PendingMouseButtonInput{context, button, pressed});
    }

    void PendingInputBatch::postMouseCursor(const safe_ptr<const InputContext> context, const float2 &absolute_pos) {
        m_events.push_back(PendingMouseCursorInput{context, absolute_pos});
    }

    void PendingInputBatch::postMouseScroll(const safe_ptr<const InputContext> context, const float2 &delta) {
        m_events.push_back(PendingMouseScrollInput{context, delta});
    }

    std::error_code PendingInputBatch::dispatch(KeyboardDevice &keyboard, MouseDevice &mouse, const TimeSpan dt) {
        // Own the collected records on the stack first: every exit path below
        // — normal return, first-error break, or an exception escaping a
        // listener/device post (e.g. a throwing character-input callback) —
        // destroys the records, releasing their safe_ptr<InputContext>
        // observers before returning to the caller. A throwing dispatch can
        // therefore never strand observers into teardown.
        Array<PendingInputEvent> owned{};
        owned.swap(m_events);
        std::error_code first_err{};
        for (const PendingInputEvent &event: owned) {
            std::visit(
                [&](const auto &pending) {
                    if (not pending.m_context.isValid()) [[unlikely]] {
                        PPR_RETAIN_ERROR_ON_FAIL(InputBatch, first_err, make_error_code(std::errc::invalid_argument));
                        return;
                    }
                    const InputContext &context = *pending.m_context;
                    using PendingT = std::decay_t<decltype(pending)>;
                    if constexpr (std::is_same_v<PendingT, PendingCharInput>) {
                        keyboard.postKeyboardCharacterInput(context, pending.m_codepoint);
                    } else if constexpr (std::is_same_v<PendingT, PendingKeyInput>) {
                        keyboard.postKeyboardKeyPressed(dt, context, pending.m_key, pending.m_pressed);
                    } else if constexpr (std::is_same_v<PendingT, PendingMouseButtonInput>) {
                        mouse.postMouseButtonPressed(dt, context, pending.m_button, pending.m_pressed);
                    } else if constexpr (std::is_same_v<PendingT, PendingMouseCursorInput>) {
                        mouse.postMouseCursorPosition(dt, context, pending.m_absolute_pos);
                    } else if constexpr (std::is_same_v<PendingT, PendingMouseScrollInput>) {
                        mouse.postMouseScrollWheel(dt, context, pending.m_delta);
                    }
                },
                event);
            if (first_err) {
                break;
            }
        }
        return first_err;
    }

    void PendingInputBatch::clear() noexcept {
        m_events.clear();
    }

    bool PendingInputBatch::empty() const noexcept {
        return m_events.empty();
    }

    std::size_t PendingInputBatch::size() const noexcept {
        return m_events.size();
    }
}
