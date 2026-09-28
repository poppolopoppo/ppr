# Validate the activated native MSVC toolset before vcpkg or project() can
# discover MinGW tools from PATH or reuse a stale CMake cache.
function(_ppr_msvc_toolchain_error reason)
    message(FATAL_ERROR "${reason}\n"
            "Activate VS 18 Insiders with its x64 vcvars64.bat, then run "
            "cmake --preset <preset> (msvc-dev, msvc-rel, or msvc-live). "
            "Only if an existing build tree was previously configured with a different toolset, "
            "run cmake --fresh --preset <preset> once.")
endfunction()

if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
    _ppr_msvc_toolchain_error("Native MSVC presets require a Windows host.")
endif()

if("$ENV{VSINSTALLDIR}" STREQUAL "" OR "$ENV{VCToolsInstallDir}" STREQUAL "" OR "$ENV{VCToolsVersion}" STREQUAL "")
    _ppr_msvc_toolchain_error("VSINSTALLDIR, VCToolsInstallDir, and VCToolsVersion must come from VS 18 Insiders vcvars64.bat.")
endif()

file(TO_CMAKE_PATH "$ENV{VSINSTALLDIR}" _ppr_vs_install)
file(TO_CMAKE_PATH "$ENV{VCToolsInstallDir}" _ppr_vc_tools)
string(REGEX REPLACE "/+$" "" _ppr_vs_install "${_ppr_vs_install}")
string(REGEX REPLACE "/+$" "" _ppr_vc_tools "${_ppr_vc_tools}")

if(NOT IS_ABSOLUTE "${_ppr_vs_install}" OR NOT IS_ABSOLUTE "${_ppr_vc_tools}" OR
   NOT _ppr_vs_install MATCHES "/18/Insiders$")
    _ppr_msvc_toolchain_error("VSINSTALLDIR must identify an absolute VS 18 Insiders installation; VCToolsInstallDir must be absolute.")
endif()

file(TO_CMAKE_PATH "$ENV{VCPKG_VISUAL_STUDIO_PATH}" _ppr_vcpkg_vs_install)
string(REGEX REPLACE "/+$" "" _ppr_vcpkg_vs_install "${_ppr_vcpkg_vs_install}")
string(TOLOWER "${_ppr_vs_install}" _ppr_vs_install_lower)
string(TOLOWER "${_ppr_vcpkg_vs_install}" _ppr_vcpkg_vs_install_lower)
if(NOT "$ENV{VCPKG_VISUAL_STUDIO_PATH}" STREQUAL "" AND
   NOT _ppr_vcpkg_vs_install_lower STREQUAL _ppr_vs_install_lower)
    _ppr_msvc_toolchain_error("VCPKG_VISUAL_STUDIO_PATH must match the activated VS 18 Insiders VSINSTALLDIR; got '$ENV{VCPKG_VISUAL_STUDIO_PATH}'.")
endif()
set(ENV{VCPKG_VISUAL_STUDIO_PATH} "${_ppr_vs_install}")

if(NOT "$ENV{VCToolsVersion}" MATCHES "^[0-9]+(\\.[0-9]+)+$" OR "$ENV{VCToolsVersion}" VERSION_LESS "14.51")
    _ppr_msvc_toolchain_error("VS 18 Insiders MSVC 14.51 or newer is required; VCToolsVersion is '$ENV{VCToolsVersion}'.")
endif()

set(_ppr_expected_vc_tools "${_ppr_vs_install}/VC/Tools/MSVC/$ENV{VCToolsVersion}")
string(TOLOWER "${_ppr_expected_vc_tools}" _ppr_expected_vc_tools_lower)
string(TOLOWER "${_ppr_vc_tools}" _ppr_vc_tools_lower)
if(NOT _ppr_vc_tools_lower STREQUAL _ppr_expected_vc_tools_lower OR NOT IS_DIRECTORY "${_ppr_vc_tools}")
    _ppr_msvc_toolchain_error("VCToolsInstallDir '${_ppr_vc_tools}' does not match the active VS 18 Insiders toolset '${_ppr_expected_vc_tools}'.")
endif()

if(NOT "$ENV{VSCMD_ARG_HOST_ARCH}" STREQUAL "x64" OR NOT "$ENV{VSCMD_ARG_TGT_ARCH}" STREQUAL "x64")
    _ppr_msvc_toolchain_error("Native MSVC presets require vcvars64.bat (x64 host and x64 target).")
endif()

set(_ppr_segment_heap "${_ppr_vs_install}/Common7/IDE/CommonExtensions/Microsoft/CMake/cmake/Microsoft/SegmentHeap.cmake")
if(NOT EXISTS "${_ppr_segment_heap}")
    _ppr_msvc_toolchain_error("VS 18 Insiders SegmentHeap.cmake is missing: '${_ppr_segment_heap}'.")
endif()

file(TO_CMAKE_PATH "${CMAKE_PROJECT_TOP_LEVEL_INCLUDES}" _ppr_cached_segment_heap)
string(TOLOWER "${_ppr_segment_heap}" _ppr_segment_heap_lower)
string(TOLOWER "${_ppr_cached_segment_heap}" _ppr_cached_segment_heap_lower)
if(NOT _ppr_cached_segment_heap_lower STREQUAL _ppr_segment_heap_lower)
    _ppr_msvc_toolchain_error("CMAKE_PROJECT_TOP_LEVEL_INCLUDES must point to the activated VS 18 Insiders SegmentHeap.cmake; got '${CMAKE_PROJECT_TOP_LEVEL_INCLUDES}'.")
endif()

foreach(_ppr_tool IN ITEMS cl link lib)
    set(_ppr_tool_path "${_ppr_vc_tools}/bin/Hostx64/x64/${_ppr_tool}.exe")
    if(NOT EXISTS "${_ppr_tool_path}")
        _ppr_msvc_toolchain_error("Activated MSVC ${_ppr_tool}.exe is missing: '${_ppr_tool_path}'.")
    endif()
endforeach()

foreach(_ppr_selection IN ITEMS CMAKE_C_COMPILER CMAKE_CXX_COMPILER CMAKE_LINKER CMAKE_AR)
    if(_ppr_selection STREQUAL "CMAKE_LINKER")
        set(_ppr_expected_tool "${_ppr_vc_tools}/bin/Hostx64/x64/link.exe")
    elseif(_ppr_selection STREQUAL "CMAKE_AR")
        set(_ppr_expected_tool "${_ppr_vc_tools}/bin/Hostx64/x64/lib.exe")
    else()
        set(_ppr_expected_tool "${_ppr_vc_tools}/bin/Hostx64/x64/cl.exe")
    endif()

    file(TO_CMAKE_PATH "${${_ppr_selection}}" _ppr_cached_tool)
    string(TOLOWER "${_ppr_cached_tool}" _ppr_cached_tool_lower)
    string(TOLOWER "${_ppr_expected_tool}" _ppr_expected_tool_lower)
    if(NOT _ppr_cached_tool_lower STREQUAL _ppr_expected_tool_lower)
        _ppr_msvc_toolchain_error("${_ppr_selection} must match the activated VS 18 Insiders toolset: expected '${_ppr_expected_tool}', got '${${_ppr_selection}}'.")
    endif()
endforeach()
