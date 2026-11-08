# ==============================================================================
# DEFAULT COMPILER SELECTION
# ==============================================================================
# PPR requires Clang on Linux: cmake/compiler/Clang.cmake passes -stdlib=libc++
# (GCC rejects it), the build depends on C++23 modules with `import std`, and
# vcpkg ports must use the matching libc++ ABI.
#
# Presets name the compiler explicitly. A configuration that does not (for
# example an IDE profile that inherits the system toolchain) would otherwise
# pick the host's default g++, which cannot build this project. Default to
# Clang only while the toolchain is still unselected; an explicit GNU choice is
# rejected by cmake/Compilers.cmake with an actionable error instead of being
# overridden.
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND NOT DEFINED CMAKE_CXX_COMPILER)
  find_program(PPR_DEFAULT_CLANGXX NAMES clang++ NO_CACHE)
  find_program(PPR_DEFAULT_CLANG NAMES clang NO_CACHE)

  if(PPR_DEFAULT_CLANGXX)
    message(STATUS "PPR: no compiler selected; defaulting to Clang: ${PPR_DEFAULT_CLANGXX}")
    set(CMAKE_CXX_COMPILER "${PPR_DEFAULT_CLANGXX}" CACHE FILEPATH "C++ compiler")

    if(NOT DEFINED CMAKE_C_COMPILER AND PPR_DEFAULT_CLANG)
      set(CMAKE_C_COMPILER "${PPR_DEFAULT_CLANG}" CACHE FILEPATH "C compiler")
    endif()

    set(PPR_DEFAULTED_CLANG TRUE)
  else()
    message(WARNING "PPR: no compiler selected and Clang was not found; "
            "configure will fail because GCC cannot build the C++23 module targets.")
  endif()

  unset(PPR_DEFAULT_CLANGXX)
  unset(PPR_DEFAULT_CLANG)
endif()
