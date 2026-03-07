add_library(EngineMath)

target_sources(EngineMath
  PUBLIC
    FILE_SET CXX_MODULES FILES
      Math.cppm
)

setup_ppr_project(EngineMath
  INTERNAL_PUBLIC_DEPS EngineCore
  EXTERNAL_SYSTEM_PRIVATE_DEPS mango
)
