# Define the executable target
add_executable(app.game main.cpp)

target_sources(app.game
    PUBLIC
        FILE_SET CXX_MODULES FILES
            colony/Agents.cppm
            colony/Buildings.cppm
            colony/Colony.cppm
            colony/ColonyDriver.cppm
            colony/ColonyPanel.cppm
            colony/ColonySmoke.cppm
            colony/ColonyTranslator.cppm
            colony/DigTool.cppm
            colony/Pathfinding.cppm
            colony/SimFramePacket.cppm
            colony/WorldGen.cppm
            Game.Config.cppm
            Game.Display.cppm
    PRIVATE
        colony/Agents.cpp
        colony/Buildings.cpp
        colony/Colony.cpp
        colony/ColonyDriver.cpp
        colony/ColonyPanel.cpp
        colony/ColonySmoke.cpp
        colony/ColonyTranslator.cpp
        colony/DigTool.cpp
        colony/Pathfinding.cpp
        colony/SimFramePacket.cpp
        colony/WorldGen.cpp
        Game.Config.cpp
        Game.Display.cpp
    )

setup_ppr_project(app.game
  INTERNAL_PUBLIC_DEPS engine.core engine.app engine.math engine.shader engine.rhi engine.sim
)

# Slice 4: driver-owned physics Scene + chunk colliders (explicit grant).
# PRIVATE shape mirrors engine.tests.sim; box2d stays behind engine.physics.
target_link_libraries(app.game PRIVATE engine.physics)

# Copy shader assets to output directory
add_custom_command(TARGET app.game POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets/shaders"
        "$<TARGET_FILE_DIR:app.game>/shaders"
        COMMENT "Copying shader assets"
)

# Stage texture and mesh fixtures next to the demo executable using the same
# POST_BUILD shape as shaders.
add_custom_command(TARGET app.game POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets/textures"
        "$<TARGET_FILE_DIR:app.game>/textures"
        COMMENT "Copying texture assets"
)

if(EXISTS "${CMAKE_SOURCE_DIR}/assets/meshes")
    add_custom_command(TARGET app.game POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets/meshes"
            "$<TARGET_FILE_DIR:app.game>/meshes"
            COMMENT "Copying mesh assets"
    )
endif()

# Game-owned invocation compatibility tests (Phase 2 gate): pure
# parse/resolve coverage for selectors/conflicts, legacy value forms,
# profiling triggers, startup booleans, configuration precedence, and help
# without configuration-file availability. Lives in the game build area so
# engine test tiers never depend upward on game modules.
if(NOT DEFINED BUILD_TESTING OR BUILD_TESTING)
    add_executable(game.tests Game.Tests.Main.cpp)

    target_sources(game.tests
        PUBLIC
            FILE_SET CXX_MODULES FILES
                Game.Config.cppm
                Game.Display.cppm
                Game.Tests.cppm
        PRIVATE
            Game.Config.cpp
            Game.Display.cpp
            Game.Tests.cpp
            Game.Config.Tests.cpp
            Game.Display.Tests.cpp
        )

    setup_ppr_project(game.tests
      INTERNAL_PUBLIC_DEPS engine.core engine.app engine.tests
    )

    target_include_directories(game.tests PRIVATE ${CMAKE_SOURCE_DIR}/lib/engine/tests/include)

    # MSVC module workaround for __tlregdtor C++ linkage issue with thread_local destructors in modules
    if(MSVC AND PPR_ENABLE_SANITIZER_ADDRESS)
        target_link_options(game.tests PRIVATE "/alternatename:?__tlregdtor@@YAHXZ=__tlregdtor")
    endif()

    add_test(NAME GameConfigUnitTests COMMAND game.tests --loop ${PPR_TEST_LOOPS})

    set_tests_properties(GameConfigUnitTests
        PROPERTIES
            TIMEOUT 120
            LABELS "unit;game"
            WORKING_DIRECTORY "$<TARGET_FILE_DIR:game.tests>"
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:game.tests>"
    )
endif()
