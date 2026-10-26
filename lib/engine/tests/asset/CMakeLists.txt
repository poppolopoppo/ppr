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

# CPU and GPU halves are separate entries. The CPU entry excludes the
# asset/render subtree and owns no lock; the GPU entry owns that subtree and
# claims the gpu lock so it cannot overlap another device user while CTest
# runs the rest of the suite in parallel. Both retain PPR_TEST_LOOPS and the
# 450 s budget (cpu + stress + gpu + gate; gate readbacks alone are ~100 s).
ppr_add_test_entry(TARGET engine.tests.asset
    NAME cpu
    FILTER "asset"
    EXCLUDE "asset/render"
    LOOPS
    TIMEOUT 450
    LABELS unit engine asset cpu
)

ppr_add_test_entry(TARGET engine.tests.asset
    NAME gpu
    FILTER "asset/render"
    EXCLUDE "asset/render/gates/grid_resident_circle_mask_motion_overlap_occlusion_and_leases"
    LOOPS
    RESOURCE_LOCK gpu
    TIMEOUT 450
    LABELS unit engine asset gpu
)

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
