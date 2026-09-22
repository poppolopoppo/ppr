add_executable(engine.tests.asset main.cpp)

target_sources(engine.tests.asset
    PUBLIC
        FILE_SET CXX_MODULES FILES
            Asset.Tests.cppm
    )

target_sources(engine.tests.asset
    PRIVATE
        Asset.Tests.cpp
        Asset.Image.Tests.cpp
        Asset.Search.Tests.cpp
        Asset.Staging.Tests.cpp
        Asset.Fuzz.Tests.cpp
        Asset.Gpu.Tests.cpp
        Asset.Gate.Tests.cpp
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
# as the app suite). The meshes tree is owned by Lane B: stage it once it exists.
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
        # Full run keeps the ASan 3-loop rigor: cpu + stress + gpu + gate no
        # longer fit the old 120 s budget (gate readbacks alone are ~100 s).
        TIMEOUT 300
        LABELS "unit;engine;asset"
)

# Phase 5 tier split: one CTest entry per group with a tier label, so each CI
# tier runs in <30 s. The full run stays a single command
# (ctest -R EngineAssetUnitTests); tiers run via ctest -L <tier>.
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

# CPU unit tier: decode/convert/staging/fuzz — no GPU, no stress loops.
ppr_asset_tier(EngineAssetTierCpu.image asset/image tier-cpu 60)
ppr_asset_tier(EngineAssetTierCpu.staging asset/staging tier-cpu 60)
ppr_asset_tier(EngineAssetTierCpu.mesh asset/mesh tier-cpu 60)
ppr_asset_tier(EngineAssetTierCpu.fuzz asset/fuzz tier-cpu 60)
# Hammer/stress tier: the 8-way parallel import/decode storm alone.
ppr_asset_tier(EngineAssetTierStress.hammer asset/stress tier-stress 60)
# GPU/readback tier: cache upload/resolve/release as one entry; each render
# gate leaf separately — under ASan the runner loops 3x, so one gate entry
# would exceed the 30 s tier budget while single leaves stay well under it.
# Gate leaves run --loop 1 (fast tier signal; the full EngineAssetUnitTests
# run keeps the ASan 3-loop rigor).
ppr_asset_tier(EngineAssetTierGpu.caches asset/gpu tier-gpu 120)
ppr_asset_tier(EngineAssetTierGpu.gate_box asset/gate/bindless_textured_box_gate tier-gpu 90 LOOPS 1)
ppr_asset_tier(EngineAssetTierGpu.gate_orm asset/gate/orm_golden_distinct_channels tier-gpu 90 LOOPS 1)
ppr_asset_tier(EngineAssetTierGpu.gate_tangent asset/gate/tangent_w_render_arbitration tier-gpu 90 LOOPS 1)
ppr_asset_tier(EngineAssetTierGpu.gate_editor asset/gate/editor_scene_flow tier-gpu 90 LOOPS 1)
