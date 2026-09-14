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
import :lifecycle;
import :camera;
import :quaternion;
import :input_listener;
import :filtered_analog;
import :window_input;
import :imgui_routing;
import :imgui_dpi;
import :zerov_probe;

export namespace pP::tests {
    PPR_UNIT_TEST (app){
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
            renderer_triangle_reinit_ok,
            imgui_live_shutdown_idempotent,
            app_init_rollback_on_window_create_failure,
            app_init_rollback_on_input_connect_failure,
            app_teardown_double_shutdown_no_refire,
            app_teardown_reports_subsystem_failure_but_completes,
            app_preserves_pre_registered_graphics_services,
            app_run_after_teardown_not_permitted,
            app_headless_lifecycle_is_idempotent,
            app_run_shutdowns_after_nonstandard_hook_exception,
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
