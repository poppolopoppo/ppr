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

set(asset_fixture_directories shaders textures fonts)
if(EXISTS "${CMAKE_SOURCE_DIR}/assets/meshes")
    list(APPEND asset_fixture_directories meshes)
endif()
ppr_stage_assets(engine.tests.asset ${asset_fixture_directories})

# CPU and GPU work use separate entries. The CPU entry excludes asset/render
# and owns no lock. GPU entries claim the gpu lock while CTest runs CPU work
# in parallel. All entries retain PPR_TEST_LOOPS; cpu and residual gpu retain
# their 450 s deadlines, with separate 300 s deadlines for measured slow gates.
ppr_add_test_entry(TARGET engine.tests.asset
    NAME cpu
    FILTER "asset"
    EXCLUDE "asset/render"
    LOOPS
    TIMEOUT 450
    LABELS unit engine asset cpu
)

set(ppr_asset_solid_interface_filter "asset/render/gates/grid_solid_slanted_material_interface_reconstructs_narrow_color_band")
set(ppr_asset_solid_envelope_filter "asset/render/gates/grid_solid_envelope_closes_nominal_interstices_without_bridging_sparse_grains")
string(JOIN "," ppr_asset_spatial_integration_filter
    "asset/render/gates/spatial_grid_samples_preserve_contours_materials_normals_and_emission"
    "asset/render/gates/spatial_retro_matches_reference_and_preserves_lifecycle"
    "asset/render/gates/spatial_background_alignment_and_opaque_admission"
    "asset/render/gates/spatial_hdr_grading_and_native_ui_follow_reconstruction"
)

ppr_add_test_entry(TARGET engine.tests.asset
    NAME gpu
    FILTER "asset/render"
    EXCLUDE "asset/render/gates/grid_resident_circle_mask_motion_overlap_occlusion_and_leases,${ppr_asset_solid_interface_filter},${ppr_asset_solid_envelope_filter},${ppr_asset_spatial_integration_filter}"
    LOOPS
    RESOURCE_LOCK gpu
    TIMEOUT 450
    LABELS unit engine asset gpu
)

# Ordinary leaf measurements were 249.02 s and 148.06 s respectively.
# Their exact exclusions above preserve the old ordinary render leaf union.
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

# Actual GridPass samples, real RetroPass reconstruction and lifecycle are
# distinct from the two inexpensive kernel/admission prerequisite leaves.
# Ordinary aggregate passed in 276 s. GPUAV exceeded 300 s after three leaves;
# the remaining Retro leaf passed alone in 135.8 s (observed total about 391 s).
# Partition by real behaviour, preserving all four exact tokens and deadlines.
set(ppr_asset_spatial_parts grid retro background grading)
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
    foreach(ppr_asset_gpu_shard IN ITEMS gpu solid_interface_gpu solid_envelope_gpu
            spatial_grid_gpu spatial_grid_gpu_validation
            spatial_retro_gpu spatial_retro_gpu_validation
            spatial_background_gpu spatial_background_gpu_validation
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
