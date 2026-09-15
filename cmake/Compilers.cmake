# Detect selected compiler and include matching specific configuration
if(MSVC)
  include(compiler/MSVC)
elseif(CMAKE_CXX_COMPILER_ID MATCHES ".*Clang")
  include(compiler/Clang)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  include(compiler/GCC)
else()
  message(AUTHOR_WARNING "Unknown set as CXX compiler: '${CMAKE_CXX_COMPILER_ID}'")
endif()

# Sets up a PPR project target with standard settings and optional dependency linking
# Usage: setup_ppr_project(target_name
#          [INTERNAL_PUBLIC_DEPS dep1 dep2 ...]
#          [EXTERNAL_SYSTEM_PRIVATE_DEPS dep1 dep2 ...]
#          [EXTERNAL_SYSTEM_PUBLIC_DEPS dep1 dep2 ...])
function(setup_ppr_project project_name)
  set(options)
  set(oneValueArgs)
  set(multiValueArgs INTERNAL_PUBLIC_DEPS EXTERNAL_SYSTEM_PRIVATE_DEPS EXTERNAL_SYSTEM_PUBLIC_DEPS)
  cmake_parse_arguments(PPR_PROJECT "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  # CXX_MODULE_STD requires toolchain support not available in VS18 2026 IDE
  set_target_properties(${project_name} PROPERTIES CXX_MODULE_STD ON)
  target_compile_features(${project_name} PRIVATE cxx_std_23 INTERFACE cxx_std_23)
  # Set compiler warnings and warning as error IFN
  target_compile_options(${project_name} PRIVATE ${PPR_PROJECT_WARNINGS_CXX})
  target_compile_options(${project_name} INTERFACE ${PROJECT_FLAGS_CXX})
  # Add global engine include directories, since legacy headers with macros are still the only way to handle assertions
  target_include_directories(${project_name} PUBLIC ${CMAKE_SOURCE_DIR}/include)
  # Enable code sanitization IFN
  enable_sanitizers(${project_name})

  # Live-only EnC link contract (per-target narrow application point; no-op
  # unless PPR_EDIT_AND_CONTINUE is ON, so msvc-rel static+LTCG is untouched).
  # Mirrors the directory-scoped contract in cmake/compiler/MSVC.cmake.
  if(PPR_EDIT_AND_CONTINUE)
    target_link_options(${project_name} PRIVATE
      "$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Debug>>:LINKER:/DEBUG:FULL>"
      "$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Debug>>:LINKER:/INCREMENTAL>"
      "$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Debug>>:LINKER:/OPT:NOREF>"
      "$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Debug>>:LINKER:/OPT:NOICF>"
      "$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Debug>>:LINKER:/LTCG:OFF>"
      "$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Debug>>:LINKER:/PDBTMCACHE>"
    )
  endif()

  # Link internal public deps (engine targets, public propagation)
  if(PPR_PROJECT_INTERNAL_PUBLIC_DEPS)
    foreach(dep IN LISTS PPR_PROJECT_INTERNAL_PUBLIC_DEPS)
      if(TARGET ${dep})
        target_link_libraries(${project_name} PUBLIC ${dep})
      endif()
    endforeach()
  endif()

  # Link external system private deps (private propagation, SYSTEM includes already set)
  if(PPR_PROJECT_EXTERNAL_SYSTEM_PRIVATE_DEPS)
    foreach(dep IN LISTS PPR_PROJECT_EXTERNAL_SYSTEM_PRIVATE_DEPS)
      if(TARGET ${dep})
        target_link_libraries(${project_name} PRIVATE ${dep})
      endif()
    endforeach()
  endif()

  # Link external system public deps (public propagation, SYSTEM includes already set)
  if(PPR_PROJECT_EXTERNAL_SYSTEM_PUBLIC_DEPS)
    foreach(dep IN LISTS PPR_PROJECT_EXTERNAL_SYSTEM_PUBLIC_DEPS)
      if(TARGET ${dep})
        target_link_libraries(${project_name} PUBLIC ${dep})
      endif()
    endforeach()
  endif()

  # All PPR internal targets use C++20 modules → disable compiler cache
  if(COMMAND ppr_disable_compiler_cache)
    ppr_disable_compiler_cache("${project_name}")
  endif()
endfunction()
