# Dear ImGui - Bloat-free Graphical User interface for C++ with minimal dependencies

CPMAddPackage(
    NAME imgui
    GITHUB_REPOSITORY ocornut/imgui
    GIT_TAG v1.92.9b-docking
)

if(imgui_ADDED)
    # Create a static library from ImGui's core source files
    add_library(imgui STATIC
        "${imgui_SOURCE_DIR}/imgui.cpp"
        "${imgui_SOURCE_DIR}/imgui_demo.cpp"
        "${imgui_SOURCE_DIR}/imgui_draw.cpp"
        "${imgui_SOURCE_DIR}/imgui_tables.cpp"
        "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    )

    target_include_directories(imgui SYSTEM PUBLIC "${imgui_SOURCE_DIR}")
    set_target_properties(imgui PROPERTIES CXX_MODULE_STD OFF)
endif()

# C++20 module bindings for Dear ImGui (MIT, https://github.com/stripe2933/imgui-module).
# `generated/imgui.cppm` re-exports every public imgui.h symbol via `using` declarations, so
# consumers write `import imgui;` instead of textually including <imgui.h>. `imgui_internal.cppm`
# (implicitly exporting `imgui`) provides the full `ImGuiContext` definition the engine needs to
# access `ImGuiContext::ErrorCallback`. Both are the "Combined Module" form (v1.92.9b), supporting
# the master and docking branches (docking-only symbols are guarded by IMGUI_HAS_DOCK, which the
# docking-pinned `imgui` static library above defines). We download only the generated module
# files (NOT configuring the third-party CMake, which would pull in example backends) and build
# them ourselves.
set(_imgui_module_dir "${CMAKE_CURRENT_BINARY_DIR}/imgui_module_bindings")
set(_imgui_module_files imgui.cppm imgui_internal.cppm)
foreach(_imgui_module_file ${_imgui_module_files})
    set(_imgui_module_src "${_imgui_module_dir}/${_imgui_module_file}")
    if(NOT EXISTS "${_imgui_module_src}")
        file(DOWNLOAD
            https://raw.githubusercontent.com/stripe2933/imgui-module/v1.92.9b/generated/${_imgui_module_file}
            "${_imgui_module_src}"
            TLS_VERIFY ON
            STATUS _imgui_module_dl_status
            LOG _imgui_module_dl_log)
        list(GET _imgui_module_dl_status 0 _imgui_module_dl_code)
        if(NOT _imgui_module_dl_code EQUAL 0)
            message(FATAL_ERROR "Failed to download imgui-module binding ${_imgui_module_file}: ${_imgui_module_dl_log}")
        endif()
    endif()
endforeach()

add_library(ImGuiModule)
target_sources(ImGuiModule
    PUBLIC
        FILE_SET CXX_MODULES FILES
        "${_imgui_module_dir}/imgui.cppm"
        "${_imgui_module_dir}/imgui_internal.cppm"
)
target_link_libraries(ImGuiModule PUBLIC imgui)
# CXX_MODULE_STD OFF: CMake 4.4 root-scope std-module synthetic-target link workaround
# (`@cmake_cxx_std.lib` LNK2001) — see AGENTS.md "CMake Version Tracking"; re-test on newer CMake.
set_target_properties(ImGuiModule PROPERTIES CXX_MODULE_STD OFF)
