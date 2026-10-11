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
      worldgen/Sim.WorldGen.Noise.cppm
      worldgen/Sim.WorldGen.Plan.cppm
      worldgen/Sim.WorldGen.Layout.cppm
      worldgen/Sim.WorldGen.Geometry.cppm
      worldgen/Sim.WorldGen.Raster.cppm
      worldgen/Sim.WorldGen.Matter.cppm
      worldgen/Sim.WorldGen.Start.cppm
      worldgen/Sim.WorldGen.Result.cppm
  PRIVATE
      chunk_grid/Sim.ChunkGrid.cpp
      ecs/Sim.Ecs.cpp
      tick/Sim.Tick.cpp
      snapshot/Sim.Snapshot.cpp
      worldgen/Sim.WorldGen.Noise.cpp
      worldgen/Sim.WorldGen.Plan.cpp
      worldgen/Sim.WorldGen.Layout.cpp
      worldgen/Sim.WorldGen.Geometry.cpp
      worldgen/Sim.WorldGen.Raster.cpp
      worldgen/Sim.WorldGen.Matter.cpp
      worldgen/Sim.WorldGen.Start.cpp
      worldgen/Sim.WorldGen.Result.cpp
)

setup_ppr_project(engine.sim
  INTERNAL_PUBLIC_DEPS engine.core engine.math
  EXTERNAL_SYSTEM_PRIVATE_DEPS ppr.fast_noise_lite
)
