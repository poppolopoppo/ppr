add_library(EngineCore)

set(HAL_PLATFORM_SOURCES
    hal/${PPR_HAL_PLATFORM}/Core.HAL.${PPR_HAL_PLATFORM}.include.hpp
    hal/${PPR_HAL_PLATFORM}/Core.HAL.${PPR_HAL_PLATFORM}.Debugger.cpp
    hal/${PPR_HAL_PLATFORM}/Core.HAL.${PPR_HAL_PLATFORM}.Filesystem.cpp
    hal/${PPR_HAL_PLATFORM}/Core.HAL.${PPR_HAL_PLATFORM}.Io.cpp
    hal/${PPR_HAL_PLATFORM}/Core.HAL.${PPR_HAL_PLATFORM}.IoMap.cpp
    hal/${PPR_HAL_PLATFORM}/Core.HAL.${PPR_HAL_PLATFORM}.IoWatch.cpp
    hal/${PPR_HAL_PLATFORM}/Core.HAL.${PPR_HAL_PLATFORM}.Memory.cpp
    hal/${PPR_HAL_PLATFORM}/Core.HAL.${PPR_HAL_PLATFORM}.Process.cpp
    hal/${PPR_HAL_PLATFORM}/Core.HAL.${PPR_HAL_PLATFORM}.Strings.cpp
    hal/${PPR_HAL_PLATFORM}/Core.HAL.${PPR_HAL_PLATFORM}.System.cpp
    hal/${PPR_HAL_PLATFORM}/Core.HAL.${PPR_HAL_PLATFORM}.Timer.cpp
)

if(PPR_HAL_PLATFORM STREQUAL "windows")
    list(APPEND HAL_PLATFORM_SOURCES
        hal/windows/Core.HAL.windows.Random.cpp
        hal/windows/Core.HAL.windows.RingBuffer.cpp
    )
endif()

target_sources(EngineCore
    PUBLIC
        FILE_SET CXX_MODULES FILES
            Core.Assert.cppm
            concurrency/Core.Concurrency.Channel.cppm
            concurrency/Core.Concurrency.Context.cppm
            concurrency/Core.Concurrency.Event.cppm
            containers/Core.Containers.HashMap.cppm
            containers/Core.Containers.STL.cppm
            containers/Core.Containers.SparseVector.cppm
            containers/Core.Containers.StableVector.cppm
            containers/Core.Containers.cppm
            Core.Enums.cppm
            function/Core.Function.Callback.cppm
            function/Core.Function.Ref.cppm
            hal/Core.HAL.cppm
            Core.Hashing.cppm
            io/Core.Io.FileWatcher.cppm
            io/Core.Io.MappedFile.cppm
            io/Core.Io.cppm
            Core.Logger.cppm
            memory/Core.Memory.Allocator.cppm
            memory/Core.Memory.Arena.cppm
            memory/Core.Memory.PagePool.cppm
            memory/Core.Memory.Pointer.cppm
            memory/Core.Memory.Poison.cppm
            memory/Core.Memory.cppm
            Core.Opaque.cppm
            Core.Service.cppm
            Core.Strings.cppm
            Core.Timer.cppm
            Core.Types.cppm
            Core.UnitTest.cppm
            Core.Utility.cppm
            Core.cppm
    PRIVATE
        Core.Assert.cpp
        concurrency/Core.Concurrency.Channel.cpp
        concurrency/Core.Concurrency.Context.cpp
        concurrency/Core.Concurrency.Event.cpp
        containers/Core.Containers.HashMap.cpp
        containers/Core.Containers.StableVector.cpp
        io/Core.Io.FileWatcher.cpp
        io/Core.Io.MappedFile.cpp
        io/Core.Io.cpp
        Core.Logger.cpp
        memory/Core.Memory.Arena.cpp
        memory/Core.Memory.PagePool.cpp
        memory/Core.Memory.Poison.cpp
        memory/Core.Memory.cpp
        Core.Opaque.cpp
        Core.Service.cpp
        Core.Timer.cpp
        Core.UnitTest.cpp
        ${HAL_PLATFORM_SOURCES}
)

setup_ppr_project(EngineCore
    EXTERNAL_SYSTEM_PRIVATE_DEPS rapidhash
)
