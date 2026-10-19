add_executable(engine.tests.app main.cpp)

target_sources(engine.tests.app
    PUBLIC
        FILE_SET CXX_MODULES BASE_DIRS ${CMAKE_SOURCE_DIR} FILES
            App.Tests.cppm

            # F1 game-payload tests compile the colony subset directly: the
            # sim-carrying packet lives in game/ (engine.app must not link
            # sim/physics), so its unit tests ship with the modules, not via
            # a link to the app.game executable. Include the owned worldgen
            # catalog/metric closure; Panel and Smoke stay self-hosted.
            ${CMAKE_SOURCE_DIR}/game/colony/Colony.cppm
            ${CMAKE_SOURCE_DIR}/game/colony/WorldGen.cppm
            ${CMAKE_SOURCE_DIR}/game/colony/Content.cppm
            ${CMAKE_SOURCE_DIR}/game/colony/Metric.cppm
            ${CMAKE_SOURCE_DIR}/game/colony/ColonyDriver.cppm
            ${CMAKE_SOURCE_DIR}/game/colony/DigTool.cppm
            ${CMAKE_SOURCE_DIR}/game/colony/Pathfinding.cppm
            ${CMAKE_SOURCE_DIR}/game/colony/ColonyTranslator.cppm
            ${CMAKE_SOURCE_DIR}/game/colony/Agents.cppm
            ${CMAKE_SOURCE_DIR}/game/colony/Buildings.cppm
            ${CMAKE_SOURCE_DIR}/game/colony/SimFramePacket.cppm
    )

target_sources(engine.tests.app
    PRIVATE
        App.Tests.cpp
        App.SimPacket.Tests.cpp
        App.Player.Tests.cpp
        App.PlayerService.Tests.cpp
        App.Player.Graph.Tests.cpp
        App.Devices.Tests.cpp
        App.Input.Listener.Tests.cpp
        App.FilteredAnalog.Tests.cpp
        App.WindowInput.Tests.cpp
        App.Shader.Tests.cpp
        App.Viewport.Tests.cpp
        App.RenderView.Tests.cpp
        App.PixelReadback.Tests.cpp
        App.Camera.Tests.cpp
        App.Display.Tests.cpp
        App.GridPass.Tests.cpp
        App.Frame.Tests.cpp
        App.FrameFence.Tests.cpp
        App.RenderLane.Tests.cpp
        App.RunGate.Tests.cpp
        App.Quaternion.Tests.cpp
        App.ImGuiRouting.Tests.cpp
        App.ImguiDpi.Tests.cpp
        App.PerformanceWidget.Tests.cpp
        App.ZeroVProbe.Tests.cpp

        ${CMAKE_SOURCE_DIR}/game/colony/Colony.cpp
        ${CMAKE_SOURCE_DIR}/game/colony/WorldGen.cpp
        ${CMAKE_SOURCE_DIR}/game/colony/WorldGen.Fixture.cpp
        ${CMAKE_SOURCE_DIR}/game/colony/WorldGen.Plan.cpp
        ${CMAKE_SOURCE_DIR}/game/colony/WorldGen.Export.cpp
        ${CMAKE_SOURCE_DIR}/game/colony/Content.cpp
        ${CMAKE_SOURCE_DIR}/game/colony/Content.Pack.cpp
        ${CMAKE_SOURCE_DIR}/game/colony/Content.Source.cpp
        ${CMAKE_SOURCE_DIR}/game/colony/Content.Descriptors.cpp
        ${CMAKE_SOURCE_DIR}/game/colony/Content.Assembly.cpp
        ${CMAKE_SOURCE_DIR}/game/colony/Content.Presentation.cpp
        ${CMAKE_SOURCE_DIR}/game/colony/Metric.cpp
        ${CMAKE_SOURCE_DIR}/game/colony/ColonyDriver.cpp
        ${CMAKE_SOURCE_DIR}/game/colony/ColonyDriver.Checkpoint.cpp
        ${CMAKE_SOURCE_DIR}/game/colony/ColonyDriver.Edits.cpp
        ${CMAKE_SOURCE_DIR}/game/colony/DigTool.cpp
        ${CMAKE_SOURCE_DIR}/game/colony/Pathfinding.cpp
        ${CMAKE_SOURCE_DIR}/game/colony/ColonyTranslator.cpp
        ${CMAKE_SOURCE_DIR}/game/colony/Agents.cpp
        ${CMAKE_SOURCE_DIR}/game/colony/Buildings.cpp
        ${CMAKE_SOURCE_DIR}/game/colony/SimFramePacket.cpp
        ${CMAKE_SOURCE_DIR}/game/colony/SimFramePacket.Diagnostics.cpp
    )

setup_ppr_project(engine.tests.app
    INTERNAL_PUBLIC_DEPS engine.app engine.sim engine.image engine.tests
    # mango-image: the ImGuiRouting font-rasterizer evidence harness writes
    # atlas PNG captures through mango::image::Surface::save (CPU-only),
    # mirroring engine.tests.asset.
    EXTERNAL_SYSTEM_PRIVATE_DEPS glfw mango-image
)

# ColonyDriver links physics privately (same shape as engine.tests.sim);
# the colony headers include Colony.Elements.h by relative name.
target_link_libraries(engine.tests.app PRIVATE engine.physics)

target_include_directories(engine.tests.app PRIVATE
    ${CMAKE_SOURCE_DIR}/lib/engine/tests/include
    ${CMAKE_SOURCE_DIR}/game
    ${CMAKE_SOURCE_DIR}/game/colony
)

# MSVC module workaround for __tlregdtor C++ linkage issue with thread_local destructors in modules
if(MSVC AND PPR_ENABLE_SANITIZER_ADDRESS)
    target_link_options(engine.tests.app PRIVATE "/alternatename:?__tlregdtor@@YAHXZ=__tlregdtor")
endif()

# Renderer resolves shader assets through the executable's content directory;
# the UI service reads its pinned font set from the staged `fonts` tree.
ppr_stage_assets(engine.tests.app shaders fonts)

# Phase 3: CTest entries share the target by resource class instead of
# serializing every app shard. Each shard invokes the same exe with
# --run-test <comma-list> (plus an optional --skip-test exclusion list) and
# --loop PPR_TEST_LOOPS (default 1); ppr_add_test_entry names every entry
# `engine.tests.app.<shard>`. Filter matching
# is prefix-based in both directions (Core.UnitTest.cpp filterMatches), so
# tokens are full `app/<group>` paths; `app/player/` carries a trailing slash
# to cover the player group WITHOUT prefix-matching the sibling
# app/player_service and app/player_graph groups (a bare `app/player` token
# would execute both siblings in a second shard). death_lifecycle and
# slow_gates split run_gate by leaf: full `app/run_gate/<leaf>` tokens keep
# prefix rules exact. death_lifecycle owns the death lifecycle (3
# death-subprocess parents + the unarmed child no-op run in the same process
# so their temp marker files stay uncontended) plus the fast lifecycle
# leaves; slow_gates owns the slow gates; display_window owns the
# display_window_ family, including placement regressions; the prefix keeps
# that filter under the 1024-character harness limit.
# Every top-level recurse child in App.Tests.cpp appears in exactly one shard
# (41/41), and the 33 run_gate leaves are partitioned across death_lifecycle,
# slow_gates, and display_window. The 15 flat app/* leaves
# (renderer_*/imgui_live_*/app_*) are split across services_cpu,
# services_gpu, and services_window by their resource use.
# GPU-backed shards claim the `gpu` resource lock; window/display shards also
# claim `window`, so CTest schedules CPU-only shards alongside them instead of
# globally serializing the target.

# death_lifecycle ~66s loop-1 dev (death x3 ~16s + saturated 11.0s +
# terminal 10.4s + focus 10.8s + busy 9.6s + precedence 3.0s + removal
# 3.1s + sim 2.3s + child no-op); highest budget. The gpu lock keeps its
# GPU-backed phases from contending with other device users.
ppr_add_test_entry(TARGET engine.tests.app
    NAME death_lifecycle
    FILTER "app/run_gate/unknown_completion_fail_stops_process,app/run_gate/unknown_completion_diag_fail_still_fail_stops,app/run_gate/unknown_completion_message_throw_still_fail_stops,app/run_gate/saturated_run_preserves_time_and_input,app/run_gate/terminal_probe_runs_zero_client_work,app/run_gate/busy_collection_failure_latches_first,app/run_gate/focus_loss_gain_ordered_across_busy,app/run_gate/first_error_precedence_clock_over_terminal,app/run_gate/removal_confirmed_preserves_first_error,app/run_gate/sim_catch_up_preserves_skipped_time,app/run_gate/unknown_completion_child_fatal,app/run_gate/monitor_change_and_revert_report_operational_errors"
    LOOPS
    RESOURCE_LOCK gpu
    TIMEOUT 600
    LABELS unit engine app
)

# slow_gates ~68s loop-1 dev (two_runs 20.7s + healthy 10.0s + diag_fail
# 11.7s + lane_block 11.6s + lane_paused 10.7s + rejected 3.2s +
# timeout_overlap 0.4s); highest budget.
ppr_add_test_entry(TARGET engine.tests.app
    NAME slow_gates
    FILTER "app/run_gate/two_runs_reestablish_quiescence_independently,app/run_gate/healthy_teardown_releases_in_order,app/run_gate/healthy_teardown_survives_diagnostic_failure,app/run_gate/lane_gate_blocks_third_until_retirement,app/run_gate/lane_gate_paused_worker_blocks_third,app/run_gate/rejected_frame_does_not_inherit_previous_submit_fate,app/run_gate/lane_gate_timeout_overlapping_gate_entry_serialized,app/run_gate/throwing_native_pump_and_wait_shutdown_retains_first_error,app/run_gate/shutdown_drops_queued_frames_before_backend_quarantine,app/run_gate/admission_observations_report_skips_with_explicit_time,app/run_gate/paced_credit_retries_preserve_elapsed_deadline"
    LOOPS
    RESOURCE_LOCK gpu
    TIMEOUT 600
    LABELS unit engine app
)

# Timing-sensitive display/window and lane tests claim both the device and the
# native window state; CTest still runs CPU-only shards alongside them.
ppr_add_test_entry(TARGET engine.tests.app
    NAME display_window
    FILTER "app/run_gate/display_window_"
    LOOPS
    RESOURCE_LOCK gpu window
    TIMEOUT 600
    LABELS unit engine app
)

# GPU functional groups create device resources; the gpu lock serializes them
# against other device users without holding back CPU-only shards.
ppr_add_test_entry(TARGET engine.tests.app
    NAME functional_gpu
    FILTER "app/grid_pass,app/pixel_readback"
    LOOPS
    RESOURCE_LOCK gpu
    TIMEOUT 600
    LABELS unit engine app
)

# CPU functional groups: sim_packet 16.9s + sub-second groups ~0.5s live
# loop-1; higher budget. No device or window state, so no lock.
ppr_add_test_entry(TARGET engine.tests.app
    NAME functional_cpu
    FILTER "app/devices,app/player/,app/dispatch,app/snapshot,app/player_graph,app/viewport,app/render_view,app/camera,app/display,app/sim_packet,app/quaternion,app/input_listener,app/filtered_analog,app/imgui_dpi,app/performance_widget,app/zerov_probe"
    LOOPS
    TIMEOUT 600
    LABELS unit engine app
)

# Timing-bound Gate-0 pacing evidence (frame_stages 200ms bound) runs alone
# with a short budget.
ppr_add_test_entry(TARGET engine.tests.app
    NAME frame_stages
    FILTER "app/frame_stages"
    LOOPS
    RUN_SERIAL
    TIMEOUT 120
    LABELS unit engine app
)

# frame_fence 14.0s live loop-1; higher budget. Device-backed, so it claims
# the gpu lock rather than serializing the whole target.
ppr_add_test_entry(TARGET engine.tests.app
    NAME frame_fence
    FILTER "app/frame_fence"
    LOOPS
    RESOURCE_LOCK gpu
    TIMEOUT 600
    LABELS unit engine app
)

# shader 3.3s + renderer reinit/off-thread + imgui_live shutdown/reinit are
# GPU-backed and claim the gpu lock.
ppr_add_test_entry(TARGET engine.tests.app
    NAME services_gpu
    FILTER "app/shader,app/renderer_triangle_reinit_ok,app/renderer_off_thread_fails_closed,app/imgui_live_shutdown_idempotent,app/imgui_live_same_object_reinit"
    LOOPS
    RESOURCE_LOCK gpu
    TIMEOUT 300
    LABELS unit engine app
)

# window_input 4.2s + imgui_routing 3.1s + player_service 1.3s live loop-1;
# these touch native window and device state, so they claim both locks.
ppr_add_test_entry(TARGET engine.tests.app
    NAME services_window
    FILTER "app/window_input,app/imgui_routing,app/player_service"
    LOOPS
    RESOURCE_LOCK gpu window
    TIMEOUT 300
    LABELS unit engine app
)

# render_lane 0.4s + the 11 flat app/* leaves ~5.9s live loop-1; CPU-only, so
# no lock.
ppr_add_test_entry(TARGET engine.tests.app
    NAME services_cpu
    FILTER "app/render_lane,app/app_init_rollback_on_window_create_failure,app/app_init_rollback_on_input_connect_failure,app/app_teardown_double_shutdown_no_refire,app/app_teardown_reports_subsystem_failure_but_completes,app/app_preserves_pre_registered_graphics_services,app/app_empty_request_exit_signals_exit_without_error,app/app_error_request_exit_latches_cause,app/app_empty_request_exit_run_returns_success,app/app_error_request_exit_run_reports_cause,app/app_headless_lifecycle_is_idempotent,app/app_run_shutdowns_after_nonstandard_hook_exception"
    LOOPS
    TIMEOUT 300
    LABELS unit engine app
)

# GPU validation entry: main.cpp pins both RHI validation modes off unless
# PPR_TEST_GPU_VALIDATION=1, so the debug core/GPU-assisted layers reach the
# real-GPU frame_fence paths only through this entry's environment. The
# RESOURCE_LOCK keeps it from contending with the asset validation entry
# without globally serializing CTest.
ppr_add_test_entry(TARGET engine.tests.app
    NAME gpu_validation
    FILTER "app/frame_fence"
    LOOPS
    RESOURCE_LOCK gpu
    TIMEOUT 300
    LABELS unit engine app gpu_validation
)

set_tests_properties(engine.tests.app.gpu_validation PROPERTIES
    ENVIRONMENT "PPR_TEST_GPU_VALIDATION=1"
)
