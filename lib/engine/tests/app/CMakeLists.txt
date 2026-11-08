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
            ${CMAKE_SOURCE_DIR}/game/colony/Tools.cppm
            ${CMAKE_SOURCE_DIR}/game/colony/Metric.cppm
            ${CMAKE_SOURCE_DIR}/game/colony/ColonyDriver.cppm
            ${CMAKE_SOURCE_DIR}/game/colony/MatterRuntime.cppm
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
        ${CMAKE_SOURCE_DIR}/game/colony/Colony.AuthoredFixture.Tests.h
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
        App.Lighting.Tests.cpp
        App.RadianceGpu.Tests.cpp
        App.WorldRadiance.Tests.cpp
        App.WorldLightField.Tests.cpp
        App.Frame.Tests.cpp
        App.FrameFence.Tests.cpp
        App.RenderLane.Tests.cpp
        App.RunGate.Tests.cpp
        App.Quaternion.Tests.cpp
        App.ImGuiRouting.Tests.cpp
        App.ImguiDpi.Tests.cpp
        App.PerformanceWidget.Tests.cpp
        App.ZeroVProbe.Tests.cpp
        App.MatterGpu.Tests.cpp
        App.MatterFork.Tests.cpp
        App.MatterFork.Oracle.h
        App.MatterRuntime.Tests.cpp

        ${CMAKE_SOURCE_DIR}/game/colony/Colony.cpp
        ${CMAKE_SOURCE_DIR}/game/colony/WorldGen.cpp
        ${CMAKE_SOURCE_DIR}/game/colony/WorldGen.Plan.cpp
        ${CMAKE_SOURCE_DIR}/game/colony/WorldGen.Export.cpp
        ${CMAKE_SOURCE_DIR}/game/colony/Content.cpp
        ${CMAKE_SOURCE_DIR}/game/colony/Content.Pack.cpp
        ${CMAKE_SOURCE_DIR}/game/colony/Content.Source.cpp
        ${CMAKE_SOURCE_DIR}/game/colony/Content.Descriptors.cpp
        ${CMAKE_SOURCE_DIR}/game/colony/Content.Assembly.cpp
        ${CMAKE_SOURCE_DIR}/game/colony/Content.Presentation.cpp
        ${CMAKE_SOURCE_DIR}/game/colony/Tools.cpp
        ${CMAKE_SOURCE_DIR}/game/colony/Metric.cpp
        ${CMAKE_SOURCE_DIR}/game/colony/ColonyDriver.cpp
        ${CMAKE_SOURCE_DIR}/game/colony/ColonyDriver.Matter.cpp
        ${CMAKE_SOURCE_DIR}/game/colony/MatterRuntime.cpp
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
target_link_libraries(engine.tests.app PRIVATE engine.physics engine.matter)

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

# Experimental group exclusion is unconditional, including when its CTest option is OFF.
# Direct per-leaf filters remain available without exposing GPU setup to CPU shards.
set(ppr_world_candidate_filter "app/world_radiance_gpu")

# Keep the measured source-kernel/extension group within its existing ceiling.
# Transfer, contact, pool and dam continuations own exact leaves, excluded from that group.
# Pinned-fork conformance uses its own exact group, leaving old workloads/deadlines intact.
# The300s ceiling matches the existing matter_gpu and matter_resident_gpu device gates.
ppr_add_test_entry(TARGET engine.tests.app
    NAME matter_fork_gpu
    FILTER "app/matter_fork"
    EXCLUDE "${ppr_world_candidate_filter}"
    LOOPS
    RESOURCE_LOCK gpu
    TIMEOUT 300
    LABELS unit engine app gpu_validation
)

set(ppr_matter_resident_filter "app/matter_gpu/resident_status_and_frozen_snapshot_preserve_full_state_without_feedback,app/matter_gpu/publication_validation_checks_every_scalar_and_matches_upload_admission,app/matter_gpu/resident_append_copies_the_current_prefix_and_uploads_only_new_rows,app/matter_runtime_gpu/resident_completion_keeps_metadata_and_freezes_exact_demand_snapshot")

ppr_add_test_entry(TARGET engine.tests.app
    NAME matter_gpu
    FILTER "app/matter_gpu"
    EXCLUDE "${ppr_matter_resident_filter},${ppr_world_candidate_filter}"
    LOOPS
    RESOURCE_LOCK gpu
    TIMEOUT 300
    LABELS unit engine app gpu_validation
)

ppr_add_test_entry(TARGET engine.tests.app
    NAME matter_resident_gpu
    FILTER "${ppr_matter_resident_filter}"
    EXCLUDE "${ppr_world_candidate_filter}"
    LOOPS
    RESOURCE_LOCK gpu
    TIMEOUT 300
    LABELS unit engine app gpu_validation
)

unset(ppr_matter_resident_filter)

# Numeric adapters need no device. Actual-domain pending physics owns a separate
# GPU entry rather than extending the already measured source-kernel group.
ppr_add_test_entry(TARGET engine.tests.app
    NAME matter_adapter
    FILTER "app/matter_adapter"
    EXCLUDE "${ppr_world_candidate_filter}"
    LOOPS
    TIMEOUT 120
    LABELS unit engine app cpu
)

ppr_add_test_entry(TARGET engine.tests.app
    NAME matter_runtime_gpu
    FILTER "app/matter_runtime_gpu"
    EXCLUDE "app/matter_runtime_gpu/dedicated_actual_domain_cold_reused_acknowledgment_and_completion_are_distinct,app/matter_runtime_gpu/resident_completion_keeps_metadata_and_freezes_exact_demand_snapshot,${ppr_world_candidate_filter}"
    LOOPS
    RESOURCE_LOCK gpu
    TIMEOUT 180
    LABELS unit engine app gpu_validation
)

ppr_add_test_entry(TARGET engine.tests.app
    NAME matter_dedicated_gpu
    FILTER "app/matter_runtime_gpu/dedicated_actual_domain_cold_reused_acknowledgment_and_completion_are_distinct"
    EXCLUDE "${ppr_world_candidate_filter}"
    LOOPS
    RESOURCE_LOCK gpu
    TIMEOUT 180
    LABELS unit engine app gpu_validation
)


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
# The primary filters partition the leaves under every App.Tests.cpp recurse child;
# the 33 run_gate leaves are partitioned across death_lifecycle,
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
    EXCLUDE "${ppr_world_candidate_filter}"
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
    EXCLUDE "${ppr_world_candidate_filter}"
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
    EXCLUDE "${ppr_world_candidate_filter}"
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
    EXCLUDE "${ppr_world_candidate_filter}"
    LOOPS
    RESOURCE_LOCK gpu
    TIMEOUT 600
    LABELS unit engine app
)

# CPU functional groups: sim_packet 16.9s + sub-second groups ~0.5s live
# loop-1; higher budget. No device or window state, so no lock.
ppr_add_test_entry(TARGET engine.tests.app
    NAME functional_cpu
    FILTER "app/devices,app/player/,app/dispatch,app/snapshot,app/player_graph,app/viewport,app/render_view,app/lighting,app/camera,app/display,app/sim_packet,app/quaternion,app/input_listener,app/filtered_analog,app/imgui_dpi,app/performance_widget,app/zerov_probe"
    EXCLUDE "${ppr_world_candidate_filter}"
    LOOPS
    TIMEOUT 600
    LABELS unit engine app
)

# Timing-bound Gate-0 pacing evidence (frame_stages 200ms bound) runs alone
# with a short budget.
ppr_add_test_entry(TARGET engine.tests.app
    NAME frame_stages
    FILTER "app/frame_stages"
    EXCLUDE "${ppr_world_candidate_filter}"
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
    EXCLUDE "${ppr_world_candidate_filter}"
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
    EXCLUDE "${ppr_world_candidate_filter}"
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
    EXCLUDE "${ppr_world_candidate_filter}"
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
    EXCLUDE "${ppr_world_candidate_filter}"
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
    EXCLUDE "${ppr_world_candidate_filter}"
    LOOPS
    RESOURCE_LOCK gpu
    TIMEOUT 300
    LABELS unit engine app gpu_validation
)

set_tests_properties(engine.tests.app.gpu_validation PROPERTIES
    ENVIRONMENT "PPR_TEST_GPU_VALIDATION=1"
)

# Optical transport leaves are partitioned identically in routine and GPUAV
# entries. Exact leaf tokens keep the partitions disjoint under prefix matching.
# Slow terrain leaves own their deadlines; the two terrain-field scenarios
# share one live producer per loop below. Grid/pixel work stays in functional_gpu.
# All filters stay below the runner's 1024-character limit.
# The CPU comparison is a separately admitted diagnostic, never a GPU workload.
# Keep it explicitly excluded even if a routine filter later broadens to the group.
set(ppr_radiance_cpu_comparison_filter "app/radiance_gpu/terrain_background_cpu_nearfield_dependency_comparison")
option(PPR_ENABLE_TERRAIN_CPU_PHYSICAL_COMPARISON
    "Register the CPU-only fixed-point near-field RC comparison diagnostic" OFF)
if(PPR_ENABLE_TERRAIN_CPU_PHYSICAL_COMPARISON)
    ppr_add_test_entry(TARGET engine.tests.app
        NAME radiance_terrain_field_cpu_comparison
        FILTER "${ppr_radiance_cpu_comparison_filter}"
        EXCLUDE "${ppr_world_candidate_filter}"
        LOOPS
        TIMEOUT 300
        LABELS qualification engine app cpu_diagnostic
    )
endif()

set(ppr_radiance_gpu_filter "app/radiance_gpu/fp32_scalar_trace_matches_quantized_optical_oracle,app/radiance_gpu/admission_quantization_and_abandoned_batches_preserve_safe_reuse,app/radiance_gpu/generic_cell_positive_underflow_never_returns_a_complete_interval,app/radiance_gpu/material_optics_palette_uses_actual96_byte_shader_rows,app/radiance_gpu/fp64_runtime_arithmetic_preserves_double_precision,app/radiance_gpu/terrain_receiver_continuation_kernel_preserves_sites_and_cell_seams,app/radiance_gpu/terrain_receiver_continuation_experiment_respects_blocked_side_quality_gate")
set(ppr_radiance_hierarchy_gpu_filter "app/radiance_gpu/vacuum_hierarchy_preserves_optics_and_rebuilds_captured_edits,app/radiance_gpu/fp32_cascades_match_double_schedule_and_degenerate_axis_support,app/radiance_gpu/bin_normalization_and_independent_direct_ray_error_are_distinct")
set(ppr_radiance_packed_gpu_filter "app/radiance_gpu/packed_half_rounding_endpoints_admission_and_visible_overflow,app/radiance_gpu/packed_cascades_match_per_level_half_reference_and_report_storage_error")
set(ppr_radiance_terrain_trace_gpu_filter "app/radiance_gpu/rounded_terrain_trace_matches_analytic_sources_and_immutable_rebuilds")
set(ppr_radiance_terrain_ambiguity_gpu_filter "app/radiance_gpu/rounded_terrain_near_tangent_and_collinear_queries_report_ambiguity")
set(ppr_radiance_terrain_cascades_gpu_filter "app/radiance_gpu/rounded_terrain_fp32_cascades_reuse_the_declared_rc_schedule")
set(ppr_radiance_coefficient_gpu_filter "app/radiance_gpu/canonical_roundness_bits_match_actual_raster_and_terrain_paths")
set(ppr_radiance_mapped_diffuse_gpu_filter "app/radiance_gpu/mapped_diffuse_uses_actual_angular_field_and_elevation_lift")
set(ppr_radiance_precision_admission_gpu_filter "app/radiance_gpu/terrain_precision_selection_rejects_unqualified_sessions_before_initialization")
set(ppr_radiance_precision_source_gpu_filter "app/radiance_gpu/terrain_precision_source_bridge_preserves_fp32_ownership_and_counter_settlement")

foreach(ppr_radiance_shard IN ITEMS gpu hierarchy_gpu packed_gpu terrain_trace_gpu terrain_ambiguity_gpu terrain_cascades_gpu coefficient_gpu mapped_diffuse_gpu precision_admission_gpu)
    ppr_add_test_entry(TARGET engine.tests.app
        NAME radiance_${ppr_radiance_shard}
        FILTER "${ppr_radiance_${ppr_radiance_shard}_filter}"
        EXCLUDE "${ppr_radiance_cpu_comparison_filter},${ppr_world_candidate_filter}"
        LOOPS
        RESOURCE_LOCK gpu
        TIMEOUT 300
        LABELS unit engine app
    )

    ppr_add_test_entry(TARGET engine.tests.app
        NAME radiance_${ppr_radiance_shard}_validation
        FILTER "${ppr_radiance_${ppr_radiance_shard}_filter}"
        EXCLUDE "${ppr_radiance_cpu_comparison_filter},${ppr_world_candidate_filter}"
        LOOPS
        RESOURCE_LOCK gpu
        TIMEOUT 300
        LABELS unit engine app gpu_validation
    )

    set_tests_properties(engine.tests.app.radiance_${ppr_radiance_shard}_validation PROPERTIES
        ENVIRONMENT "PPR_TEST_GPU_VALIDATION=1"
    )

    unset(ppr_radiance_${ppr_radiance_shard}_filter)
endforeach()
unset(ppr_radiance_shard)

# One provisional cold qualification bound: five native terrain entries can
# individually take 400-525s; 3000s rounds 5*525s + 300s warm scenarios/teardown.
# This is an operational whole-process bound, not a GPU performance allowance.
# Existing producer timestamps and per-leaf verdicts separate cold compilation
# from warm work; the runner watchdog continues to report stalls independently.
set(ppr_radiance_terrain_field_gpu_filter "app/radiance_gpu/terrain_receivers_reconstruct_c1_and_retain_completed_generation_leases,app/radiance_gpu/terrain_background_view_preserves_world_visibility_samples_and_leases")
foreach(ppr_field_mode IN ITEMS ordinary validation)
    set(ppr_field_entry "radiance_terrain_field_gpu")
    if(ppr_field_mode STREQUAL "validation")
        string(APPEND ppr_field_entry "_validation")
    endif()

    ppr_add_test_entry(TARGET engine.tests.app
        NAME ${ppr_field_entry}
        FILTER "${ppr_radiance_terrain_field_gpu_filter}"
        EXCLUDE "${ppr_radiance_cpu_comparison_filter},${ppr_world_candidate_filter}"
        LOOPS
        RESOURCE_LOCK gpu
        TIMEOUT 3000
        LABELS unit engine app
    )
    set(ppr_field_validation 0)
    if(ppr_field_mode STREQUAL "validation")
        set(ppr_field_validation 1)
    endif()

    set_tests_properties(engine.tests.app.${ppr_field_entry} PROPERTIES
        ENVIRONMENT "PPR_TEST_TERRAIN_PRODUCTION_FP64=0;PPR_TEST_RADIANCE_CASCADE_DIAGNOSTIC=1;PPR_TEST_GPU_VALIDATION=${ppr_field_validation}"
    )
    if(ppr_field_mode STREQUAL "validation")
        set_property(TEST engine.tests.app.${ppr_field_entry}
            APPEND PROPERTY LABELS gpu_validation)
    endif()
endforeach()
unset(ppr_field_entry)
unset(ppr_field_mode)
unset(ppr_field_validation)
unset(ppr_radiance_terrain_field_gpu_filter)

# Explicit checked-session qualification only. Keep OFF in ordinary configure/
# aggregate matrices; the coordinator enables this for the exact300s source and
# 600s receiver entries, then restores OFF. The option never supplies the operator's
# PPR_TEST_TERRAIN_PRODUCTION_FP64=1 RTE64 assertion or makes a capability claim.
option(PPR_ENABLE_TERRAIN_PRODUCTION_FP64_QUALIFICATION
    "Register opt-in production FP64 source (300s) and receiver (600s) qualification" OFF)
if(PPR_ENABLE_TERRAIN_PRODUCTION_FP64_QUALIFICATION)
    foreach(ppr_precision_mode IN ITEMS ordinary validation)
        set(ppr_precision_source_entry "radiance_precision_source_gpu")
        if(ppr_precision_mode STREQUAL "validation")
            string(APPEND ppr_precision_source_entry "_validation")
        endif()
        ppr_add_test_entry(TARGET engine.tests.app
            NAME ${ppr_precision_source_entry}
            FILTER "${ppr_radiance_precision_source_gpu_filter}"
            EXCLUDE "${ppr_radiance_cpu_comparison_filter},${ppr_world_candidate_filter}"
            LOOPS
            RESOURCE_LOCK gpu
            TIMEOUT 300
            LABELS qualification engine app precision_qualification
        )

        set(ppr_precision_entry "radiance_receiver_production_fp64_qualification_${ppr_precision_mode}")
        ppr_add_test_entry(TARGET engine.tests.app
            NAME ${ppr_precision_entry}
            FILTER "app/radiance_gpu/terrain_receivers_reconstruct_c1_and_retain_completed_generation_leases"
            EXCLUDE "${ppr_radiance_cpu_comparison_filter},${ppr_world_candidate_filter}"
            LOOPS
            RESOURCE_LOCK gpu
            TIMEOUT 600
            LABELS qualification engine app precision_qualification
        )
        if(ppr_precision_mode STREQUAL "validation")
            set_property(TEST engine.tests.app.${ppr_precision_source_entry}
                APPEND PROPERTY ENVIRONMENT "PPR_TEST_GPU_VALIDATION=1")
            set_property(TEST engine.tests.app.${ppr_precision_source_entry}
                APPEND PROPERTY LABELS gpu_validation)
            set_property(TEST engine.tests.app.${ppr_precision_entry}
                APPEND PROPERTY ENVIRONMENT "PPR_TEST_GPU_VALIDATION=1")
            set_property(TEST engine.tests.app.${ppr_precision_entry}
                APPEND PROPERTY LABELS gpu_validation)
        endif()
    endforeach()
    unset(ppr_precision_source_entry)
    unset(ppr_precision_entry)
    unset(ppr_precision_mode)
endif()
unset(ppr_radiance_precision_source_gpu_filter)

unset(ppr_radiance_cpu_comparison_filter)

# Explicit candidate quality experiment, separate from the legacy diagnostic
# producer and normal activation. Both entries use one shared device/producer.
option(PPR_TEST_WORLD_RADIANCE_CANDIDATE "Register the bounded native-cell RC candidate experiment" OFF)
if(PPR_TEST_WORLD_RADIANCE_CANDIDATE)
    foreach(ppr_world_mode IN ITEMS ordinary validation)
        set(ppr_world_name world_radiance_candidate_gpu)
        set(ppr_world_validation 0)
        set(ppr_world_labels unit engine app experiment)
        if(ppr_world_mode STREQUAL "validation")
            string(APPEND ppr_world_name "_validation")
            set(ppr_world_validation 1)
            list(APPEND ppr_world_labels gpu_validation)
        endif()
        ppr_add_test_entry(TARGET engine.tests.app
            NAME ${ppr_world_name}
            FILTER "${ppr_world_candidate_filter}"
            # The temporal sequence has its own independently timed entry below.
            EXCLUDE "app/world_radiance_gpu/temporal_world_field_sequence"
            LOOPS
            RESOURCE_LOCK gpu
            TIMEOUT 300
            LABELS ${ppr_world_labels}
        )
        set_tests_properties(engine.tests.app.${ppr_world_name} PROPERTIES
            ENVIRONMENT "PPR_TEST_GPU_VALIDATION=${ppr_world_validation}"
            FAIL_REGULAR_EXPRESSION "VUID-|Validation Error"
        )
        ppr_add_test_entry(TARGET engine.tests.app
            NAME ${ppr_world_name}_temporal
            FILTER "app/world_radiance_gpu/temporal_world_field_sequence"
            LOOPS
            RESOURCE_LOCK gpu
            TIMEOUT 300
            LABELS ${ppr_world_labels}
        )
        set_tests_properties(engine.tests.app.${ppr_world_name}_temporal PROPERTIES
            ENVIRONMENT "PPR_TEST_GPU_VALIDATION=${ppr_world_validation}"
            FAIL_REGULAR_EXPRESSION "VUID-|Validation Error"
        )
    endforeach()
    unset(ppr_world_mode)
    unset(ppr_world_name)
    unset(ppr_world_validation)
    unset(ppr_world_labels)
endif()

# World light field plan and (later) GPU cases. CPU plan cases take the gpu
# lock today because the group will host device-backed field/source cases.
ppr_add_test_entry(TARGET engine.tests.app
    NAME world_light_field
    FILTER "app/world_light_field"
    LOOPS
    RESOURCE_LOCK gpu
    TIMEOUT 300
    LABELS unit engine app
)

unset(ppr_world_candidate_filter)
