# Preserve Vulkan's recoverable swapchain status without changing CPM's cache.
if(NOT SLANG_RHI_ENABLE_VULKAN)
    return()
endif()

set(_surface_original "${slang-rhi_SOURCE_DIR}/src/vulkan/vk-surface.cpp")
file(READ "${_surface_original}" _surface_source)
string(REPLACE "\r\n" "\n" _surface_source "${_surface_source}")
string(SHA256 _surface_hash "${_surface_source}")
if(NOT _surface_hash STREQUAL "bd2e692c4664ae751f1a2be388fda30b92581f1fbe3533ac0fded0519088a007")
    message(FATAL_ERROR "Slang-RHI Vulkan surface differs from the reviewed pin")
endif()

set(_surface_old "    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)")
set(_surface_new "    // PPR consumes pending as a request to reconfigure, never as a presented frame.
    if (result == VK_ERROR_OUT_OF_DATE_KHR)
    {
        return SLANG_E_PENDING;
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)")
string(REPLACE "${_surface_old}" "${_surface_new}" _surface_source "${_surface_source}")

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
