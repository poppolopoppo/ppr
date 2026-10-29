# Preserve Vulkan's recoverable swapchain status without changing CPM's cache.
if(NOT SLANG_RHI_ENABLE_VULKAN)
    return()
endif()

set(_surface_original "${slang-rhi_SOURCE_DIR}/src/vulkan/vk-surface.cpp")
file(READ "${_surface_original}" _surface_source)
string(REPLACE "\r\n" "\n" _surface_source "${_surface_source}")
string(SHA256 _surface_hash "${_surface_source}")
if(NOT _surface_hash STREQUAL "a88d8f44e00da3f4961b823e3d146047b5e1cb4bb4b957d350c83d9dd896ee8d")
    message(FATAL_ERROR "Slang-RHI Vulkan surface differs from the reviewed pin")
endif()

# Upstream (2026-10-29) fails every acquire/present result other than success
# and suboptimality, except that vk_present now short-circuits out-of-date to
# SLANG_OK without presenting. The \n escapes in the anchors below keep the
# patterns line-ending independent of this file's checkout on either host.

# Acq→pending: an outdated swapchain acquires no image, so hand the Renderer a
# recoverable status to refence through the fenced reconfiguration path.
set(_surface_acquire_old "    );\n\n    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)")
set(_surface_acquire_new "    );\n\n    // PPR consumes pending as a request to reconfigure, never as a presented frame.\n    if (result == VK_ERROR_OUT_OF_DATE_KHR)\n    {\n        return SLANG_E_PENDING;\n    }\n    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)")

# Present→pending: keep the not-presented state visible so the Renderer's
# settlePresent() path reconfigures instead of accounting the dropped frame as presented.
set(_surface_present_old "    if (result == VK_ERROR_OUT_OF_DATE_KHR)\n        return SLANG_OK;")
set(_surface_present_new "    // PPR consumes pending as a request to reconfigure; this image was not presented.\n    if (result == VK_ERROR_OUT_OF_DATE_KHR)\n    {\n        return SLANG_E_PENDING;\n    }")

string(REPLACE "${_surface_present_old}" "${_surface_present_new}" _surface_source "${_surface_source}")
string(REPLACE "${_surface_acquire_old}" "${_surface_acquire_new}" _surface_source "${_surface_source}")
string(REGEX MATCHALL "SLANG_E_PENDING" _surface_pending_marks "${_surface_source}")
list(LENGTH _surface_pending_marks _surface_pending_count)
if(NOT _surface_pending_count EQUAL 2)
    message(FATAL_ERROR "Slang-RHI Vulkan surface pending replacements did not apply exactly twice")
endif()

set(_surface_directory "${CMAKE_CURRENT_BINARY_DIR}/ppr-slang-rhi")
file(MAKE_DIRECTORY "${_surface_directory}")
file(WRITE "${_surface_directory}/vk-surface.cpp.in" "${_surface_source}")
configure_file("${_surface_directory}/vk-surface.cpp.in" "${_surface_directory}/vk-surface.cpp" COPYONLY)

get_target_property(_surface_sources slang-rhi SOURCES)
set(_surface_replaced FALSE)
foreach(_surface_file IN LISTS _surface_sources)
    if(_surface_file STREQUAL "src/vulkan/vk-surface.cpp" OR _surface_file STREQUAL "${_surface_original}")
        list(APPEND _surface_patched_sources "${_surface_directory}/vk-surface.cpp")
        set(_surface_replaced TRUE)
    else()
        list(APPEND _surface_patched_sources "${_surface_file}")
    endif()
endforeach()
if(NOT _surface_replaced)
    message(FATAL_ERROR "Slang-RHI Vulkan surface source registration was not found")
endif()
set_property(TARGET slang-rhi PROPERTY SOURCES "${_surface_patched_sources}")
target_include_directories(slang-rhi PRIVATE "${slang-rhi_SOURCE_DIR}/src/vulkan")
