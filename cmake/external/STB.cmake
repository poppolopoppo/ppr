# STB - Header-only collection of single-file public domain libraries
# Provides image loading and other utilities for the engine

# Pinned before upstream db69a229ae ("deprecate stb_image_resize and
# stb_sprintf as they're moved to their own repos"), which relocated
# stb_image_resize2.h into deprecated/. engine.image includes that header from
# the repository root, so a floating master tag breaks the build.
CPMAddPackage(
    NAME stb
    GITHUB_REPOSITORY nothings/stb
    GIT_TAG 2b2e64bc4778df6a06fbea3a5dbe61f7a76ecf19
)

include("${CMAKE_CURRENT_LIST_DIR}/PatchStbResize.cmake")

# Keep the remaining upstream headers available after the corrected resize header.
if(NOT TARGET stb)
    add_library(stb INTERFACE IMPORTED GLOBAL)
    target_include_directories(stb SYSTEM INTERFACE "${_stbir_include_dir}" "${stb_SOURCE_DIR}")
endif()
