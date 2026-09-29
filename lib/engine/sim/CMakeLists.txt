add_library(engine.sim)

target_sources(engine.sim
  PUBLIC
    FILE_SET CXX_MODULES FILES
      Sim.cppm
      chunk_grid/Sim.ChunkGrid.cppm
      ecs/Sim.Ecs.cppm
      footprint/Sim.Footprint.cppm
      tick/Sim.Tick.cppm
      snapshot/Sim.Snapshot.cppm
      worldgen/Sim.WorldGen.cppm
  PRIVATE
      chunk_grid/Sim.ChunkGrid.cpp
      ecs/Sim.Ecs.cpp
      tick/Sim.Tick.cpp
      snapshot/Sim.Snapshot.cpp
      worldgen/Sim.WorldGen.cpp
)

setup_ppr_project(engine.sim
  INTERNAL_PUBLIC_DEPS engine.core engine.math
)
