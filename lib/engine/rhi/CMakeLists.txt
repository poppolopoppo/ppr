add_library(EngineRHI)

target_sources(EngineRHI
  PUBLIC
    FILE_SET CXX_MODULES FILES
      RHI.cppm
  PRIVATE
      RHI.cpp
)

setup_ppr_project(EngineRHI
  INTERNAL_PUBLIC_DEPS EngineCore
  EXTERNAL_SYSTEM_PUBLIC_DEPS slang-rhi slang
)
