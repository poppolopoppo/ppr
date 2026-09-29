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
            colony/TimeTrials.cppm
            colony/WorldGen.cppm
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
        colony/TimeTrials.cpp
        colony/WorldGen.cpp
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
