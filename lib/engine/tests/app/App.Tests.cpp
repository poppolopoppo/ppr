module engine.tests.app;

import engine.core;

namespace pP::tests {
    const UnitTest &devicesTests() noexcept;

    const UnitTest &playerTests() noexcept;

    const UnitTest &dispatchTests() noexcept;

    const UnitTest &snapshotTests() noexcept;

    const UnitTest &player_serviceTests() noexcept;

    const UnitTest &player_graphTests() noexcept;

    const UnitTest &shaderTests() noexcept;

    const UnitTest &viewportTests() noexcept;

    const UnitTest &render_viewTests() noexcept;

    const UnitTest &pixel_readbackTests() noexcept;

    const UnitTest &renderer_triangle_reinit_okTests() noexcept;

    const UnitTest &imgui_live_shutdown_idempotentTests() noexcept;

    const UnitTest &app_init_rollback_on_window_create_failureTests() noexcept;

    const UnitTest &app_init_rollback_on_input_connect_failureTests() noexcept;

    const UnitTest &app_teardown_double_shutdown_no_refireTests() noexcept;

    const UnitTest &app_teardown_reports_subsystem_failure_but_completesTests() noexcept;

    const UnitTest &app_preserves_pre_registered_graphics_servicesTests() noexcept;

    const UnitTest &app_headless_lifecycle_is_idempotentTests() noexcept;

    const UnitTest &app_run_shutdowns_after_nonstandard_hook_exceptionTests() noexcept;

    const UnitTest &cameraTests() noexcept;

    const UnitTest &quaternionTests() noexcept;

    const UnitTest &input_listenerTests() noexcept;

    const UnitTest &filtered_analogTests() noexcept;

    const UnitTest &window_inputTests() noexcept;

    const UnitTest &imgui_routingTests() noexcept;

    const UnitTest &imgui_dpiTests() noexcept;

    const UnitTest &zerov_probeTests() noexcept;

    // Defined here rather than as an inline constexpr umbrella in App.Tests.cppm:
    // same MSVC C1001 shape as engine.tests.core. Same test set and order as before.
    extern const UnitTest app = UnitTest::Named("app") / [](UnitTest::IRun &_) -> void {
        _.recurse({
            devicesTests(),
            playerTests(),
            dispatchTests(),
            snapshotTests(),
            player_serviceTests(),
            player_graphTests(),
            shaderTests(),
            viewportTests(),
            render_viewTests(),
            pixel_readbackTests(),
            renderer_triangle_reinit_okTests(),
            imgui_live_shutdown_idempotentTests(),
            app_init_rollback_on_window_create_failureTests(),
            app_init_rollback_on_input_connect_failureTests(),
            app_teardown_double_shutdown_no_refireTests(),
            app_teardown_reports_subsystem_failure_but_completesTests(),
            app_preserves_pre_registered_graphics_servicesTests(),
            app_headless_lifecycle_is_idempotentTests(),
            app_run_shutdowns_after_nonstandard_hook_exceptionTests(),
            cameraTests(),
            quaternionTests(),
            input_listenerTests(),
            filtered_analogTests(),
            window_inputTests(),
            imgui_routingTests(),
            imgui_dpiTests(),
            zerov_probeTests(),
        });
    };
} // namespace pP::tests
