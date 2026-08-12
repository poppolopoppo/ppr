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

# In dev (ASAN) builds, make STL container annotations neutral for LNK2038
# while keeping them active at runtime (see cmake/compiler/MSVC.cmake).
if (PPR_ENABLE_SANITIZER_ADDRESS)
    set(MANGO_ANNOTATE_STL_FLAG "/D_ANNOTATE_STL")
endif ()

CPMAddPackage(
    NAME mango
    #GITHUB_REPOSITORY t0rakka/mango
    #GIT_TAG main
        GITHUB_REPOSITORY poppolopoppo/mango
        GIT_TAG v0.2-pre-release
    CMAKE_ARGS
        "-DCMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}"
        "-DVCPKG_TARGET_TRIPLET=${VCPKG_TARGET_TRIPLET}"
        "-DCMAKE_CXX_FLAGS=${MANGO_ANNOTATE_STL_FLAG}"
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

# Remove /MP and /arch:AVX* from mango's INTERFACE_COMPILE_OPTIONS so they
# don't propagate to PPR targets. These per-target flags cause CMake to create
# separate @cmake_cxx_std synth targets with different flags, triggering
# "Disagreement of the location of the 'std' module" errors and C2678 type
# mismatches between synth targets. mango doesn't use import std; its /MP and
# /arch:AVX2 remain on mango's own compilation (via PRIVATE/PUBLIC), but PPR
# consumers get consistent flags from the global add_compile_options instead.
get_target_property(_mango_iface_opts mango INTERFACE_COMPILE_OPTIONS)
if(_mango_iface_opts)
    list(FILTER _mango_iface_opts EXCLUDE REGEX "^/MP$|^/arch:AVX")
    set_target_properties(mango PROPERTIES INTERFACE_COMPILE_OPTIONS "${_mango_iface_opts}")
endif()

# Mark mango includes as SYSTEM to suppress warnings from external headers
get_target_property(mango_inc mango INTERFACE_INCLUDE_DIRECTORIES)
if(mango_inc)
    set_target_properties(mango PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${mango_inc}")
endif()
