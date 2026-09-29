find_package(box2d CONFIG REQUIRED)

add_library(engine.physics)

target_sources(engine.physics
  PUBLIC
    FILE_SET CXX_MODULES FILES
      Physics.cppm
      Physics.Scene.cppm
      Physics.ChunkColliders.cppm
      Physics.Coupling.cppm
  PRIVATE
      Physics.Scene.cpp
      Physics.ChunkColliders.cpp
      Physics.Coupling.cpp
)

setup_ppr_project(engine.physics
  INTERNAL_PUBLIC_DEPS engine.core engine.math
  EXTERNAL_SYSTEM_PRIVATE_DEPS box2d::box2d
)
