module engine.tests.app;

import engine.core;

namespace pP::tests {
    extern const UnitTest devices;
    extern const UnitTest player;
    extern const UnitTest dispatch;
    extern const UnitTest snapshot;
    extern const UnitTest player_service;
    extern const UnitTest player_graph;
    extern const UnitTest shader;
    extern const UnitTest viewport;
    extern const UnitTest render_view;
    extern const UnitTest pixel_readback;
    extern const UnitTest renderer_triangle_reinit_ok;
    extern const UnitTest imgui_live_shutdown_idempotent;
    extern const UnitTest app_init_rollback_on_window_create_failure;
    extern const UnitTest app_init_rollback_on_input_connect_failure;
    extern const UnitTest app_teardown_double_shutdown_no_refire;
    extern const UnitTest app_teardown_reports_subsystem_failure_but_completes;
    extern const UnitTest app_preserves_pre_registered_graphics_services;
    extern const UnitTest app_headless_lifecycle_is_idempotent;
    extern const UnitTest app_run_shutdowns_after_nonstandard_hook_exception;
    extern const UnitTest camera;
    extern const UnitTest quaternion;
    extern const UnitTest input_listener;
    extern const UnitTest filtered_analog;
    extern const UnitTest window_input;
    extern const UnitTest imgui_routing;
    extern const UnitTest imgui_dpi;
    extern const UnitTest zerov_probe;
    // Defined here rather than as an inline constexpr umbrella in App.Tests.cppm:
    // same MSVC C1001 shape as engine.tests.core. Same test set and order as before.
    extern const UnitTest app = UnitTest::Named("app") / [](UnitTest::IRun &_) -> void {
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
} // namespace pP::tests
