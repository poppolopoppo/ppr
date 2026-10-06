add_executable(engine.tests.app main.cpp)

target_sources(engine.tests.app
    PUBLIC
        FILE_SET CXX_MODULES BASE_DIRS ${CMAKE_SOURCE_DIR} FILES
            App.Tests.cppm

            # F1 game-payload tests compile the colony subset directly: the
            # sim-carrying packet lives in game/ (engine.app must not link
            # sim/physics), so its unit tests ship with the modules, not via
            # a link to the app.game executable. WorldGen is included as a
            # leaf (core/sim deps only, already linked); Panel, Smoke and
            # TimeTrials stay out (unneeded here; smoke stays self-hosted).
            ${CMAKE_SOURCE_DIR}/game/colony/Colony.cppm
            ${CMAKE_SOURCE_DIR}/game/colony/WorldGen.cppm
            ${CMAKE_SOURCE_DIR}/game/colony/ColonyDriver.cppm
            ${CMAKE_SOURCE_DIR}/game/colony/DigTool.cppm
            ${CMAKE_SOURCE_DIR}/game/colony/Pathfinding.cppm
            ${CMAKE_SOURCE_DIR}/game/colony/ColonyTranslator.cppm
            ${CMAKE_SOURCE_DIR}/game/colony/Agents.cppm
            ${CMAKE_SOURCE_DIR}/game/colony/Buildings.cppm
            ${CMAKE_SOURCE_DIR}/game/colony/SimFramePacket.cppm
    )

target_sources(engine.tests.app
    PRIVATE
        App.Tests.cpp
        App.SimPacket.Tests.cpp
        App.Player.Tests.cpp
        App.PlayerService.Tests.cpp
        App.Player.Graph.Tests.cpp
        App.Devices.Tests.cpp
        App.Input.Listener.Tests.cpp
        App.FilteredAnalog.Tests.cpp
        App.WindowInput.Tests.cpp
        App.Shader.Tests.cpp
        App.Viewport.Tests.cpp
        App.RenderView.Tests.cpp
        App.PixelReadback.Tests.cpp
        App.Camera.Tests.cpp
        App.GridPass.Tests.cpp
        App.Frame.Tests.cpp
        App.Quaternion.Tests.cpp
        App.ImGuiRouting.Tests.cpp
        App.ImguiDpi.Tests.cpp
        App.ZeroVProbe.Tests.cpp

        ${CMAKE_SOURCE_DIR}/game/colony/Colony.cpp
        ${CMAKE_SOURCE_DIR}/game/colony/WorldGen.cpp
        ${CMAKE_SOURCE_DIR}/game/colony/ColonyDriver.cpp
        ${CMAKE_SOURCE_DIR}/game/colony/DigTool.cpp
        ${CMAKE_SOURCE_DIR}/game/colony/Pathfinding.cpp
        ${CMAKE_SOURCE_DIR}/game/colony/ColonyTranslator.cpp
        ${CMAKE_SOURCE_DIR}/game/colony/Agents.cpp
        ${CMAKE_SOURCE_DIR}/game/colony/Buildings.cpp
        ${CMAKE_SOURCE_DIR}/game/colony/SimFramePacket.cpp
    )

setup_ppr_project(engine.tests.app
    INTERNAL_PUBLIC_DEPS engine.app engine.sim engine.tests
    EXTERNAL_SYSTEM_PRIVATE_DEPS glfw
)

# ColonyDriver links physics privately (same shape as engine.tests.sim);
# the colony headers include StageTiming.h/Colony.Elements.h by relative name.
target_link_libraries(engine.tests.app PRIVATE engine.physics)

target_include_directories(engine.tests.app PRIVATE
    ${CMAKE_SOURCE_DIR}/lib/engine/tests/include
    ${CMAKE_SOURCE_DIR}/game
    ${CMAKE_SOURCE_DIR}/game/colony
)

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

add_test(NAME EngineAppUnitTests COMMAND engine.tests.app)

set_tests_properties(EngineAppUnitTests
    PROPERTIES
        # Runner executes 3 shuffle loops (~176s measured debug); keep margin.
        TIMEOUT 600
        LABELS "unit;engine;app"
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:engine.tests.app>"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:engine.tests.app>"
)
