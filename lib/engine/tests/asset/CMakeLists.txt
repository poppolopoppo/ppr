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
        TIMEOUT 120
        LABELS "unit;engine;asset"
)
