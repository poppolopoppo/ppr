module;
#include "pP/Macros.h"

export module engine.tests.app;

import engine.core;
import :devices;
import :player;
import :dispatch;
import :snapshot;
import :player_service;
import :player_graph;
import :shader;
import :viewport;

export namespace pP::tests {
    PPR_UNIT_TEST(app) {
        _.recurse({
            app_devices,
            app_player,
            app_dispatch,
            app_snapshot,
            app_player_service,
            app_player_graph,
            app_shader,
            app_viewport,
        });
    };
}
