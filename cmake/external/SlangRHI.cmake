# Slang-RHI - Shader language and rendering hardware abstraction
# Provides a unified graphics API abstraction layer with support for multiple graphics backends
# NOTE: This package includes Slang transitively via SLANG_RHI_FETCH_SLANG option

set(SLANG_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(SLANG_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)

CPMAddPackage(
    NAME slang-rhi
    GITHUB_REPOSITORY shader-slang/slang-rhi
    GIT_TAG main
    OPTIONS
        "SLANG_RHI_FETCH_SLANG ON"
        "SLANG_RHI_BUILD_SHARED OFF"
        "SLANG_RHI_BUILD_EXAMPLES OFF"
        "SLANG_RHI_BUILD_TESTS OFF"
        "SLANG_RHI_ENABLE_CUDA OFF"
        "SLANG_RHI_ENABLE_OPTIX OFF"
        "SLANG_RHI_ENABLE_CPU OFF"
        "SLANG_RHI_ENABLE_D3D11 OFF"
        "CMAKE_CXX_MODULE_STD OFF"
)

set_target_properties(slang-rhi PROPERTIES UNITY_BUILD ON)
set_target_properties(slang-rhi PROPERTIES CXX_MODULE_STD OFF)

# Mark slang-rhi includes as SYSTEM to suppress warnings from external headers
get_target_property(slang_rhi_inc slang-rhi INTERFACE_INCLUDE_DIRECTORIES)
if(slang_rhi_inc)
    set_target_properties(slang-rhi PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${slang_rhi_inc}")
endif()

# Also mark slang includes as SYSTEM if the target exists
if(TARGET slang)
    get_target_property(slang_inc slang INTERFACE_INCLUDE_DIRECTORIES)
    if(slang_inc)
        set_target_properties(slang PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${slang_inc}")
    endif()
endif()
