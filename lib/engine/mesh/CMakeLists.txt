add_library(engine.mesh)

target_sources(engine.mesh
  PUBLIC
    FILE_SET CXX_MODULES FILES
      Mesh.cppm
      Mesh.Types.cppm
      Mesh.Convert.cppm
  PRIVATE
      Mesh.Convert.cpp
)

setup_ppr_project(engine.mesh
  INTERNAL_PUBLIC_DEPS engine.core engine.math
  EXTERNAL_SYSTEM_PRIVATE_DEPS mango-import3d
)
