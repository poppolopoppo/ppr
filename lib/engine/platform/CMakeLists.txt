add_library(EnginePlatform)

target_sources(EnginePlatform
  PUBLIC
    FILE_SET CXX_MODULES FILES
      Platform.cppm
      IPlatform.cppm
      IWindow.cppm
      IApp.cppm
      IMath.cppm
)

setup_ppr_project(EnginePlatform
  INTERNAL_PUBLIC_DEPS EngineCore EngineMath
)
