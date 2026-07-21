option(ENABLE_CACHE "Enable cache if available" OFF)
if(NOT ENABLE_CACHE)
  return()
endif()

set(CACHE_OPTION "ccache" CACHE STRING "Compiler cache to be used")
set(CACHE_OPTION_VALUES "ccache" "sccache")
set_property(CACHE CACHE_OPTION PROPERTY STRINGS ${CACHE_OPTION_VALUES})
list(FIND CACHE_OPTION_VALUES ${CACHE_OPTION} CACHE_OPTION_INDEX)

if(${CACHE_OPTION_INDEX} EQUAL -1)
  message(STATUS "Using custom compiler cache system: '${CACHE_OPTION}', explicitly supported entries are ${CACHE_OPTION_VALUES}")
endif()

find_program(CACHE_BINARY NAMES ${CACHE_OPTION_VALUES})
if(CACHE_BINARY)
  # Set launcher globally for ALL targets.
  # Targets that use C++20 modules MUST call ppr_disable_compiler_cache() to opt out.
  # This is safe because:
  #   - Non-module TUs (all external deps, .cpp impl files) are correctly cached.
  #   - C++20 module TUs cannot be safely cached (ccache doesn't track BMI content).
  #   - MSVC /Zi PDB race is avoided: PPR uses /Z7 via CMAKE_MSVC_DEBUG_INFORMATION_FORMAT.
  set(CMAKE_C_COMPILER_LAUNCHER "${CACHE_BINARY}")
  set(CMAKE_CXX_COMPILER_LAUNCHER "${CACHE_BINARY}")
  message(STATUS "${CACHE_BINARY} found and enabled for non-module TUs")

  # Per-target opt-out for C++20 module targets.
  function(ppr_disable_compiler_cache target)
    set_property(TARGET "${target}" PROPERTY C_COMPILER_LAUNCHER "")
    set_property(TARGET "${target}" PROPERTY CXX_COMPILER_LAUNCHER "")
  endfunction()
else()
  message(WARNING "${CACHE_OPTION} is enabled but was not found. Not using it")
endif()
