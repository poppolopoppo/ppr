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
        GIT_TAG 358026169e216d145e9b64301b614ae834f15716
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
        "BUILD_TESTS OFF"
        "BUILD_TOOLS OFF"
)

# Remove /MP, /arch:AVX*, and /Ox from Mango targets' INTERFACE_COMPILE_OPTIONS
# so they don't propagate to PPR targets. These per-target flags cause CMake to
# create separate @cmake_cxx_std synth targets with different flags, triggering
# "Disagreement of the location of the 'std' module" errors and C2678 type
# mismatches between synth targets. /Ox is genex-wrapped
# ($<$<CONFIG:Release>:/Ox>) in mango's CMakeLists. CXX_MODULE_STD OFF
# explicitly normalizes Mango's non-module targets and prevents them from
# providing or conflicting through CMake standard-library-module paths.
foreach(mango_target IN ITEMS mango mango-core mango-image mango-import3d mango-window mango-opengl mango-vulkan)
    if(TARGET ${mango_target})
        set_target_properties(${mango_target} PROPERTIES CXX_MODULE_STD OFF)

        get_target_property(_mango_iface_opts ${mango_target} INTERFACE_COMPILE_OPTIONS)
        if(_mango_iface_opts)
            list(FILTER _mango_iface_opts EXCLUDE REGEX "^/MP$|^/arch:AVX|/Ox|^-m(avx512|avx|sse|bmi|fma|sha|aes|pclmul|popcnt|f16c|lzcnt)")
            set_target_properties(${mango_target} PROPERTIES INTERFACE_COMPILE_OPTIONS "${_mango_iface_opts}")
        endif()

        # Mark each mango target's own includes as SYSTEM to suppress warnings
        # from external headers. The include dir is carried PUBLIC by mango-core
        # (and siblings) and inherited transitively by the mango umbrella, so
        # marking only mango itself is a no-op.
        get_target_property(_mango_target_inc ${mango_target} INTERFACE_INCLUDE_DIRECTORIES)
        if(_mango_target_inc)
            set_target_properties(${mango_target} PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_mango_target_inc}")
        endif()

        # vcpkg builds mango's deps (fmt, ...) against libstdc++; PPR links
        # libc++. Propagate the missing DSO on Linux so final links resolve
        # libstdc++ symbols referenced from those static archives. It must be
        # a single -Wl, flag: the clang driver silently drops a bare -lstdc++
        # under -stdlib=libc++. The --no-as-needed guard is also required:
        # Ubuntu ld drops the DSO otherwise, even with symbols pending.
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            target_link_libraries(${mango_target} INTERFACE "-Wl,--no-as-needed,-lstdc++,--as-needed")
        endif()
    endif()
endforeach()
