module;
#include "pP/UnitTest.h"

module engine.tests.app;

import engine.core;
import engine.app;
import std;

namespace pP::tests::detail {
    PPR_UNIT_TEST(player_id_ordering) {
        constexpr PlayerIdentity keyboard{
            .m_user_id = default_value_v,
            .m_device_id = default_value_v,
            .m_local_index = 0u,
            .m_kind = EPlayerKind::keyboard,
        };

        constexpr PlayerIdentity gamepad{
            .m_user_id = default_value_v,
            .m_device_id = default_value_v,
            .m_local_index = 0u,
            .m_kind = EPlayerKind::gamepad,
        };;

        PPR_TEST_ASSERT(keyboard == keyboard);
        PPR_TEST_ASSERT(keyboard != gamepad);
        PPR_TEST_ASSERT(keyboard < gamepad);
    };

    PPR_UNIT_TEST(player_construction_and_device_views) {
        constexpr PlayerIdentity id{
            .m_user_id = default_value_v,
            .m_device_id = default_value_v,
            .m_local_index = 0u,
            .m_kind = EPlayerKind::keyboard,
        };

        KeyboardDevice test_keyboard{InputDeviceID{0u}};

        Player player{id};
        PPR_TEST_ASSERT(player.getIdentity() == id);
        PPR_TEST_ASSERT(player.getDeviceViews().isEmpty());

        player.pushDeviceView(safe_ptr<const IInputDevice>{&test_keyboard});
        PPR_TEST_ASSERT(player.getDeviceViews().size() == 1u);
    };


    PPR_UNIT_TEST(player_mapping_to_action) {
        constexpr PlayerIdentity id{
            .m_user_id = default_value_v,
            .m_device_id = default_value_v,
            .m_local_index = 0u,
            .m_kind = EPlayerKind::keyboard,
        };

        InputAction action{"Jump", EInputValueType::digital, EInputActionFlags::none};
        InputMapping mapping{"PlayerControls"};
        mapping.mapInputKey(safe_ptr<const InputAction>{&action}, InputKey::from(EKeyboardKey::space).value());

        Player player{id};
        player.addMapping(safe_ptr<const InputMapping>{&mapping}, 0);

        const InputMessage message{
            InputKey::from(EKeyboardKey::space).value(),
            InputDigital{true},
            InputDeviceID{0u},
            EInputMessageEvent::pressed,
        };

        const EInputMessageResponse response = player.getListener().postKeyEvent(TimeSpan{}, message);
        PPR_TEST_ASSERT(response != EInputMessageResponse::unhandled);

        const std::optional<InputValue> value = player.getActionValue(action);
        PPR_TEST_ASSERT(value.has_value());
        PPR_TEST_ASSERT(std::get<InputDigital>(*value) == InputDigital{true});
    };

    PPR_UNIT_TEST(is_any_key_returns_unhandled) {
        const InputMessage message{
            InputKey::any_digital,
            InputDigital{true},
            InputDeviceID{0u},
            EInputMessageEvent::pressed,
        };

        InputListener listener{};
        const EInputMessageResponse response = listener.postKeyEvent(TimeSpan{}, message);
        PPR_TEST_ASSERT(response == EInputMessageResponse::unhandled);
    };


    PPR_UNIT_TEST(captures_frame_messages) {
        constexpr PlayerIdentity id{
            .m_user_id = default_value_v,
            .m_device_id = default_value_v,
            .m_local_index = 0u,
            .m_kind = EPlayerKind::keyboard,
        };

        Player player{id};

        const InputMessage message{
            InputKey::from(EKeyboardKey::space).value(),
            InputDigital{true},
            InputDeviceID{0u},
            EInputMessageEvent::pressed,
        };
        player.pushFrameMessage(message);

        const InputFrameSnapshot snapshot = player.sample();
        PPR_TEST_ASSERT(snapshot.m_player_id == id);
        PPR_TEST_ASSERT(snapshot.m_messages.size() == 1u);
    };
} // namespace pP::tests::detail

namespace pP::tests {
    extern const UnitTest player = UnitTest::Named("player") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::player_id_ordering,
            detail::player_construction_and_device_views,
        });
    };
    extern const UnitTest dispatch = UnitTest::Named("dispatch") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::player_mapping_to_action,
            detail::is_any_key_returns_unhandled,
        });
    };
    extern const UnitTest snapshot = UnitTest::Named("snapshot") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            detail::captures_frame_messages,
        });
    };
} // namespace pP::tests
