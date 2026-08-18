add_library(EngineApp)

target_sources(EngineApp
    PUBLIC
        FILE_SET CXX_MODULES FILES
        App.cppm
        App.Application.cppm
        App.Imgui.cppm

        camera/App.Camera.cppm

        input/device/App.Input.Gamepad.cppm
        input/device/App.Input.Keyboard.cppm
        input/device/App.Input.Mouse.cppm

        input/App.Input.Action.cppm
        input/App.Input.Device.cppm
        input/App.Input.Key.cppm
        input/App.Input.Listener.cppm
        input/App.Input.Mapping.cppm
        input/App.Input.Replay.cppm

        platform/glfw/App.Platform.Glfw.cppm
        platform/glfw/App.Platform.Glfw.Input.cppm
        platform/glfw/App.Platform.Glfw.Player.cppm
        platform/glfw/App.Platform.Glfw.Window.cppm
        platform/App.Platform.cppm

        player/App.Player.cppm
        player/App.Player.Graph.cppm

        renderer/App.Renderer.cppm
        renderer/App.Viewport.cppm

        service/App.Service.Input.cppm
        service/App.Service.Player.cppm
        service/App.Service.UI.cppm
        service/App.Service.Window.cppm

        window/App.Window.Handle.cppm
        window/App.Window.Monitor.cppm
    PRIVATE
        App.Application.cpp

        camera/App.Camera.cpp

        input/device/App.Input.Gamepad.cpp
        input/device/App.Input.Keyboard.cpp
        input/device/App.Input.Mouse.cpp
        input/App.Input.Action.cpp
        input/App.Input.Key.cpp
        input/App.Input.Listener.cpp
        input/App.Input.Mapping.cpp
        input/App.Input.Replay.cpp

        platform/glfw/App.Platform.Glfw.include.hpp
        platform/glfw/App.Platform.Glfw.cpp
        platform/glfw/App.Platform.Glfw.Input.cpp
        platform/glfw/App.Platform.Glfw.Player.cpp
        platform/glfw/App.Platform.Glfw.Window.cpp

        player/App.Player.cpp
        player/App.Player.Graph.cpp

        window/App.Window.Handle.cpp
        window/App.Window.Monitor.cpp

        renderer/App.Renderer.cpp
        renderer/App.Viewport.cpp

        service/App.Service.UI.cpp

        App.TemplateInstantiations.cpp
)

setup_ppr_project(EngineApp
        INTERNAL_PUBLIC_DEPS EngineCore EngineMath EngineShader EngineRHI
        EXTERNAL_SYSTEM_PRIVATE_DEPS glfw mango
        EXTERNAL_SYSTEM_PUBLIC_DEPS imgui
)

# `import imgui;` (C++20 binding for Dear ImGui) must be resolvable from every engine.app importer.
target_link_libraries(EngineApp PUBLIC ImGuiModule)
