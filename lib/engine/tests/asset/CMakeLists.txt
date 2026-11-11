add_executable(engine.tests.asset main.cpp)

target_sources(engine.tests.asset
    PUBLIC
        FILE_SET CXX_MODULES FILES
            Asset.Tests.cppm
    )

target_sources(engine.tests.asset
    PRIVATE
        Asset.Tests.cpp
        Asset.Pack.Tests.cpp
        Asset.Preparation.Tests.cpp
        # Image: image goldens + mip goldens + fixture path-join (all CPU-only).
        Asset.Image.Tests.cpp
        Asset.Image.Mips.Tests.cpp
        Asset.Image.Preparation.Tests.cpp
        # Mesh: mesh import, UV/normal bakes (all CPU-only). Material/pipeline
        # proofs moved to engine.tests.app (App.RenderView).
        Asset.Search.Tests.cpp
        Asset.Search.UvNormal.Tests.cpp
        # Render: bindless caches, readback gates, indirect draws (shared GPU scope),
        # plus the device-loss/editor quarantine (private apps).
        Asset.Render.Caches.Tests.cpp
        Asset.Render.Gates.Tests.cpp
        Asset.Render.Draws.Tests.cpp
        Asset.Render.SharedMesh.Tests.cpp
        # Resilience: fuzz corpus + import/decode storm (both CPU-only).
        Asset.Resilience.Tests.cpp
        # Observe: asset-pipeline logging proofs (CPU-owned lines).
        Asset.Observe.Tests.cpp
        Asset.GpuFixture.cpp
    )

setup_ppr_project(engine.tests.asset
    INTERNAL_PUBLIC_DEPS engine.asset engine.image engine.mesh engine.app engine.rhi engine.shader engine.tests
    EXTERNAL_SYSTEM_PRIVATE_DEPS mango-image mango-import3d glfw
)

target_include_directories(engine.tests.asset PRIVATE ${CMAKE_SOURCE_DIR}/lib/engine/tests/include)

# MSVC module workaround for __tlregdtor C++ linkage issue with thread_local destructors in modules
if(MSVC AND PPR_ENABLE_SANITIZER_ADDRESS)
    target_link_options(engine.tests.asset PRIVATE "/alternatename:?__tlregdtor@@YAHXZ=__tlregdtor")
endif()

set(asset_fixture_directories shaders textures fonts lighting)
if(EXISTS "${CMAKE_SOURCE_DIR}/assets/meshes")
    list(APPEND asset_fixture_directories meshes)
endif()
ppr_stage_assets(engine.tests.asset ${asset_fixture_directories})

# CPU and GPU work use separate entries. The CPU entry excludes asset/render
# and owns no lock. GPU entries claim the gpu lock while CTest runs CPU work
# in parallel. All entries retain PPR_TEST_LOOPS; the cpu entry and every
# routine GPU correctness shard keep the 450 s deadline, with separate 300 s
# deadlines for measured slow gates.
ppr_add_test_entry(TARGET engine.tests.asset
    NAME cpu
    FILTER "asset"
    EXCLUDE "asset/render"
    LOOPS
    TIMEOUT 450
    LABELS unit engine asset cpu
)

# Routine correctness is sharded by responsibility instead of one `gpu`
# aggregate. The retired aggregate mixed correctness leaves with measurement
# loops, so it could not finish in 450 s.
# Each shard names its leaves explicitly to keep the union auditable; the
# joins below produce the single comma-separated token the runner expects.
# Validation entries further down re-select specific leaves opt-in and are not
# part of this union.
ppr_add_test_entry(TARGET engine.tests.asset
    NAME gpu_caches
    FILTER "asset/render/caches,asset/render/sharedmesh"
    LOOPS
    RESOURCE_LOCK gpu
    TIMEOUT 450
    LABELS unit engine asset gpu
)

string(JOIN "," ppr_asset_gpu_picture_filter
    "asset/render/gates/picture_controls_preserve_independent_color_and_scene_history"
    "asset/render/gates/picture_hdr_punchy_matches_frozen_reference_and_single_encoding"
    "asset/render/gates/picture_hdr_rejected_policy_preserves_prepared_frame_and_history"
    "asset/render/gates/spatial_msaa4_raster_thresholds_recover_indexed_pattern"
    "asset/render/gates/spatial_msaa4_matches_grid_reference_and_affine_moments"
    "asset/render/gates/background_magnification_matches_wrapped_cubic_reference"
)

ppr_add_test_entry(TARGET engine.tests.asset
    NAME gpu_picture
    FILTER "${ppr_asset_gpu_picture_filter}"
    LOOPS
    RESOURCE_LOCK gpu
    TIMEOUT 450
    LABELS unit engine asset gpu
)

string(JOIN "," ppr_asset_gpu_grid_particles_filter
    "asset/render/gates/grid_particle_support_and_resource_repeat"
    "asset/render/gates/grid_candidate_world_retains_active_until_matching_activation"
    "asset/render/gates/grid_compute_visibility_matches_reference_and_draws"
    "asset/render/gates/grid_stochastic_world_uvs_mips_and_chunk_seams"
    "asset/render/gates/grid_surface_inventory_transitions_preserve_gas_visibility_and_single_blending"
)

ppr_add_test_entry(TARGET engine.tests.asset
    NAME gpu_grid_particles
    FILTER "${ppr_asset_gpu_grid_particles_filter}"
    LOOPS
    RESOURCE_LOCK gpu
    TIMEOUT 450
    LABELS unit engine asset gpu
)

string(JOIN "," ppr_asset_gpu_grid_surface_filter
    "asset/render/gates/grid_optical_controls_and_four_layer_parallax"
    "asset/render/gates/grid_translucent_solid_borders_and_internal_boundaries"
    "asset/render/gates/grid_solid_particles_share_catalog_textures_and_metric_uvs"
    "asset/render/gates/grid_solid_five_pixel_features_survive_large_offset_viewport"
)

ppr_add_test_entry(TARGET engine.tests.asset
    NAME gpu_grid_surface
    FILTER "${ppr_asset_gpu_grid_surface_filter}"
    LOOPS
    RESOURCE_LOCK gpu
    TIMEOUT 450
    LABELS unit engine asset gpu
)

string(JOIN "," ppr_asset_gpu_grid_liquid_filter
    "asset/render/gates/grid_liquid_surface_connects_and_preserves_element_boundaries"
    "asset/render/gates/grid_liquid_optics_and_viewport_appearance"
    "asset/render/gates/grid_liquid_nominal_density_deformation_coverage"
    "asset/render/gates/grid_liquid_five_pixel_features_survive_large_offset_viewport"
)

ppr_add_test_entry(TARGET engine.tests.asset
    NAME gpu_grid_liquid
    FILTER "${ppr_asset_gpu_grid_liquid_filter}"
    LOOPS
    RESOURCE_LOCK gpu
    TIMEOUT 450
    LABELS unit engine asset gpu
)

string(JOIN "," ppr_asset_gpu_misc_filter
    "asset/render/gates/bindless_textured_box_gate"
    "asset/render/gates/triangle_depth_occlusion_gate"
    "asset/render/gates/orm_golden_distinct_channels"
    "asset/render/gates/tangent_w_render_proof"
    "asset/render/gates/pipeline_signature_count_sensitivity"
    "asset/render/gates/pipeline_signature_tail_insensitivity"
    "asset/render/gates/pipeline_signature_hash_consistent_with_equality"
    "asset/render/gates/render_rejects_color_attachments_beyond_capacity"
    "asset/render/gates/render_rejects_multilayer_multimip_color_attachment"
    "asset/render/gates/render_resolves_msaa_color_attachment"
    "asset/render/gates/render_accepts_msaa_sample_counts"
    "asset/render/gates/gpu_timestamps_follow_completed_submissions_and_reject_abandoned_encoders"
    "asset/render/gates/pre_render_preparation_orders_and_rejects_submission"
    "asset/render/draws"
    "asset/render/quarantine"
)

ppr_add_test_entry(TARGET engine.tests.asset
    NAME gpu_misc
    FILTER "${ppr_asset_gpu_misc_filter}"
    LOOPS
    RESOURCE_LOCK gpu
    TIMEOUT 450
    LABELS unit engine asset gpu
)

unset(ppr_asset_gpu_picture_filter)
unset(ppr_asset_gpu_grid_particles_filter)
unset(ppr_asset_gpu_grid_surface_filter)
unset(ppr_asset_gpu_grid_liquid_filter)
unset(ppr_asset_gpu_misc_filter)

set(ppr_asset_solid_interface_filter "asset/render/gates/grid_solid_slanted_material_interface_reconstructs_narrow_color_band")
set(ppr_asset_solid_envelope_filter "asset/render/gates/grid_solid_envelope_closes_nominal_interstices_without_bridging_sparse_grains")
string(JOIN "," ppr_asset_spatial_integration_filter
    "asset/render/gates/spatial_grid_samples_preserve_contours_materials_normals_and_emission"
    "asset/render/gates/spatial_hdr_grading_and_native_ui_follow_reconstruction"
)

# The two slow solid gates keep their own 300-second entries; the routine
# shards above no longer exclude leaves from a broad aggregate. Ordinary leaf
# measurements were 249.02 s and 148.06 s respectively.
ppr_add_test_entry(TARGET engine.tests.asset
    NAME solid_interface_gpu
    FILTER "${ppr_asset_solid_interface_filter}"
    LOOPS
    RESOURCE_LOCK gpu
    TIMEOUT 300
    LABELS unit engine asset gpu
)

ppr_add_test_entry(TARGET engine.tests.asset
    NAME solid_envelope_gpu
    FILTER "${ppr_asset_solid_envelope_filter}"
    LOOPS
    RESOURCE_LOCK gpu
    TIMEOUT 300
    LABELS unit engine asset gpu
)

unset(ppr_asset_solid_interface_filter)
unset(ppr_asset_solid_envelope_filter)

# Actual GridPass samples and independent post grading are distinct from the
# two inexpensive kernel/admission prerequisite leaves.
# Ordinary aggregate passed in 276 s. GPUAV exceeded 300 s after three leaves.
# Partition by real behaviour, preserving the surviving exact tokens/deadlines.
set(ppr_asset_spatial_parts grid grading)
string(REPLACE "," ";" ppr_asset_spatial_filters "${ppr_asset_spatial_integration_filter}")
foreach(ppr_asset_spatial IN ZIP_LISTS ppr_asset_spatial_parts ppr_asset_spatial_filters)
    ppr_add_test_entry(TARGET engine.tests.asset
        NAME spatial_${ppr_asset_spatial_0}_gpu
        FILTER "${ppr_asset_spatial_1}"
        LOOPS
        RESOURCE_LOCK gpu
        TIMEOUT 300
        LABELS unit engine asset gpu
    )

    ppr_add_test_entry(TARGET engine.tests.asset
        NAME spatial_${ppr_asset_spatial_0}_gpu_validation
        FILTER "${ppr_asset_spatial_1}"
        LOOPS
        RESOURCE_LOCK gpu
        TIMEOUT 300
        LABELS unit engine asset gpu_validation
    )

    set_tests_properties(engine.tests.asset.spatial_${ppr_asset_spatial_0}_gpu_validation PROPERTIES
        ENVIRONMENT "PPR_TEST_GPU_VALIDATION=1"
    )
endforeach()
unset(ppr_asset_spatial_0)
unset(ppr_asset_spatial_1)
unset(ppr_asset_spatial_parts)
unset(ppr_asset_spatial_filters)
unset(ppr_asset_spatial_integration_filter)

# Keep the resident circle pixel/lease gate in its existing validation entry.
ppr_add_test_entry(TARGET engine.tests.asset
    NAME resident_particles_gpu
    FILTER "asset/render/gates/grid_resident_circle_mask_motion_overlap_occlusion_and_leases"
    LOOPS
    RESOURCE_LOCK gpu
    TIMEOUT 300
    LABELS unit engine asset gpu_validation
)

set_tests_properties(engine.tests.asset.resident_particles_gpu PROPERTIES
    ENVIRONMENT "PPR_TEST_GPU_VALIDATION=1"
)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND PPR_ENABLE_SANITIZER_ADDRESS)
    # Retain the NVIDIA caller below DBus frames for the existing driver-only suppressions.
    foreach(ppr_asset_gpu_shard IN ITEMS gpu_caches gpu_picture gpu_grid_particles gpu_grid_surface gpu_grid_liquid gpu_misc
            solid_interface_gpu solid_envelope_gpu
            spatial_grid_gpu spatial_grid_gpu_validation
            spatial_grading_gpu spatial_grading_gpu_validation)
        set_property(TEST engine.tests.asset.${ppr_asset_gpu_shard} APPEND PROPERTY ENVIRONMENT_MODIFICATION
            "ASAN_OPTIONS=string_append::fast_unwind_on_malloc=0:malloc_context_size=20"
        )
    endforeach()
    unset(ppr_asset_gpu_shard)
endif()

# GPU validation entry, mirroring the app-side twin: PPR_TEST_GPU_VALIDATION=1
# enables the RHI debug core/GPU-assisted validation layers for the grid
# compute visibility gate, and RESOURCE_LOCK gpu serializes it against
# engine.tests.app.gpu_validation without globally serializing CTest.
ppr_add_test_entry(TARGET engine.tests.asset
    NAME gpu_validation
    FILTER "asset/render/gates/grid_compute_visibility_matches_reference_and_draws"
    LOOPS
    RESOURCE_LOCK gpu
    TIMEOUT 300
    LABELS unit engine asset gpu_validation
)

set_tests_properties(engine.tests.asset.gpu_validation PROPERTIES
    ENVIRONMENT "PPR_TEST_GPU_VALIDATION=1"
)

# HDR qualification must exercise both debug layers as well as routine rendering.
ppr_add_test_entry(TARGET engine.tests.asset
    NAME picture_hdr_gpu_validation
    FILTER "asset/render/gates/picture_hdr_punchy_matches_frozen_reference_and_single_encoding,asset/render/gates/picture_hdr_rejected_policy_preserves_prepared_frame_and_history"
    LOOPS
    RESOURCE_LOCK gpu
    TIMEOUT 300
    LABELS unit engine asset gpu_validation
)

set_tests_properties(engine.tests.asset.picture_hdr_gpu_validation PROPERTIES
    ENVIRONMENT "PPR_TEST_GPU_VALIDATION=1"
)

# Indexed spatial resolve qualification also runs with both debug layers.
ppr_add_test_entry(TARGET engine.tests.asset
    NAME picture_spatial_gpu_validation
    FILTER "asset/render/gates/spatial_msaa4_raster_thresholds_recover_indexed_pattern,asset/render/gates/spatial_msaa4_matches_grid_reference_and_affine_moments"
    LOOPS
    RESOURCE_LOCK gpu
    TIMEOUT 300
    LABELS unit engine asset gpu_validation
)

set_tests_properties(engine.tests.asset.picture_spatial_gpu_validation PROPERTIES
    ENVIRONMENT "PPR_TEST_GPU_VALIDATION=1"
)
