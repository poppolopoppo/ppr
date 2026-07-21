module;
#include "pP/Macros.h"

export module engine.tests.app:player;

import engine.core;
import engine.app;
import std;

static pP::KeyboardDevice s_test_keyboard{pP::InputDeviceID{0u}};

export namespace pP::tests {
    PPR_UNIT_TEST(player_id_ordering) {
        constexpr PlayerIdentity keyboard{
            .m_user_id = default_value_v,
            .m_device_id = default_value_v,
            .m_local_index = 0u,
            .m_kind = EPlayerKind::keyboard, };

        constexpr PlayerIdentity gamepad{
            .m_user_id = default_value_v,
            .m_device_id = default_value_v,
            .m_local_index = 0u,
            .m_kind = EPlayerKind::gamepad, };

        PPR_ASSERT(keyboard == keyboard);
        PPR_ASSERT(keyboard != gamepad);
        PPR_ASSERT(keyboard < gamepad);
    };

    PPR_UNIT_TEST(player_construction_and_device_views) {
        constexpr PlayerIdentity id{
            .m_user_id = default_value_v,
            .m_device_id = default_value_v,
            .m_local_index = 0u,
            .m_kind = EPlayerKind::keyboard, };

        Player player{id};
        PPR_ASSERT(player.getIdentity() == id);
        PPR_ASSERT(player.getDeviceViews().isEmpty());

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
