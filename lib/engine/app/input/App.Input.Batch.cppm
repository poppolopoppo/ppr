module;

export module engine.app:input.batch;

import :input.device;
import :input.key;
import :input.listener;

import engine.core;
import engine.math;
import std;

export namespace pP {
    // ------------------------------------------------------------------
    // P0 owned completed-batch boundary: native event collection appends
    // owned records here WITHOUT invoking client callbacks; the defined
    // client-advancement point replays them exactly once, in collection
    // order, then clears. Same-thread staged storage only — no spans,
    // no function_ref, no borrowed views escape. Poll-time gamepad
    // sampling bypasses the batch (it already runs once per frame at the
    // advancement point); only pump-time key/char/mouse events batch.
    // ------------------------------------------------------------------

    struct PendingCharInput {
        safe_ptr<const InputContext> m_context{};
        hal::native::char_t m_codepoint{};
    };

    struct PendingKeyInput {
        safe_ptr<const InputContext> m_context{};
        EKeyboardKey m_key{};
        bool m_pressed{false};
    };

    struct PendingMouseButtonInput {
        safe_ptr<const InputContext> m_context{};
        EMouseButton m_button{};
        bool m_pressed{false};
    };

    struct PendingMouseCursorInput {
        safe_ptr<const InputContext> m_context{};
        float2 m_absolute_pos{zero_v};
    };

    struct PendingMouseScrollInput {
        safe_ptr<const InputContext> m_context{};
        float2 m_delta{zero_v};
    };

    using PendingInputEvent = std::variant<
        PendingCharInput, PendingKeyInput, PendingMouseButtonInput, PendingMouseCursorInput, PendingMouseScrollInput>;

    class PendingInputBatch {
    public:
        PendingInputBatch() noexcept = default;

        void postChar(safe_ptr<const InputContext> context, hal::native::char_t codepoint);

        void postKey(safe_ptr<const InputContext> context, EKeyboardKey key, bool pressed);

        void postMouseButton(safe_ptr<const InputContext> context, EMouseButton button, bool pressed);

        void postMouseCursor(safe_ptr<const InputContext> context, const float2 &absolute_pos);

        void postMouseScroll(safe_ptr<const InputContext> context, const float2 &delta);

        // Replay every collected event in order through the real devices,
        // then clear. First error wins; a null-context record fails closed.
        [[nodiscard]] std::error_code dispatch(KeyboardDevice &keyboard, MouseDevice &mouse, TimeSpan dt);

        void clear() noexcept;

        [[nodiscard]] bool empty() const noexcept;

        [[nodiscard]] std::size_t size() const noexcept;

    private:
        Array<PendingInputEvent> m_events{};
    };
}
