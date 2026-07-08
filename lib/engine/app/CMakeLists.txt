add_library(EngineApp)

target_sources(EngineApp
    PUBLIC
        FILE_SET CXX_MODULES FILES
            App.cppm

            input/device/App.Input.Gamepad.cppm
            input/device/App.Input.Keyboard.cppm
            input/device/App.Input.Mouse.cppm

            input/App.Input.Action.cppm
            input/App.Input.Device.cppm
            input/App.Input.Key.cppm
            input/App.Input.Listener.cppm
            input/App.Input.Mapping.cppm

            App.Service.Input.cppm
    PRIVATE
        Application.cpp

        input/device/App.Input.Gamepad.cpp
        input/device/App.Input.Keyboard.cpp
        input/device/App.Input.Mouse.cpp

        input/App.Input.Action.cpp
        input/App.Input.Key.cpp
        input/App.Input.Listener.cpp
        input/App.Input.Mapping.cpp
)

setup_ppr_project(EngineApp
  INTERNAL_PUBLIC_DEPS EngineCore EngineMath EngineRHI
  EXTERNAL_SYSTEM_PRIVATE_DEPS glfw
)
