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

option(PPR_NULL_PLATFORM "Build with null platform backend (headless/testing)" OFF)
if(PPR_NULL_PLATFORM)
    target_compile_definitions(EngineApp PRIVATE PPR_NULL_PLATFORM=1)
endif()

setup_ppr_project(EngineApp
  INTERNAL_PUBLIC_DEPS EngineCore EngineRHI EnginePlatform
  EXTERNAL_SYSTEM_PRIVATE_DEPS glfw
)
