# Dear ImGui - Bloat-free Graphical User interface for C++ with minimal dependencies

CPMAddPackage(
    NAME imgui
    GITHUB_REPOSITORY ocornut/imgui
    GIT_TAG v1.91.8-docking
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
    target_compile_definitions(imgui PRIVATE IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
    set_target_properties(imgui PROPERTIES CXX_MODULE_STD OFF)
endif()
