add_library(engine.matter)

target_sources(engine.matter
  PUBLIC
    FILE_SET CXX_MODULES FILES
      Matter.cppm
      Matter.Gpu.Types.cppm
      Matter.Gpu.Sparse.cppm
      Matter.Gpu.cppm
  PRIVATE
      Matter.Gpu.cpp
      Matter.Gpu.Compact.cpp
      Matter.Gpu.Sparse.cpp
)

setup_ppr_project(engine.matter
  INTERNAL_PUBLIC_DEPS engine.core engine.math engine.shader engine.rhi
)
