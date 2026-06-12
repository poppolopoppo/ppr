add_library(EngineCore)

target_sources(EngineCore
    PUBLIC
        FILE_SET CXX_MODULES FILES
            Core.Assert.cppm
            Core.Concurrency.Channel.cppm
            Core.Concurrency.Context.cppm
            Core.Concurrency.Event.cppm
            Core.Containers.cppm
            Core.Containers.HashMap.cppm
            Core.Containers.SparseVector.cppm
            Core.Containers.StableVector.cppm
            Core.Enums.cppm
            Core.FunctionRef.cppm
            Core.HAL.cppm
            Core.Logger.cppm
            Core.Memory.cppm
            Core.Memory.Allocator.cppm
            Core.Memory.Arena.cppm
            Core.Memory.PagePool.cppm
            Core.Memory.Poison.cppm
            Core.Opaque.cppm
            Core.Strings.cppm
            Core.Timer.cppm
            Core.Types.cppm
            Core.UnitTest.cppm

            Core.cppm
    PRIVATE
        Core.Logger.cpp
        Core.Timer.cpp
        Core.Memory.Poison.cpp
        Core.Memory.cpp
        Core.Memory.PagePool.cpp
        Core.Memory.Arena.cpp
        Core.Opaque.cpp
        ${PPR_HAL_PLATFORM}/Core.HAL.${PPR_HAL_PLATFORM}.cpp
)

setup_ppr_project(EngineCore
    EXTERNAL_SYSTEM_PRIVATE_DEPS rapidhash
)
