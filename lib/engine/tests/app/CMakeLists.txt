add_executable(engine.tests.app main.cpp)

target_sources(engine.tests.app
    PUBLIC
        FILE_SET CXX_MODULES FILES
            App.Tests.cppm
            App.Camera.Tests.cppm
            App.Devices.Tests.cppm
            App.Player.Tests.cppm
            App.Dispatch.Tests.cppm
            App.Snapshot.Tests.cppm
            App.PlayerService.Tests.cppm
            App.Player.Graph.Tests.cppm
            App.Shader.Tests.cppm
            App.Viewport.Tests.cppm
            App.RenderView.Tests.cppm
            App.PixelReadback.Tests.cppm
            App.Quaternion.Tests.cppm
            App.Input.Listener.Tests.cppm
            App.WindowInput.Tests.cppm
            App.ImGuiRouting.Tests.cppm
            App.ImguiDpi.Tests.cppm
            App.FilteredAnalog.Tests.cppm
            App.ZeroVProbe.Tests.cppm
    )

setup_ppr_project(engine.tests.app
    INTERNAL_PUBLIC_DEPS engine.app engine.tests
    EXTERNAL_SYSTEM_PRIVATE_DEPS glfw
)

target_include_directories(engine.tests.app PRIVATE ${CMAKE_SOURCE_DIR}/lib/engine/tests/include)

# MSVC module workaround for __tlregdtor C++ linkage issue with thread_local destructors in modules
if(MSVC AND PPR_ENABLE_SANITIZER_ADDRESS)
    target_link_options(engine.tests.app PRIVATE "/alternatename:?__tlregdtor@@YAHXZ=__tlregdtor")
endif()

# Copy shader assets next to the test executable so Renderer can find them via content_dir
add_custom_command(TARGET engine.tests.app POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets/shaders"
        "$<TARGET_FILE_DIR:engine.tests.app>/shaders"
        COMMENT "Copying shader assets"
)

add_test(NAME EngineAppUnitTests COMMAND engine.tests.app WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

set_tests_properties(EngineAppUnitTests
    PROPERTIES
        TIMEOUT 120
        LABELS "unit;engine;app"
)
