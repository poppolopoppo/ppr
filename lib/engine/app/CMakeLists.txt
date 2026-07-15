add_library(EngineApp)

target_sources(EngineApp
    PUBLIC
        FILE_SET CXX_MODULES FILES
        App.cppm
        App.Application.cppm

        input/device/App.Input.Gamepad.cppm
        input/device/App.Input.Keyboard.cppm
        input/device/App.Input.Mouse.cppm

        input/App.Input.Action.cppm
        input/App.Input.Device.cppm
        input/App.Input.Key.cppm
        input/App.Input.Listener.cppm
        input/App.Input.Mapping.cppm
        App.Player.cppm

        platform/Glfw/App.Platform.Glfw.cppm
        platform/Glfw/App.Platform.Glfw.Input.cppm
        platform/Glfw/App.Platform.Glfw.Window.cppm
        platform/App.Platform.cppm

        service/App.Service.Input.cppm
        service/App.Service.Player.cppm
        service/App.Service.Window.cppm

        window/App.Window.Handle.cppm
        window/App.Window.Monitor.cppm
    PRIVATE
        App.Application.cpp

        input/device/App.Input.Gamepad.cpp
        input/device/App.Input.Keyboard.cpp
        input/device/App.Input.Mouse.cpp

        input/App.Input.Action.cpp
        input/App.Input.Key.cpp
        input/App.Input.Listener.cpp
        input/App.Input.Mapping.cpp
        App.Player.cpp

        window/App.Window.Handle.cpp
        window/App.Window.Monitor.cpp

        platform/Glfw/App.Platform.Glfw.cpp
        platform/Glfw/App.Platform.Glfw.Input.cpp
        platform/Glfw/App.Platform.Glfw.Window.cpp
)

setup_ppr_project(EngineApp
        INTERNAL_PUBLIC_DEPS EngineCore EngineMath EngineRHI
        EXTERNAL_SYSTEM_PRIVATE_DEPS glfw
)
