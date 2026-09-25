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
        Asset.Render.Draws.Tests.cpp
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

add_test(NAME EngineAssetUnitTests COMMAND engine.tests.asset)

set_tests_properties(EngineAssetUnitTests
        PROPERTIES
        # Full run keeps the ASan 3-loop rigor (tier/loop policy in codemap.md):
        # cpu + stress + gpu + gate no longer fit the old 120 s budget
        # (gate readbacks alone are ~100 s).
        TIMEOUT 450
        LABELS "unit;engine;asset"
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:engine.tests.asset>"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:engine.tests.asset>"
)
