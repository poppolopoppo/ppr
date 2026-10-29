# Dear ImGui - Bloat-free Graphical User interface for C++ with minimal dependencies

CPMAddPackage(
    NAME imgui
    GITHUB_REPOSITORY ocornut/imgui
    GIT_TAG v1.93.0-docking
)

if(imgui_ADDED)
    # Create a static library from ImGui's core source files
    add_library(imgui.base STATIC
        "${imgui_SOURCE_DIR}/imgui.cpp"
        "${imgui_SOURCE_DIR}/imgui_demo.cpp"
        "${imgui_SOURCE_DIR}/imgui_draw.cpp"
        "${imgui_SOURCE_DIR}/imgui_tables.cpp"
        "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    )

    # M4 evaluation (opt-in, PPR_IMGUI_FREETYPE=ON): swap the atlas loader from
    # `imgui_draw.cpp`'s bundled stb_truetype to the vendored FreeType wrapper
    # shipped in the pinned tree. The wrapper TU is the same revision as the
    # rest of imgui.base (GIT_TAG above), so no separate pin can drift.
    #
    # PUBLIC (not PRIVATE) IMGUI_ENABLE_FREETYPE: imgui_internal.h:127-131
    # compiles the stb loader OUT only when this macro is defined, and the
    # generated `imgui` module bindings are compiled inside this directory and
    # re-export the same headers from the same build — the definition must
    # reach every consumer (module BMIs and `engine.app`) so the loaded loader
    # and the `imgui.h` view stay consistent.
    if(PPR_IMGUI_FREETYPE)
        target_sources(imgui.base PRIVATE "${imgui_SOURCE_DIR}/misc/freetype/imgui_freetype.cpp")
        target_compile_definitions(imgui.base PUBLIC IMGUI_ENABLE_FREETYPE)
        target_link_libraries(imgui.base PRIVATE Freetype::Freetype)
    endif()

    target_include_directories(imgui.base SYSTEM PUBLIC "${imgui_SOURCE_DIR}")
    set_target_properties(imgui.base PROPERTIES CXX_MODULE_STD OFF)
endif()

# C++20 module bindings for Dear ImGui (MIT, https://github.com/stripe2933/imgui-module).
# `generated/imgui.cppm` re-exports every public imgui.h symbol via `using` declarations, so
# consumers write `import imgui;` instead of textually including <imgui.h>. `imgui_internal.cppm`
# (implicitly exporting `imgui`) provides the full `ImGuiContext` definition the engine needs to
# access `ImGuiContext::ErrorCallback`. Both are the "Combined Module" form (v1.93.0), supporting
# the master and docking branches (docking-only symbols are guarded by IMGUI_HAS_DOCK, which the
# docking-pinned `imgui.base` static library above defines). We download only the generated module
# files (NOT configuring the third-party CMake, which would pull in example backends) and build
# them ourselves.
#
# The bindings are downloaded ONCE into a location shared by every CMake build preset
# (`out/build/imgui_module_bindings`) instead of per-preset `CMAKE_CURRENT_BINARY_DIR`. This keeps
# a single physical `export module imgui;` declaration on disk, which prevents CLion's C++20 module
# indexer from reporting "Module 'imgui' is ambiguous" when several build directories coexist.
set(_imgui_module_dir "${CMAKE_BINARY_DIR}/../imgui_module_bindings")
file(MAKE_DIRECTORY "${_imgui_module_dir}")
set(_imgui_module_files imgui.cppm imgui_internal.cppm)
foreach(_imgui_module_file ${_imgui_module_files})
    set(_imgui_module_src "${_imgui_module_dir}/${_imgui_module_file}")
    if(NOT EXISTS "${_imgui_module_src}")
        file(DOWNLOAD
            https://raw.githubusercontent.com/stripe2933/imgui-module/v1.93.0/generated/${_imgui_module_file}
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

add_library(imgui)
target_sources(imgui
    PUBLIC
        FILE_SET CXX_MODULES FILES
        "${_imgui_module_dir}/imgui.cppm"
        "${_imgui_module_dir}/imgui_internal.cppm"
)
target_link_libraries(imgui PUBLIC imgui.base)
# CXX_MODULE_STD OFF: CMake 4.4 root-scope std-module synthetic-target link workaround
# (`@cmake_cxx_std.lib` LNK2001) — see AGENTS.md "CMake Version Tracking"; re-test on newer CMake.
set_target_properties(imgui PROPERTIES CXX_MODULE_STD OFF)
# The upstream-generated `imgui.cppm` binding has an extra `;` after a member
# body (e.g. `CheckVersion`), which clang reports as `-Wc++98-compat-extra-semi`
# and project `-Werror` promotes. Silence only that warning for this generated
# target; project-wide `-Werror` is untouched. MSVC path unaffected (genex is
# Clang-only, matching `compiler/Clang.cmake` which also serves clang-cl).
# PUBLIC (not PRIVATE): CMake precompiles a provider's BMI with the importing
# target's flags, so the suppression must travel on imgui's interface to reach
# the `imgui@synth_*` BMI compiles. Consumers only gain one extra `-Wno-*`.
target_compile_options(imgui PUBLIC $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wno-c++98-compat-extra-semi>)
