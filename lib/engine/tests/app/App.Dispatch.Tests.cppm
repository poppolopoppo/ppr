module;
#include "pP/UnitTest.h"

export module engine.tests.app:dispatch;

import engine.core;
import engine.app;
import std;

export namespace pP::tests {
    PPR_UNIT_TEST(dispatch_player_mapping_to_action) {
        constexpr PlayerIdentity id{
            .m_user_id = default_value_v,
            .m_device_id = default_value_v,
            .m_local_index = 0u,
            .m_kind = EPlayerKind::keyboard, };

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
            InputKey::any_key,
            InputDigital{true},
            InputDeviceID{0u},
            EInputMessageEvent::pressed,
        };

        InputListener listener{};
        const EInputMessageResponse response = listener.postKeyEvent(TimeSpan{}, message);
        PPR_TEST_ASSERT(response == EInputMessageResponse::unhandled);
    };

    PPR_UNIT_TEST(app_dispatch) {
        _.recurse({
            dispatch_player_mapping_to_action,
            is_any_key_returns_unhandled,
        });
    };
}
