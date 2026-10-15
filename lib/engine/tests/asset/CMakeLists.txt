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
        # Image: image goldens + mip goldens + fixture path-join (all CPU-only).
        Asset.Image.Tests.cpp
        Asset.Image.Mips.Tests.cpp
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

set(asset_fixture_directories shaders textures)
if(EXISTS "${CMAKE_SOURCE_DIR}/assets/meshes")
    list(APPEND asset_fixture_directories meshes)
endif()
ppr_stage_assets(engine.tests.asset ${asset_fixture_directories})

add_test(NAME EngineAssetUnitTests COMMAND engine.tests.asset --loop ${PPR_TEST_LOOPS})

set_tests_properties(EngineAssetUnitTests
        PROPERTIES
        # Full run uses PPR_TEST_LOOPS (default 1), serialized for GPU gates.
        # cpu + stress + gpu + gate retain margin over the old 120 s budget
        # (gate readbacks alone are ~100 s).
        RUN_SERIAL TRUE
        TIMEOUT 450
        LABELS "unit;engine;asset"
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:engine.tests.asset>"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:engine.tests.asset>"
)
