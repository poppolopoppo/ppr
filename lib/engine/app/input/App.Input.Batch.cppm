module;

export module engine.app:input.batch;

import :input.device;
import :input.key;
import :input.listener;
import :window.handle;

import engine.core;
import engine.math;
import std;

export namespace pP {
    class IInputDevice;

    // ------------------------------------------------------------------
    // P0 owned completed-batch boundary: native event collection appends
    // owned records here WITHOUT invoking client callbacks; the defined
    // client-advancement point replays them exactly once, in collection
    // order, then clears. Same-thread staged storage only — no spans,
    // no function_ref, no borrowed views escape. Poll-time gamepad
    // sampling bypasses the batch (it already runs once per frame at the
    // advancement point); only pump-time key/char/mouse events batch.
    //
    // B2' ordered control records: focus + device hotplug callbacks are
    // split into an immediate platform half (window flags, device
    // registries — applied synchronously at collection time) and a deferred
    // client half (broadcasts — applied by the appliers below during the
    // exactly-once dispatch, in collection order with the input replay).
    // Busy-frame pumps accumulate records without dispatching; the discard-
    // before-destroy rule covers control records identically (clear() drops
    // every observer view, including windows and devices).
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

    // Deferred client half of a window-focus change. The platform half
    // (Window::m_focused + service focused-window tracking) already ran at
    // collection time; the applier fires the window + service broadcasts.
    struct PendingFocusInput {
        safe_ptr<Window> m_window{};
        bool m_focused{false};
    };

    // Deferred client half of a device hotplug event. The platform half
    // (device registry + id/name bookkeeping) already ran at collection
    // time; the applier fires the connected/disconnected broadcast.
    struct PendingDeviceInput {
        safe_ptr<const IInputDevice> m_device{};
        bool m_connected{false};
    };

    using PendingInputEvent = std::variant<
        PendingCharInput, PendingKeyInput, PendingMouseButtonInput, PendingMouseCursorInput, PendingMouseScrollInput,
        PendingFocusInput, PendingDeviceInput>;

    // Deferred-half appliers, supplied by the platform at the defined
    // advancement point (never stored). Null appliers with queued control
    // records fail closed.
    using PendingFocusApplier = std23::move_only_function<std::error_code(safe_ptr<Window> window, bool focused)>;
    using PendingDeviceApplier =
        std23::move_only_function<std::error_code(safe_ptr<const IInputDevice> device, bool connected)>;

    class PendingInputBatch {
    public:
        PendingInputBatch() noexcept = default;

        void postChar(safe_ptr<const InputContext> context, hal::native::char_t codepoint);

        void postKey(safe_ptr<const InputContext> context, EKeyboardKey key, bool pressed);

        void postMouseButton(safe_ptr<const InputContext> context, EMouseButton button, bool pressed);

        void postMouseCursor(safe_ptr<const InputContext> context, const float2 &absolute_pos);

        void postMouseScroll(safe_ptr<const InputContext> context, const float2 &delta);

        void postFocus(safe_ptr<Window> window, bool focused);

        void postDeviceConnection(safe_ptr<const IInputDevice> device, bool connected);

        // Replay every collected event in order through the real devices
        // (input records) and the platform appliers (control records),
        // then clear. First error wins; a null-context record fails closed.
        [[nodiscard]] std::error_code dispatch(
            KeyboardDevice &keyboard, MouseDevice &mouse, TimeSpan dt, PendingFocusApplier &&focus_applier = {},
            PendingDeviceApplier &&device_applier = {});

        void clear() noexcept;

        [[nodiscard]] bool empty() const noexcept;

        [[nodiscard]] std::size_t size() const noexcept;

    private:
        Array<PendingInputEvent> m_events{};
    };
}
