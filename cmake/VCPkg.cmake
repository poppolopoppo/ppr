# ==============================================================================
# VCPKG TOOLCHAIN CONFIGURATION
# ==============================================================================
# This configuration makes vcpkg optional. It will use the vcpkg toolchain if
# VCPKG_ROOT is set, but will work without it by using system CMake (CPM can
# fetch from source).
#
# Usage:
#   - With vcpkg:    Set VCPKG_ROOT environment variable before running cmake
#   - Without vcpkg: Just run cmake normally, dependencies will be fetched via CPM
# ==============================================================================

# Only configure vcpkg if VCPKG_ROOT is explicitly set by the user
if(DEFINED ENV{VCPKG_ROOT})
    message(STATUS "PPR: VCPKG_ROOT detected, integrating vcpkg toolchain")

    if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
        set(CMAKE_TOOLCHAIN_FILE
                "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
                CACHE STRING "vcpkg toolchain file")
        message(STATUS "PPR: Using vcpkg toolchain from $ENV{VCPKG_ROOT}")
    else()
        message(STATUS "PPR: Using preconfigured cmake toolchain file: ${CMAKE_TOOLCHAIN_FILE}")
    endif()
else()
    message(STATUS "PPR: VCPKG_ROOT not set - using system CMake package discovery and CPM for dependencies")
    message(STATUS "PPR: To use vcpkg, set VCPKG_ROOT environment variable")
endif()

# If VCPKG is being used, read the triplet and configure runtime
if(DEFINED VCPKG_INSTALLED_DIR OR DEFINED ENV{VCPKG_ROOT})
    # Derive runtime from triplet name (Windows only)
    if(DEFINED VCPKG_TARGET_TRIPLET)
        # EnC requires /MDd everywhere: CPM-built slang-rhi, vcpkg libs (e.g. glfw
        # via /Z7 objects linked into a /ZI live binary), and the prebuilt slang
        # binary must agree. A -static triplet forces /MT and breaks EnC (LNK2038).
        if(PPR_EDIT_AND_CONTINUE AND VCPKG_TARGET_TRIPLET MATCHES "-static")
            message(FATAL_ERROR "PPR_EDIT_AND_CONTINUE requires a dynamic vcpkg triplet (/MDd); "
                    "'${VCPKG_TARGET_TRIPLET}' forces /MT which is incompatible with Edit & Continue.")
        endif()
        if(VCPKG_TARGET_TRIPLET MATCHES "-static" AND WIN32)
            set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "" FORCE)
        elseif(WIN32)
            set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL" CACHE STRING "" FORCE)
        endif()
        message(STATUS "PPR: vcpkg triplet: ${VCPKG_TARGET_TRIPLET}")
    endif()
    if(WIN32)
        message(STATUS "PPR: MSVC runtime: ${CMAKE_MSVC_RUNTIME_LIBRARY}")
        # Fail configure on a /MT mix with EnC even when the triplet name was
        # overridden by hand: only a DLL runtime (/MDd) is EnC-compatible.
        if(PPR_EDIT_AND_CONTINUE AND DEFINED CMAKE_MSVC_RUNTIME_LIBRARY AND NOT CMAKE_MSVC_RUNTIME_LIBRARY MATCHES "DLL")
            message(FATAL_ERROR "PPR_EDIT_AND_CONTINUE requires /MDd (CMAKE_MSVC_RUNTIME_LIBRARY with DLL); "
                    "current '${CMAKE_MSVC_RUNTIME_LIBRARY}' forces /MT which breaks Edit & Continue.")
        endif()
    endif()

    # Add vcpkg config-mode packages to CMAKE_PREFIX_PATH for find_package()
    if(DEFINED VCPKG_INSTALLED_DIR)
        message(STATUS "PPR: VCPKG_INSTALLED_DIR=${VCPKG_INSTALLED_DIR}")
        list(APPEND CMAKE_PREFIX_PATH "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share")
        list(APPEND CMAKE_PREFIX_PATH "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib/cmake")
        list(APPEND CMAKE_PREFIX_PATH "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
        message(STATUS "PPR: CMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}")
    endif()
endif()

# Prefer Config packages over Find modules (vcpkg convention)
if(NOT DEFINED CMAKE_FIND_PACKAGE_PREFER_CONFIG)
    set(CMAKE_FIND_PACKAGE_PREFER_CONFIG ON CACHE BOOL "Prefer Config packages over Find modules" FORCE)
else()
    set(CMAKE_FIND_PACKAGE_PREFER_CONFIG ON)
endif()

message(STATUS "PPR: vcpkg configuration complete")
