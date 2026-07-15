module;
#include "pP/Macros.h"

export module engine.tests.app:player;

import engine.core;
import engine.app;
import std;

static pP::KeyboardDevice s_test_keyboard{pP::InputDeviceID{0u}};

export namespace pP::tests {
    PPR_UNIT_TEST(player_id_ordering) {
        const PlayerId keyboard{.m_kind = EPlayerKind::keyboard, .m_local_index = 0u, .m_user_id = 0u};
        const PlayerId gamepad{.m_kind = EPlayerKind::gamepad, .m_local_index = 0u, .m_user_id = 0u};
        PPR_ASSERT(keyboard == keyboard);
        PPR_ASSERT(keyboard != gamepad);
        PPR_ASSERT(keyboard < gamepad);
    };

    PPR_UNIT_TEST(player_construction_and_device_views) {
        const PlayerId id{.m_kind = EPlayerKind::keyboard, .m_local_index = 0u, .m_user_id = 0u};
        Player player{id};
        PPR_ASSERT(player.getId() == id);
        PPR_ASSERT(player.getDeviceViews().empty());

        player.pushDeviceView(safe_ptr<const IInputDevice>{&s_test_keyboard});
        PPR_ASSERT(player.getDeviceViews().size() == 1u);
    };

    PPR_UNIT_TEST(app_player) {
        _.recurse({
            player_id_ordering,
            player_construction_and_device_views,
        });
    };
}
