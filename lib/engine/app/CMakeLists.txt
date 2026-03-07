add_library(EngineApp)

target_sources(EngineApp
    PUBLIC
        FILE_SET CXX_MODULES FILES
            App.cppm
    PRIVATE
        Application.cpp
)

setup_ppr_project(EngineApp
  INTERNAL_PUBLIC_DEPS EngineCore EngineRHI
  EXTERNAL_SYSTEM_PRIVATE_DEPS glfw
)
