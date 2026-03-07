# Mango is a multipurpose image processing library that depends on:
# - fmt (formatting)
# - zlib (compression)
# - libdeflate (deflate support)
# - zstd (zstd compression)
# - lcms2 (color space conversion) - OPTIONAL
# - simdjson (3D import) - if BUILD_IMPORT3D=ON
#
# These dependencies are provided by vcpkg via manifest mode (vcpkg.json).
# When mango is added as a CPM sub-project, it needs CMAKE_PREFIX_PATH explicitly
# set to find the locally-installed vcpkg packages (not the system vcpkg location).

CPMAddPackage(
    NAME mango
    GITHUB_REPOSITORY t0rakka/mango
    GIT_TAG main
    CMAKE_ARGS
        "-DCMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}"
        "-DVCPKG_TARGET_TRIPLET=${VCPKG_TARGET_TRIPLET}"
    OPTIONS
        "ENABLE_AVX ON"
        "ENABLE_AVX2 ON"
        "ENABLE_SSE2 ON"
        "ENABLE_SSE4 ON"
        "BUILD_OPENGL OFF"
        "BUILD_VULKAN OFF"
        "BUILD_IMPORT3D ON"
        "BUILD_EXAMPLES OFF"
        "BUILD_SHARED_LIBS OFF"
)

set_target_properties(mango PROPERTIES CXX_MODULE_STD OFF)

# Mark mango includes as SYSTEM to suppress warnings from external headers
get_target_property(mango_inc mango INTERFACE_INCLUDE_DIRECTORIES)
if(mango_inc)
    set_target_properties(mango PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${mango_inc}")
endif()
