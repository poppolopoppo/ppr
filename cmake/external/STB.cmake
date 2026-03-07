# STB - Header-only collection of single-file public domain libraries
# Provides image loading and other utilities for the engine

CPMAddPackage(
    NAME stb
    GITHUB_REPOSITORY nothings/stb
    GIT_TAG master
)

# Create interface target for stb
if(NOT TARGET stb)
    add_library(stb INTERFACE IMPORTED GLOBAL)
    target_include_directories(stb SYSTEM INTERFACE ${stb_SOURCE_DIR})
endif()
