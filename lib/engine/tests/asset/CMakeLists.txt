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
        # Mip chains: one focused test per TU.
        Asset.MipsChain.Tests.cpp
        Asset.MipsCoverage.Tests.cpp
        Asset.MipsBleed.Tests.cpp
        Asset.MipsQuality.Tests.cpp
        Asset.Search.Tests.cpp
        Asset.Staging.Tests.cpp
        # UV transforms/sets + inverse-transpose normals (one focused group, own TU).
        Asset.UvNormal.Tests.cpp
        Asset.Fuzz.Tests.cpp
        Asset.Gpu.Tests.cpp
        Asset.GpuFixture.cpp
        Asset.Gate.Tests.cpp
        # Indirect layouts (CPU-only) + direct-vs-indirect parity gate.
        Asset.IndirectLayout.Tests.cpp
        Asset.IndirectParity.Tests.cpp
        # Compute-publish gate (count, parity, guards, ring).
        Asset.IndirectCompute.Tests.cpp
        # Failure injection (no half-publish, clamped draw, empty staged) +
        # mid-frame device-loss restart hash.
        Asset.IndirectInjection.Tests.cpp
        Asset.IndirectLoss.Tests.cpp
        # Asset-pipeline logging proofs: CPU-owned lines + helper (own TU) and
        # GPU-owned lines (own TU, needs the rendering TestApp + fixtures).
        Asset.Logging.Tests.cpp
        Asset.LoggingGpu.Tests.cpp
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

# CPU unit tier: decode/convert/staging/fuzz — no GPU, no stress loops.
ppr_asset_tier(EngineAssetTierCpu.image asset/image tier-cpu 60)
# Mip goldens (CPU-only chain generation).
ppr_asset_tier(EngineAssetTierCpu.mips_chain asset/mips_chain tier-cpu 60)
ppr_asset_tier(EngineAssetTierCpu.mips_coverage asset/mips_coverage tier-cpu 60)
ppr_asset_tier(EngineAssetTierCpu.mips_bleed asset/mips_bleed tier-cpu 60)
ppr_asset_tier(EngineAssetTierCpu.mips_quality asset/mips_quality tier-cpu 60)
ppr_asset_tier(EngineAssetTierCpu.staging asset/staging tier-cpu 60)
ppr_asset_tier(EngineAssetTierCpu.mesh asset/mesh tier-cpu 60)
ppr_asset_tier(EngineAssetTierCpu.fuzz asset/fuzz tier-cpu 60)
# Hammer/stress tier: the 8-way parallel import/decode storm alone.
ppr_asset_tier(EngineAssetTierStress.hammer asset/stress tier-stress 60)
# GPU/readback tier: cache upload/resolve/release as one entry; each render
# gate leaf separately (single leaves stay well under budget).
# Gate leaves run --loop 1; the full EngineAssetUnitTests run keeps the
# ASan 3-loop rigor (policy in codemap.md).
ppr_asset_tier(EngineAssetTierGpu.caches asset/gpu tier-gpu 120)
ppr_asset_tier(EngineAssetTierGpu.gate_box asset/gate/bindless_textured_box_gate tier-gpu 90 LOOPS 1)
ppr_asset_tier(EngineAssetTierGpu.gate_orm asset/gate/orm_golden_distinct_channels tier-gpu 90 LOOPS 1)
ppr_asset_tier(EngineAssetTierGpu.gate_tangent asset/gate/tangent_w_render_arbitration tier-gpu 90 LOOPS 1)
ppr_asset_tier(EngineAssetTierGpu.gate_editor asset/gate/editor_scene_flow tier-gpu 90 LOOPS 1)
# Indirect layouts are CPU-only; the parity gate is a readback leaf like the
# other gates (loop 1 in tier; full run keeps 3-loop rigor).
ppr_asset_tier(EngineAssetTierCpu.indirect_layout asset/indirect_layout tier-cpu 60)
ppr_asset_tier(EngineAssetTierGpu.gate_indirect asset/indirect_parity tier-gpu 90 LOOPS 1)
ppr_asset_tier(EngineAssetTierGpu.gate_indirect_compute asset/indirect_compute tier-gpu 90 LOOPS 1)
# Injection + loss leaves run loop-1 in the fast tier (full run keeps 3-loop rigor).
ppr_asset_tier(EngineAssetTierGpu.gate_indirect_injection asset/indirect_injection tier-gpu 90 LOOPS 1)
ppr_asset_tier(EngineAssetTierGpu.gate_indirect_loss asset/indirect_loss tier-gpu 90 LOOPS 1)
