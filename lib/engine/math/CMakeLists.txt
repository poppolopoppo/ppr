add_library(engine.math)

target_sources(engine.math
  PUBLIC
    FILE_SET CXX_MODULES FILES
      Math.cppm
)

setup_ppr_project(engine.math
  INTERNAL_PUBLIC_DEPS engine.core
  EXTERNAL_SYSTEM_PRIVATE_DEPS mango
)
