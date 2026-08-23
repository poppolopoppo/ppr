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
import :quaternion;
import :input_listener;
import :filtered_analog;
import :viewport_client;

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
            app_quaternion,
            app_input_listener,
            app_filtered_analog,
            app_viewport_client,
        });
    };
}
