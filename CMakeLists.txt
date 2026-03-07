cmake_minimum_required(VERSION 4.2 FATAL_ERROR)

include(cmake/PreventInSourceBuilds.cmake)

# Enable C++23 module standard library support BEFORE project()
# This is required for CMake to properly handle FILE_SET CXX_MODULES
if(CMAKE_VERSION VERSION_LESS "4.3.0")
    # https://github.com/Kitware/CMake/blob/v4.2.4/Help/dev/experimental.rst
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "d0edc3af-4c50-42ea-a356-e2862fe7a444")
else()
    # https://github.com/Kitware/CMake/blob/v4.3.2/Help/dev/experimental.rst
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")
endif()
set(CMAKE_EXPERIMENTAL_EXPORT_BUILD_DATABASE "73194a1d-c0b5-41b9-9190-a4512925e192")
# set(CMAKE_EXPERIMENTAL_EXPORT_PACKAGE_DEPENDENCIES "1942b4fa-b2c5-4546-9385-83f254070067")
# set(CMAKE_EXPERIMENTAL_EXPORT_PACKAGE_INFO "b80be207-778e-46ba-8080-b23bba22639e")
# set(CMAKE_EXPERIMENTAL_FIND_CPS_PACKAGES "e82e467b-f997-4464-8ace-b00808fff261")

# Set defaults for standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)
set(CMAKE_CXX_MODULE_STD ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_EXPORT_BUILD_DATABASE ON)

include(cmake/VCPkg.cmake)

project(
        PPR
        VERSION 0.1.0
        DESCRIPTION "C++ Video Game with Slang, GLFW, and C++20 Modules"
        LANGUAGES CXX C)

#include(CTest)
#enable_testing()

# Define sanitizer and static analyzer options
option(PPR_ENABLE_DEVELOPER_MODE "Enable developer mode with warning-as-errors and sanitizers" FALSE)
option(PPR_ENABLE_COVERAGE "Enable coverage reporting for gcc/clang" FALSE)
option(PPR_ENABLE_SANITIZER_ADDRESS "Enable address sanitizer" FALSE)
option(PPR_ENABLE_SANITIZER_LEAK "Enable leak sanitizer" FALSE)
option(PPR_ENABLE_SANITIZER_UNDEFINED "Enable undefined behavior sanitizer" FALSE)
option(PPR_ENABLE_SANITIZER_THREAD "Enable thread sanitizer" FALSE)
option(PPR_ENABLE_SANITIZER_MEMORY "Enable memory sanitizer" FALSE)
option(PPR_ENABLE_UNITY_BUILD "Enable unity build for faster builds" FALSE)
option(PPR_ENABLE_CLANG_TIDY "Enable clang-tidy" FALSE)
option(PPR_ENABLE_CPPCHECK "Enable cppcheck" FALSE)
option(PPR_WARNINGS_AS_ERRORS "Treat warnings as errors" FALSE)

if(PPR_ENABLE_DEVELOPER_MODE)
    message(STATUS "Enable developer mode with config=${CMAKE_BUILD_TYPE}")
    #set(ENABLE_CACHE TRUE) # keep ccache as a separate opt-in
    set(PPR_ENABLE_SANITIZER_ADDRESS TRUE)
    set(PPR_ENABLE_SANITIZER_UNDEFINED TRUE)
    #set(PPR_ENABLE_CLANG_TIDY TRUE) # clang-tidy is not working with modules yet, so we need to disable it for now
    set(PPR_ENABLE_CPPCHECK TRUE)
    set(PPR_WARNINGS_AS_ERRORS TRUE)
endif()

# Include helper modules
list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

include(HAL)
include(Compilers)
include(Sanitizers)
include(StaticAnalyzers)
include(Cache)
include(Dependencies)

# Enable global static analyzers if requested
if(PPR_ENABLE_CPPCHECK)
    enable_cppcheck(${PPR_WARNINGS_AS_ERRORS} "")
endif()

# Add sourced directories
add_subdirectory(lib)
add_subdirectory(game)
