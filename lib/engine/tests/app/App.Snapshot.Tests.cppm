module;
#include "pP/Macros.h"

export module engine.tests.app:snapshot;

import engine.core;
import engine.app;
import std;

export namespace pP::tests {
    PPR_UNIT_TEST(snapshot_captures_frame_messages) {
        constexpr PlayerIdentity id{
            .m_user_id = default_value_v,
            .m_device_id = default_value_v,
            .m_local_index = 0u,
            .m_kind = EPlayerKind::keyboard, };

        Player player{id};

        const InputMessage message{
            InputKey::from(EKeyboardKey::space).value(),
            InputDigital{true},
            zero_v,
            InputDeviceID{0u},
            EInputMessageEvent::pressed,
        };
        player.pushFrameMessage(message);

        const InputFrameSnapshot snapshot = player.sample();
        PPR_ASSERT(snapshot.m_player_id == id);
        PPR_ASSERT(snapshot.m_messages.size() == 1u);
    };

    PPR_UNIT_TEST(app_snapshot) {
        _.recurse({
            snapshot_captures_frame_messages,
        });
    };
}
