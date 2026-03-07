# Define the executable target
add_executable(VideoGameApp main.cpp)

setup_ppr_project(VideoGameApp
  INTERNAL_PUBLIC_DEPS EngineCore EngineApp EngineMath EngineRHI
)

# Ensure runtime DLLs from linked targets are placed next to the exe.
# CMake 3.27+ provides $<TARGET_RUNTIME_DLLS:...> which computes exactly the
# Windows-specific DLLs pulled in by transitive link dependencies.  This
# removes the need to hardcode a list and works regardless of how slang-rhi
# supplies its libraries.
add_custom_command(TARGET VideoGameApp POST_BUILD
        COMMAND_EXPAND_LISTS
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_RUNTIME_DLLS:VideoGameApp> $<TARGET_FILE_DIR:VideoGameApp>
)
