add_executable(engine.tests.app main.cpp)

target_sources(engine.tests.app
    PUBLIC
        FILE_SET CXX_MODULES BASE_DIRS ${CMAKE_SOURCE_DIR} FILES
            App.Tests.cppm

            # F1 game-payload tests compile the colony subset directly: the
            # sim-carrying packet lives in game/ (engine.app must not link
            # sim/physics), so its unit tests ship with the modules, not via
            # a link to the app.game executable. WorldGen is included as a
            # leaf (core/sim deps only, already linked); Panel, Smoke and
            # TimeTrials stay out (unneeded here; smoke stays self-hosted).
            ${CMAKE_SOURCE_DIR}/game/colony/Colony.cppm
            ${CMAKE_SOURCE_DIR}/game/colony/WorldGen.cppm
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
        App.GridPass.Tests.cpp
        App.Frame.Tests.cpp
        App.FrameFence.Tests.cpp
        App.RenderLane.Tests.cpp
        App.RunGate.Tests.cpp
        App.Quaternion.Tests.cpp
        App.ImGuiRouting.Tests.cpp
        App.ImguiDpi.Tests.cpp
        App.ZeroVProbe.Tests.cpp

        ${CMAKE_SOURCE_DIR}/game/colony/Colony.cpp
        ${CMAKE_SOURCE_DIR}/game/colony/WorldGen.cpp
        ${CMAKE_SOURCE_DIR}/game/colony/ColonyDriver.cpp
        ${CMAKE_SOURCE_DIR}/game/colony/DigTool.cpp
        ${CMAKE_SOURCE_DIR}/game/colony/Pathfinding.cpp
        ${CMAKE_SOURCE_DIR}/game/colony/ColonyTranslator.cpp
        ${CMAKE_SOURCE_DIR}/game/colony/Agents.cpp
        ${CMAKE_SOURCE_DIR}/game/colony/Buildings.cpp
        ${CMAKE_SOURCE_DIR}/game/colony/SimFramePacket.cpp
    )

setup_ppr_project(engine.tests.app
    INTERNAL_PUBLIC_DEPS engine.app engine.sim engine.tests
    EXTERNAL_SYSTEM_PRIVATE_DEPS glfw
)

# ColonyDriver links physics privately (same shape as engine.tests.sim);
# the colony headers include StageTiming.h/Colony.Elements.h by relative name.
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

# Copy shader assets next to the test executable so Renderer can find them via content_dir
add_custom_command(TARGET engine.tests.app POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets/shaders"
        "$<TARGET_FILE_DIR:engine.tests.app>/shaders"
        COMMENT "Copying shader assets"
)

# Phase 2.5 A1: EngineAppUnitTests split into 5 parallel-safe ctest shards.
# Each shard invokes the same exe with --run-test <comma-list> at the default
# loop count (ASan 3-loop rigor preserved; no --loop override). Filter matching
# is prefix-based in both directions (Core.UnitTest.cpp filterMatches), so
# tokens are full `app/<group>` paths; `app/player/` carries a trailing slash
# to cover the player group WITHOUT prefix-matching the sibling
# app/player_service and app/player_graph groups (a bare `app/player` token
# would execute both siblings in a second shard). Shards app_a1/a2 split
# run_gate by leaf: full `app/run_gate/<leaf>` tokens keep prefix rules
# exact. app_a1 owns the death lifecycle (3 death-subprocess parents + the
# unarmed child no-op run in the same process so their temp marker files stay
# uncontended) plus the fast lifecycle leaves; app_a2 owns the slow gates.
# Every recurse child in App.Tests.cpp appears in exactly one shard (39/39);
# the 15 flat app/* leaves (renderer_*/imgui_live_*/app_*,
# owned by the WindowInput/PixelReadback TUs) ride with app_d_io.
add_test(NAME app_a1_death_lifecycle COMMAND engine.tests.app --run-test
    "app/run_gate/unknown_completion_fail_stops_process,app/run_gate/unknown_completion_diag_fail_still_fail_stops,app/run_gate/unknown_completion_message_throw_still_fail_stops,app/run_gate/saturated_run_preserves_time_and_input,app/run_gate/terminal_probe_runs_zero_client_work,app/run_gate/busy_collection_failure_latches_first,app/run_gate/focus_loss_gain_ordered_across_busy,app/run_gate/first_error_precedence_clock_over_terminal,app/run_gate/removal_confirmed_preserves_first_error,app/run_gate/sim_catch_up_preserves_skipped_time,app/run_gate/unknown_completion_child_fatal")

add_test(NAME app_a2_gates COMMAND engine.tests.app --run-test
    "app/run_gate/two_runs_reestablish_quiescence_independently,app/run_gate/healthy_teardown_releases_in_order,app/run_gate/healthy_teardown_survives_diagnostic_failure,app/run_gate/lane_gate_blocks_third_until_retirement,app/run_gate/lane_gate_paused_worker_blocks_third,app/run_gate/rejected_frame_does_not_inherit_previous_submit_fate,app/run_gate/lane_gate_timeout_overlapping_gate_entry_serialized")

add_test(NAME app_b_sim COMMAND engine.tests.app --run-test
    "app/devices,app/player/,app/dispatch,app/snapshot,app/player_graph,app/viewport,app/render_view,app/grid_pass,app/pixel_readback,app/camera,app/frame_stages,app/sim_packet,app/quaternion,app/input_listener,app/filtered_analog,app/imgui_dpi,app/zerov_probe")

add_test(NAME app_c_fence COMMAND engine.tests.app --run-test "app/frame_fence")

add_test(NAME app_d_io COMMAND engine.tests.app --run-test
    "app/player_service,app/shader,app/renderer_triangle_reinit_ok,app/renderer_off_thread_fails_closed,app/imgui_live_shutdown_idempotent,app/imgui_live_same_object_reinit,app/app_init_rollback_on_window_create_failure,app/app_init_rollback_on_input_connect_failure,app/app_teardown_double_shutdown_no_refire,app/app_teardown_reports_subsystem_failure_but_completes,app/app_preserves_pre_registered_graphics_services,app/app_empty_request_exit_signals_exit_without_error,app/app_error_request_exit_latches_cause,app/app_empty_request_exit_run_returns_success,app/app_error_request_exit_run_reports_cause,app/app_headless_lifecycle_is_idempotent,app/app_run_shutdowns_after_nonstandard_hook_exception,app/render_lane,app/window_input,app/imgui_routing")

set_tests_properties(app_a1_death_lifecycle
    PROPERTIES
        # a1 death_lifecycle ~66s loop-1 dev (death x3 ~16s + saturated 11.0s +
        # terminal 10.4s + focus 10.8s + busy 9.6s + precedence 3.0s + removal
        # 3.1s + sim 2.3s + child no-op); highest budget.
        TIMEOUT 600
        LABELS "unit;engine;app"
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:engine.tests.app>"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:engine.tests.app>"
)

set_tests_properties(app_a2_gates
    PROPERTIES
        # a2 gates ~68s loop-1 dev (two_runs 20.7s + healthy 10.0s + diag_fail
        # 11.7s + lane_block 11.6s + lane_paused 10.7s + rejected 3.2s +
        # timeout_overlap 0.4s); highest budget.
        TIMEOUT 600
        LABELS "unit;engine;app"
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:engine.tests.app>"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:engine.tests.app>"
)

set_tests_properties(app_b_sim
    PROPERTIES
        # sim_packet 16.9s + sub-second groups ~0.5s live loop-1; higher budget.
        # Timing-bound Gate-0 pacing evidence (frame_stages 200ms bound) runs
        # uncontended; standard command stays `ctest -j4`; other 7 entries
        # still parallelize.
        RUN_SERIAL TRUE
        TIMEOUT 600
        LABELS "unit;engine;app"
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:engine.tests.app>"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:engine.tests.app>"
)

set_tests_properties(app_c_fence
    PROPERTIES
        # frame_fence 14.0s live loop-1; higher budget.
        TIMEOUT 600
        LABELS "unit;engine;app"
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:engine.tests.app>"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:engine.tests.app>"
)

set_tests_properties(app_d_io
    PROPERTIES
        # window_input 4.2s + shader 3.3s + imgui_routing 3.1s + player_service
        # 1.3s + render_lane 0.4s + 15 flat app/* leaves ~5.9s live loop-1.
        TIMEOUT 300
        LABELS "unit;engine;app"
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:engine.tests.app>"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:engine.tests.app>"
)
