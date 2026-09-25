# Define the executable target
add_executable(app.game main.cpp)

setup_ppr_project(app.game
  INTERNAL_PUBLIC_DEPS engine.core engine.app engine.math engine.shader engine.rhi
)

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
