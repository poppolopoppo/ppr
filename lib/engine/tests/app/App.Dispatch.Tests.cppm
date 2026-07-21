module;
#include "pP/Macros.h"

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
        mapping.mapKey(safe_ptr<const InputAction>{&action}, InputKey::from(EKeyboardKey::space).value());

        Player player{id};
        player.addMapping(safe_ptr<const InputMapping>{&mapping}, 0);

        const InputMessage message{
            InputKey::from(EKeyboardKey::space).value(),
            InputDigital{true},
            zero_v,
            InputDeviceID{0u},
            EInputMessageEvent::pressed,
        };

        const EInputListenerResponse response = player.getListener().postKeyEvent(message);
        PPR_ASSERT(response != EInputListenerResponse::unhandled);

        const std::optional<InputValue> value = player.getActionValue(action);
        PPR_ASSERT(value.has_value());
        PPR_ASSERT(std::get<InputDigital>(*value) == InputDigital{true});
    };

    PPR_UNIT_TEST(is_any_key_returns_unhandled) {
        const InputMessage message{
            InputKey::any_key,
            InputDigital{true},
            zero_v,
            InputDeviceID{0u},
            EInputMessageEvent::pressed,
        };

        InputListener listener{};
        const EInputListenerResponse response = listener.postKeyEvent(message);
        PPR_ASSERT(response == EInputListenerResponse::unhandled);
    };

    PPR_UNIT_TEST(app_dispatch) {
        _.recurse({
            dispatch_player_mapping_to_action,
            is_any_key_returns_unhandled,
        });
    };
}
