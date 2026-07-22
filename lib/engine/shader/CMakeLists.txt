add_library(EngineShader)

target_sources(EngineShader
  PUBLIC
    FILE_SET CXX_MODULES FILES
      Shader.cppm
  PRIVATE
      Shader.cpp
)

setup_ppr_project(EngineShader
  INTERNAL_PUBLIC_DEPS EngineCore
  EXTERNAL_SYSTEM_PRIVATE_DEPS slang
)
