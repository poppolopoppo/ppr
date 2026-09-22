# Define the executable target
add_executable(app.game main.cpp)

setup_ppr_project(app.game
  INTERNAL_PUBLIC_DEPS engine.core engine.app engine.math engine.shader engine.rhi
)

# Ensure runtime DLLs from linked targets are placed next to the exe.
# CMake 3.27+ provides $<TARGET_RUNTIME_DLLS:...> which computes exactly the
# Windows-specific DLLs pulled in by transitive link dependencies.  This
# removes the need to hardcode a list and works regardless of how slang-rhi
# supplies its libraries. Windows-only: the genex is empty elsewhere, which
# would degrade the copy command into a usage error.
if(WIN32)
    add_custom_command(TARGET app.game POST_BUILD
            COMMAND_EXPAND_LISTS
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:app.game> $<TARGET_FILE_DIR:app.game>
    )
endif()

# Copy shader assets to output directory
add_custom_command(TARGET app.game POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets/shaders"
        "$<TARGET_FILE_DIR:app.game>/shaders"
        COMMENT "Copying shader assets"
)
