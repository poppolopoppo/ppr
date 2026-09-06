add_library(engine.app)

target_sources(engine.app
        PUBLIC
        FILE_SET CXX_MODULES FILES

        App.cppm
        App.Application.cppm

        input/App.Input.Action.cppm
        input/App.Input.Device.cppm
        input/App.Input.FilteredAnalog.cppm
        input/App.Input.Key.cppm
        input/App.Input.Listener.cppm

        platform/glfw/App.Platform.Glfw.cppm
        platform/glfw/App.Platform.Glfw.Input.cppm
        platform/glfw/App.Platform.Glfw.Player.cppm
        platform/glfw/App.Platform.Glfw.Window.cppm
        platform/App.Platform.cppm

        player/App.Player.cppm
        player/App.Player.Graph.cppm

        renderer/App.Renderer.cppm
        renderer/App.Renderer.Types.cppm
        renderer/App.Renderer.TrianglePass.cppm

        scene/App.Scene.Camera.cppm
        scene/App.Scene.Camera.Controller.cppm

        service/App.Service.Input.cppm
        service/App.Service.Player.cppm
        service/App.Service.UI.cppm
        service/App.Service.Window.cppm

        ui/App.UI.ImGui.cppm

        window/App.Window.Handle.cppm
        window/App.Window.Monitor.cppm
        window/App.Window.Viewport.cppm

        PRIVATE

        App.Application.cpp
        App.TemplateInstantiations.cpp

        input/App.Input.Action.cpp
        input/App.Input.Device.cpp
        input/App.Input.FilteredAnalog.cpp
        input/App.Input.Key.cpp
        input/App.Input.Listener.cpp

        platform/glfw/App.Platform.Glfw.include.hpp
        platform/glfw/App.Platform.Glfw.cpp
        platform/glfw/App.Platform.Glfw.Input.cpp
        platform/glfw/App.Platform.Glfw.Player.cpp
        platform/glfw/App.Platform.Glfw.Window.cpp

        player/App.Player.cpp
        player/App.Player.Graph.cpp

        renderer/App.Renderer.cpp
        renderer/App.Renderer.Types.cpp
        renderer/App.Renderer.TrianglePass.cpp

        scene/App.Scene.Camera.cpp
        scene/App.Scene.Camera.Controller.cpp

        ui/App.UI.ImGui.cpp

        window/App.Window.Handle.cpp
        window/App.Window.Monitor.cpp
        window/App.Window.Viewport.cpp
)

setup_ppr_project(engine.app
        INTERNAL_PUBLIC_DEPS engine.core engine.math engine.shader engine.rhi
        EXTERNAL_SYSTEM_PRIVATE_DEPS glfw mango
        EXTERNAL_SYSTEM_PUBLIC_DEPS imgui.base
)

# `import imgui;` (C++20 binding for Dear ImGui) must be resolvable from every engine.app importer.
target_link_libraries(engine.app PUBLIC imgui)
