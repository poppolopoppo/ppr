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
import :render_view;
import :pixel_readback;
import :camera;
import :quaternion;
import :input_listener;
import :filtered_analog;
import :window_input;
import :imgui_routing;
import :imgui_dpi;
import :zerov_probe;

export namespace pP::tests {
    PPR_UNIT_TEST(app){
        _.recurse({
            devices,
            player,
            dispatch,
            snapshot,
            player_service,
            player_graph,
            shader,
            viewport,
            render_view,
            pixel_readback,
            camera,
            quaternion,
            input_listener,
            filtered_analog,
            window_input,
            imgui_routing,
            imgui_dpi,
            zerov_probe,
        });

    };
}
