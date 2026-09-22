add_library(engine.image)

target_sources(engine.image
  PUBLIC
    FILE_SET CXX_MODULES FILES
      Image.cppm
      Image.Types.cppm
      Image.Decode.cppm
  PRIVATE
      Image.Types.cpp
      Image.Decode.cpp
)

setup_ppr_project(engine.image
  INTERNAL_PUBLIC_DEPS engine.core engine.math
  EXTERNAL_SYSTEM_PRIVATE_DEPS mango-image
)
