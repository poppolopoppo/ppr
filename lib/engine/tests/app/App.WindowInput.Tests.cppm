module;
#include "pP/UnitTest.h"

export module engine.tests.app:window_input;

import engine.app;
import engine.core;
import engine.math;
import std;

export namespace pP::tests {
    namespace WindowInput {
        struct FakeInputService final : IInputService {
            KeyboardDevice m_keyboard{InputDeviceID{1u}};
            MouseDevice m_mouse{InputDeviceID{2u}};
            GamepadDevice m_gamepad{InputDeviceID{3u}};
            InputContext m_global{};
            std::vector<SharedInputContext> m_contexts{};
            float2 m_last_cursor{0.0f, 0.0f};

            [[nodiscard]] const KeyboardDevice &getKeyboard() const noexcept override {
                return m_keyboard;
            }

            [[nodiscard]] const MouseDevice &getMouse() const noexcept override {
                return m_mouse;
            }

            [[nodiscard]] const GamepadDevice &getGamepad(int) const noexcept override {
                return m_gamepad;
            }

            [[nodiscard]] SharedInputDevice getInputDeviceByID(const InputDeviceID &) const noexcept override {
                return {};
            }

            [[nodiscard]] std::error_code enumerateInputDevices(Collector<SharedInputDevice>) const noexcept override {
                return default_value_v;
            }

            [[nodiscard]] std::error_code enumerateInputKeysSupported(Collector<InputKey>) const override {
                return default_value_v;
            }

            [[nodiscard]] InputContext &getGlobalInputContext() noexcept override {
                return m_global;
            }

            [[nodiscard]] bool hasInputContext(const InputContext &context) const noexcept override {
                if (&m_global == &context) {
                    return true;
                }
                return std::ranges::any_of(m_contexts, [&](const SharedInputContext &stored) noexcept {
                    return stored.get() == &context;
                });
            }

            void addInputContext(SharedInputContext context) override {
                m_contexts.push_back(std::move(context));
            }

            bool removeInputContext(const InputContext &context) override {
                const auto it = std::ranges::find_if(m_contexts, [&](const SharedInputContext &stored) noexcept {
                    return stored.get() == &context;
                });
                if (it == m_contexts.end()) {
                    return false;
                }
                m_contexts.erase(it);
                return true;
            }

            void assignInputContextToDevice(InputDeviceID, SharedInputContext) override {
            }

            void clearInputContextDeviceAssignments() override {
                m_contexts.clear();
            }

            [[nodiscard]] std::error_code enumerateInputContexts(Collector<InputContext>) const override {
                return default_value_v;
            }

            [[nodiscard]] std::error_code enumerateInputContextDeviceAssignments(
                Collector<IInputDevice, InputContext>) const override {
                return default_value_v;
            }

            [[nodiscard]] std::error_code pollInputDevices(TimeSpan) override {
                return default_value_v;
            }

            void resetInputDevices() noexcept override {
                m_keyboard.resetInputState();
                m_mouse.resetInputState();
                m_gamepad.resetInputState();
            }

            void postKeyboardCharacterInput(const InputContext &context, const hal::native::char_t codepoint) override {
                m_keyboard.postKeyboardCharacterInput(context, codepoint);
            }

            void postKeyboardKeyPressed(const InputContext &context, const EKeyboardKey key, const bool pressed) override {
                m_keyboard.postKeyboardKeyPressed(TimeSpan{}, context, key, pressed);
            }

            void postMouseButtonPressed(const InputContext &context, const EMouseButton button, const bool pressed) override {
                m_mouse.postMouseButtonPressed(TimeSpan{}, context, button, pressed);
            }

            void postMouseCursorPosition(const InputContext &context, const float2 &absolute_pos) override {
                m_last_cursor = absolute_pos;
                m_mouse.postMouseCursorPosition(TimeSpan{}, context, absolute_pos);
            }

            void postMouseScrollWheel(const InputContext &context, const float2 &delta) override {
                m_mouse.postMouseScrollWheel(TimeSpan{}, context, delta);
            }

            [[nodiscard]] DeviceCallback::Handle whenDeviceConnected(DeviceCallback::Event) override {
                return {};
            }

            [[nodiscard]] DeviceCallback::Handle whenDeviceDisconnected(DeviceCallback::Event) override {
                return {};
            }
        };

        PPR_UNIT_TEST (mouse_keeps_client_space_with_nonzero_window_origin) {
            FakeInputService inputs{};
            Window window{
                WindowHandle{reinterpret_cast<void *>(1)}, NativeWindowHandle{reinterpret_cast<void *>(1)},
                WindowModel{.m_window_position = int2{400, 300}, .m_window_size = int2{800, 600}}
            };
            {
                WindowInputContext routed{safe_ptr<IInputService>{&inputs}, safe_ptr<Window>{&window}};
                const float2 client{10.0f, 20.0f};
                window.m_when_mouse_moved(window, client);

                PPR_TEST_ASSERT(inputs.m_last_cursor.x == client.x && inputs.m_last_cursor.y == client.y);

                const float2 &stored = inputs.m_mouse.m_cursor_pos.m_raw.m_absolute;
                PPR_TEST_ASSERT(stored.x == client.x && stored.y == client.y);
            }
            std::ignore = window.release();
        };

        PPR_UNIT_TEST (char_posted_after_poll_clear_survives_to_consumer) {
            KeyboardDevice device{InputDeviceID{0u}};
            InputContext context{};

            // Frame start: transient clear runs before window-event dispatch.
            PPR_TEST_ASSERT(not device.pollInputMessages(TimeSpan{}));
            // Dispatch: character arrives via window event.
            device.postKeyboardCharacterInput(context, static_cast<hal::native::char_t>('a'));
            // Consumer (ImGui::NewFrame) observes it in the same frame.
            PPR_TEST_ASSERT(device.m_character_inputs.size() == 1u);
            // Next frame start clears it exactly once.
            PPR_TEST_ASSERT(not device.pollInputMessages(TimeSpan{}));
            PPR_TEST_ASSERT(device.m_character_inputs.empty());
        };

        PPR_UNIT_TEST (reset_clears_pending_characters) {
            KeyboardDevice device{InputDeviceID{0u}};
            InputContext context{};

            device.postKeyboardCharacterInput(context, static_cast<hal::native::char_t>('b'));
            PPR_TEST_ASSERT(device.m_character_inputs.size() == 1u);

            device.resetInputState();
            PPR_TEST_ASSERT(device.m_character_inputs.empty());
        };
    }

    PPR_UNIT_TEST (window_input){
        _.recurse({
            WindowInput::mouse_keeps_client_space_with_nonzero_window_origin,
            WindowInput::char_posted_after_poll_clear_survives_to_consumer,
            WindowInput::reset_clears_pending_characters,
        });

    };
}
