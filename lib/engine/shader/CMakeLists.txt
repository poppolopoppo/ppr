add_library(engine.shader)

target_sources(engine.shader
  PUBLIC
    FILE_SET CXX_MODULES FILES
      Shader.cppm
  PRIVATE
      Shader.cpp
)

setup_ppr_project(engine.shader
  INTERNAL_PUBLIC_DEPS engine.core
  EXTERNAL_SYSTEM_PRIVATE_DEPS slang
)
