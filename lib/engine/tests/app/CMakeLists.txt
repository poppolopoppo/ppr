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
        App.PostProcess.Tests.cpp
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
        App.WorldLighting.Backend.Tests.cpp
        App.WorldLighting.Codec.Tests.cpp
        App.WorldLighting.Projection.Tests.cpp
        App.WorldLighting.Trace.Tests.cpp

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
ppr_stage_assets(engine.tests.app shaders fonts lighting)

# Keep the measured source-kernel/extension group within its existing ceiling.
# Transfer, contact, pool and dam continuations own exact leaves, excluded from that group.
# Pinned-fork conformance uses its own exact group, leaving old workloads/deadlines intact.
# The 300 s ceiling matches the existing matter device gates.
ppr_add_test_entry(TARGET engine.tests.app
    NAME matter_fork_gpu
    FILTER "app/matter_fork"
    LOOPS
    RESOURCE_LOCK gpu
    TIMEOUT 300
    LABELS unit engine app gpu_validation
)

set(ppr_matter_resident_filter "app/matter_gpu/resident_status_and_frozen_snapshot_preserve_full_state_without_feedback,app/matter_gpu/publication_validation_checks_every_scalar_and_matches_upload_admission,app/matter_gpu/resident_append_copies_the_current_prefix_and_uploads_only_new_rows,app/matter_runtime_gpu/resident_completion_keeps_metadata_and_freezes_exact_demand_snapshot")

# The former single `matter_gpu` entry exceeded its 300 s deadline (300.52 s) under
# core + GPU-assisted validation: per-case device setup, a fresh Slang session that
# recompiles `solver.slang` for every case, and whole-grid dispatches under the
# validation layers. A group-scoped shader/device/queue owner was measured on this
# host and rejected: reusing one Slang session served the cached module to ten new
# entry-point programs per case, so that case's program creation measured 10x
# slower (2.09 s -> 21.3-22.1 s per solver), so cases keep private service
# lifetimes. The entry is split by existing responsibility instead.
#
# Every `app/matter_gpu` leaf must appear in exactly one explicit list below. The
# lists are explicit tokens, not group prefixes, so each entry's worst observed
# cost stays bounded: a new leaf is NOT auto-included and must be added here.
# The four lists are disjoint and cover exactly the 32 leaves the aggregate owned
# (2 terrain + 9 contact + 11 material + 10 grid), so GPU-validation coverage is
# unchanged. The two whole-grid terrain leaves are separated from the rest of the
# contact geometry because their joint worst observed bound co-resident with the
# contact set was 267.62 s (89 % of 300 s).
set(ppr_matter_terrain_filter "app/matter_gpu/terrain_rounded_v_endpoint_preserves_certified_contacts_and_resident_rest,app/matter_gpu/terrain_recovery_sweeps_and_rest_preserve_published_contact")

set(ppr_matter_contact_filter "app/matter_gpu/node_wgsl_conversion_preserves_endpoints_and_scatter_rejection,app/matter_gpu/bootstrap_grid_checks_preserve_fast_and_fallback_error_acceptance,app/matter_gpu/compact_terrain_gpu_decode_matches_wide_tables_masks_and_tile_bits,app/matter_gpu/terrain_endpoint_response_and_complete_boundary_search,app/matter_gpu/wave_boundary_search_matches_serial_for_full_mixed_groups,app/matter_gpu/moving_body_static_limit_translation_rotation_and_one_way_contact,app/matter_gpu/body_admission_rejection_and_same_epoch_contact_publication,app/matter_gpu/predicted_body_slices_select_distinct_substep_rows,app/matter_gpu/guardian_blocks_normal_and_halves_tangent_analytically")

set(ppr_matter_material_filter "app/matter_gpu/sand_reference_occupancy_compression_and_floor_controls_diagnostic,app/matter_gpu/sand_density_and_terrain_contact_instrumented_gpu_cost_diagnostic,app/matter_gpu/deterministic_1024_sparse_dense_scene,app/matter_gpu/sand_objective_volume_relaxes_compression_without_attraction_and_preserves_restart,app/matter_gpu/sand_terrain_friction_is_dissipative_and_preserves_body_and_guardian_order,app/matter_gpu/isolated_liquid_density_error_decays_without_affine_growth,app/matter_gpu/evolved_liquid_pool_has_no_interior_render_holes,app/matter_gpu/all_materials_preserve_inventory_and_publish_finite_state_at_k2,app/matter_gpu/k2_only_rejects_other_counts_before_work,app/matter_gpu/initialized_rest_gravity_and_publication,app/matter_gpu/all_four_absolute_budgets_reject_without_publication")

set(ppr_matter_grid_filter "app/matter_gpu/sparse_capacity_layout_and_independent_admission_bounds,app/matter_gpu/sparse_gpu_aprons_publish_unique_flat_keys_and_group_rows,app/matter_gpu/sparse_generations_poison_capacity_and_cross_group_budget_are_transactional,app/matter_gpu/opt_in_stage_timing_follows_completed_fence_and_epoch,app/matter_gpu/device_peak_footprint_for_dual_grid_ownership,app/matter_gpu/strict_conversion_and_collection_retry_preserve_cause,app/matter_gpu/prepared_replacement_accounts_peak_and_freezes_admission,app/matter_gpu/reflected_scalar_protocol_and_source_helpers,app/matter_gpu/split_device_rows_pack_and_unpack_every_semantic_field_by_name,app/matter_gpu/lossless_terrain_codecs_preserve_tables_tails_and_reject_invalid_input")

ppr_add_test_entry(TARGET engine.tests.app
    NAME matter_terrain_gpu
    FILTER "${ppr_matter_terrain_filter}"
    LOOPS
    RESOURCE_LOCK gpu
    TIMEOUT 300
    LABELS unit engine app gpu_validation
)

ppr_add_test_entry(TARGET engine.tests.app
    NAME matter_contact_gpu
    FILTER "${ppr_matter_contact_filter}"
    LOOPS
    RESOURCE_LOCK gpu
    TIMEOUT 300
    LABELS unit engine app gpu_validation
)

ppr_add_test_entry(TARGET engine.tests.app
    NAME matter_material_gpu
    FILTER "${ppr_matter_material_filter}"
    LOOPS
    RESOURCE_LOCK gpu
    TIMEOUT 300
    LABELS unit engine app gpu_validation
)

ppr_add_test_entry(TARGET engine.tests.app
    NAME matter_grid_gpu
    FILTER "${ppr_matter_grid_filter}"
    LOOPS
    RESOURCE_LOCK gpu
    TIMEOUT 300
    LABELS unit engine app gpu_validation
)

ppr_add_test_entry(TARGET engine.tests.app
    NAME matter_resident_gpu
    FILTER "${ppr_matter_resident_filter}"
    LOOPS
    RESOURCE_LOCK gpu
    TIMEOUT 300
    LABELS unit engine app gpu_validation
)

unset(ppr_matter_terrain_filter)
unset(ppr_matter_contact_filter)
unset(ppr_matter_material_filter)
unset(ppr_matter_grid_filter)
unset(ppr_matter_resident_filter)

# Numeric adapters need no device. Actual-domain pending physics owns a separate
# GPU entry rather than extending the already measured source-kernel group.
ppr_add_test_entry(TARGET engine.tests.app
    NAME matter_adapter
    FILTER "app/matter_adapter"
    LOOPS
    TIMEOUT 120
    LABELS unit engine app cpu
)

# Runtime lifecycle: the dedicated cold/reused acknowledgment leaf and the
# resident-completion leaf keep their own entries below/above, so the remaining
# leaves are two explicit responsibility lists. They are tokens, not
# `app/matter_runtime_gpu`, exactly like the matter shards: a new leaf is not
# auto-included, and no leaf can run in two entries. The split exists because
# this entry measured 177.55 s and 61.13 s against the 180 s deadline, so its
# scene/resident-tick leaf no longer shares a deadline with the publication and
# pending-pause leaves.
set(ppr_matter_runtime_filter "app/matter_runtime_gpu/terminal_collection_rejection_discards_without_joint_publication,app/matter_runtime_gpu/actual_domain_pending_pause_finishes_only_admitted_slice")

set(ppr_matter_runtime_scene_filter "app/matter_runtime_gpu/selected_map_edges_run_resident_ticks_and_frozen_snapshots")

ppr_add_test_entry(TARGET engine.tests.app
    NAME matter_runtime_gpu
    FILTER "${ppr_matter_runtime_filter}"
    LOOPS
    RESOURCE_LOCK gpu
    TIMEOUT 180
    LABELS unit engine app gpu_validation
)

ppr_add_test_entry(TARGET engine.tests.app
    NAME matter_runtime_scene_gpu
    FILTER "${ppr_matter_runtime_scene_filter}"
    LOOPS
    RESOURCE_LOCK gpu
    TIMEOUT 180
    LABELS unit engine app gpu_validation
)

unset(ppr_matter_runtime_filter)
unset(ppr_matter_runtime_scene_filter)

ppr_add_test_entry(TARGET engine.tests.app
    NAME matter_dedicated_gpu
    FILTER "app/matter_runtime_gpu/dedicated_actual_domain_cold_reused_acknowledgment_and_completion_are_distinct"
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
    FILTER "app/devices,app/player/,app/dispatch,app/snapshot,app/player_graph,app/viewport,app/render_view,app/camera,app/display,app/sim_packet,app/quaternion,app/input_listener,app/filtered_analog,app/imgui_dpi,app/performance_widget,app/zerov_probe,app/surface_pixels_round_trip_through_png"
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

# Independent post-processing contracts: presentation/settings, fractional
# percentile mass, ceil bloom, output lifetime and the GPU metering/bloom
# entries. The CPU leaves share the entry with device cases, so it claims the
# gpu lock.
ppr_add_test_entry(TARGET engine.tests.app
    NAME post_process
    FILTER "app/post_process"
    LOOPS
    RESOURCE_LOCK gpu
    TIMEOUT 300
    LABELS unit engine app
)

# Backend qualification for the world-lighting storage, winner, sample-rate
# and status contracts. These leaves execute real shader programs and device
# pipelines, so both entries claim the gpu lock. The validation twin re-runs
# the identical group under the RHI debug layers; it is not a routine entry.
ppr_add_test_entry(TARGET engine.tests.app
    NAME world_lighting_backend
    FILTER "app/world_lighting_backend/"
    LOOPS
    RESOURCE_LOCK gpu
    TIMEOUT 180
    LABELS unit engine app
)

ppr_add_test_entry(TARGET engine.tests.app
    NAME world_lighting_backend_gpu_validation
    FILTER "app/world_lighting_backend/"
    LOOPS
    RESOURCE_LOCK gpu
    TIMEOUT 300
    LABELS unit engine app gpu_validation
)

set_tests_properties(engine.tests.app.world_lighting_backend_gpu_validation PROPERTIES
    ENVIRONMENT "PPR_TEST_GPU_VALIDATION=1"
)

# Deterministic categorical projection and asynchronous numeric status: real
# committed GPU particle publication plus device dispatches, so both entries
# claim the gpu lock. The validation twin re-runs the identical group under the
# RHI debug layers.
ppr_add_test_entry(TARGET engine.tests.app
    NAME world_lighting_projection
    FILTER "app/world_lighting_projection/"
    LOOPS
    RESOURCE_LOCK gpu
    TIMEOUT 240
    LABELS unit engine app
)

ppr_add_test_entry(TARGET engine.tests.app
    NAME world_lighting_projection_gpu_validation
    FILTER "app/world_lighting_projection/"
    LOOPS
    RESOURCE_LOCK gpu
    TIMEOUT 420
    LABELS unit engine app gpu_validation
)

set_tests_properties(engine.tests.app.world_lighting_projection_gpu_validation PROPERTIES
    ENVIRONMENT "PPR_TEST_GPU_VALIDATION=1"
)

# Full-domain visibility, R8 jump construction and the common segment tracer.
# Every leaf drives the real pass (projection + two construction dispatches) and
# the real trace helper through device readbacks, so both entries claim the gpu
# lock. The validation twin re-runs the identical group under the RHI debug
# layers.
ppr_add_test_entry(TARGET engine.tests.app
    NAME world_lighting_trace
    FILTER "app/world_lighting_trace/"
    LOOPS
    RESOURCE_LOCK gpu
    TIMEOUT 300
    LABELS unit engine app
)

ppr_add_test_entry(TARGET engine.tests.app
    NAME world_lighting_trace_gpu_validation
    FILTER "app/world_lighting_trace/"
    LOOPS
    RESOURCE_LOCK gpu
    TIMEOUT 480
    LABELS unit engine app gpu_validation
)

set_tests_properties(engine.tests.app.world_lighting_trace_gpu_validation PROPERTIES
    ENVIRONMENT "PPR_TEST_GPU_VALIDATION=1"
)

# Packed world-lighting codec and calibrated thermal lookup: CPU contracts plus
# real GPU shader/device execution, so it claims the gpu lock.
ppr_add_test_entry(TARGET engine.tests.app
    NAME world_lighting_codec
    FILTER "app/world_lighting_codec/"
    LOOPS
    RESOURCE_LOCK gpu
    TIMEOUT 300
    LABELS unit engine app
)
