add_library(engine.rhi)

target_sources(engine.rhi
  PUBLIC
    FILE_SET CXX_MODULES FILES
      RHI.cppm
  PRIVATE
      RHI.cpp
)

setup_ppr_project(engine.rhi
  INTERNAL_PUBLIC_DEPS engine.core engine.math engine.shader
  EXTERNAL_SYSTEM_PUBLIC_DEPS slang-rhi slang
)
