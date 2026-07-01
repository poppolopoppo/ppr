add_library(EngineApp)

target_sources(EngineApp
    PUBLIC
        FILE_SET CXX_MODULES FILES
            App.cppm
            InputService.cppm
    PRIVATE
        Application.cpp
        InputService.cpp
)

setup_ppr_project(EngineApp
  INTERNAL_PUBLIC_DEPS EngineCore EngineRHI EnginePlatform
  EXTERNAL_SYSTEM_PRIVATE_DEPS glfw
)
