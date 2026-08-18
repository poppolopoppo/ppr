module;
#include "pP/UnitTest.h"

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
import :pixel_readback;
import :camera;

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
            app_pixel_readback,
            app_camera,
        });
    };
}
