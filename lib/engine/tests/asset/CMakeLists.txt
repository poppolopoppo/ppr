add_executable(engine.tests.asset main.cpp)

target_sources(engine.tests.asset
    PUBLIC
        FILE_SET CXX_MODULES FILES
            Asset.Tests.cppm
    )

target_sources(engine.tests.asset
    PRIVATE
        Asset.Tests.cpp
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
        Asset.Render.Indirect.Tests.cpp
        # Resilience: fuzz corpus + import/decode storm (both CPU-only).
        Asset.Resilience.Tests.cpp
        # Observe: asset-pipeline logging proofs (CPU-owned lines).
        Asset.Observe.Tests.cpp
        Asset.GpuFixture.cpp
    )

setup_ppr_project(engine.tests.asset
    INTERNAL_PUBLIC_DEPS engine.image engine.mesh engine.app engine.rhi engine.shader engine.tests
    EXTERNAL_SYSTEM_PRIVATE_DEPS mango-image mango-import3d glfw
)

target_include_directories(engine.tests.asset PRIVATE ${CMAKE_SOURCE_DIR}/lib/engine/tests/include)

# MSVC module workaround for __tlregdtor C++ linkage issue with thread_local destructors in modules
if(MSVC AND PPR_ENABLE_SANITIZER_ADDRESS)
    target_link_options(engine.tests.asset PRIVATE "/alternatename:?__tlregdtor@@YAHXZ=__tlregdtor")
endif()

# Stage texture/mesh fixtures next to the test executable (same POST_BUILD shape
# as the app suite).
add_custom_command(TARGET engine.tests.asset POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets/textures"
        "$<TARGET_FILE_DIR:engine.tests.asset>/textures"
        COMMENT "Copying texture fixtures"
)

if(EXISTS "${CMAKE_SOURCE_DIR}/assets/meshes")
    add_custom_command(TARGET engine.tests.asset POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets/meshes"
            "$<TARGET_FILE_DIR:engine.tests.asset>/meshes"
            COMMENT "Copying mesh fixtures"
    )
endif()

# TrianglePass::initialize loads mesh_bindless.slang via content_dir (same shape as
# the app suite); the GPU cache group boots a rendering TestApp.
add_custom_command(TARGET engine.tests.asset POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets/shaders"
        "$<TARGET_FILE_DIR:engine.tests.asset>/shaders"
        COMMENT "Copying shader assets"
)

add_test(NAME EngineAssetUnitTests COMMAND engine.tests.asset WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

set_tests_properties(EngineAssetUnitTests
        PROPERTIES
        # Full run keeps the ASan 3-loop rigor (tier/loop policy in codemap.md):
        # cpu + stress + gpu + gate no longer fit the old 120 s budget
        # (gate readbacks alone are ~100 s).
        TIMEOUT 450
        LABELS "unit;engine;asset"
)

# Tier split: one CTest entry per group with a tier label, so each CI
# tier runs in <30 s (policy in codemap.md). The full run stays a single
# command (ctest -R EngineAssetUnitTests); tiers run via ctest -L <tier>.
function(ppr_asset_tier test_name filter tier timeout_seconds)
    cmake_parse_arguments(TIER "" "LOOPS" "" ${ARGN})
    set(tier_loops "")
    if(DEFINED TIER_LOOPS)
        set(tier_loops --loop ${TIER_LOOPS})
    endif()
    add_test(NAME ${test_name} COMMAND engine.tests.asset --run-test ${filter} --no-shuffle ${tier_loops}
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(${test_name}
            PROPERTIES
            TIMEOUT ${timeout_seconds}
            LABELS "unit;engine;asset;${tier}"
    )
endfunction()

# CPU tiers: decode/convert/resilience/observe hold no SharedGpu refs, so
# these entries never boot a device. GPU tiers run --loop 1; the full
# EngineAssetUnitTests run keeps the ASan 3-loop rigor (policy in codemap.md).
ppr_asset_tier(EngineAssetTierCpu.image asset/image tier-cpu 60)
ppr_asset_tier(EngineAssetTierCpu.mesh asset/mesh tier-cpu 60)
ppr_asset_tier(EngineAssetTierCpu.resilience asset/resilience tier-cpu 60)
ppr_asset_tier(EngineAssetTierCpu.observe asset/observe tier-cpu 60)
# GPU tiers: caches + readback gates + indirect draws under one shared scope
# per entry; quarantine (editor flow + device-loss restart) runs its private
# apps alone.
ppr_asset_tier(EngineAssetTierGpu.caches asset/render/caches tier-gpu 120 LOOPS 1)
ppr_asset_tier(EngineAssetTierGpu.gates asset/render/gates tier-gpu 90 LOOPS 1)
ppr_asset_tier(EngineAssetTierGpu.indirect asset/render/indirect tier-gpu 90 LOOPS 1)
ppr_asset_tier(EngineAssetTierGpu.quarantine asset/render/quarantine tier-gpu 120 LOOPS 1)
