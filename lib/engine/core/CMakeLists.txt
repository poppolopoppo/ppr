add_library(EngineCore)

set(HAL_PLATFORM_SOURCES
    ${PPR_HAL_PLATFORM}/Core.HAL.${PPR_HAL_PLATFORM}.include.hpp
    ${PPR_HAL_PLATFORM}/Core.HAL.${PPR_HAL_PLATFORM}.Debugger.cpp
    ${PPR_HAL_PLATFORM}/Core.HAL.${PPR_HAL_PLATFORM}.Filesystem.cpp
    ${PPR_HAL_PLATFORM}/Core.HAL.${PPR_HAL_PLATFORM}.Io.cpp
    ${PPR_HAL_PLATFORM}/Core.HAL.${PPR_HAL_PLATFORM}.IoMap.cpp
    ${PPR_HAL_PLATFORM}/Core.HAL.${PPR_HAL_PLATFORM}.IoWatch.cpp
    ${PPR_HAL_PLATFORM}/Core.HAL.${PPR_HAL_PLATFORM}.Memory.cpp
    ${PPR_HAL_PLATFORM}/Core.HAL.${PPR_HAL_PLATFORM}.Process.cpp
    ${PPR_HAL_PLATFORM}/Core.HAL.${PPR_HAL_PLATFORM}.Strings.cpp
    ${PPR_HAL_PLATFORM}/Core.HAL.${PPR_HAL_PLATFORM}.System.cpp
    ${PPR_HAL_PLATFORM}/Core.HAL.${PPR_HAL_PLATFORM}.Timer.cpp
)

if(PPR_HAL_PLATFORM STREQUAL "windows")
    list(APPEND HAL_PLATFORM_SOURCES
        windows/Core.HAL.windows.Random.cpp
        windows/Core.HAL.windows.RingBuffer.cpp
    )
endif()

target_sources(EngineCore
    PUBLIC
        FILE_SET CXX_MODULES FILES
            Core.Assert.cppm
            Core.Concurrency.Channel.cppm
            Core.Concurrency.Context.cppm
            Core.Concurrency.Event.cppm
            Core.Containers.HashMap.cppm
            Core.Containers.STL.cppm
            Core.Containers.SparseVector.cppm
            Core.Containers.StableVector.cppm
            Core.Containers.cppm
            Core.Enums.cppm
            Core.Function.Callback.cppm
            Core.Function.Ref.cppm
            Core.HAL.cppm
            Core.Hashing.cppm
            Core.Io.FileWatcher.cppm
            Core.Io.MappedFile.cppm
            Core.Io.cppm
            Core.Logger.cppm
            Core.Memory.Allocator.cppm
            Core.Memory.Arena.cppm
            Core.Memory.PagePool.cppm
            Core.Memory.Pointer.cppm
            Core.Memory.Poison.cppm
            Core.Memory.cppm
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
        Core.Concurrency.Channel.cpp
        Core.Concurrency.Context.cpp
        Core.Concurrency.Event.cpp
        Core.Containers.HashMap.cpp
        Core.Containers.StableVector.cpp
        Core.Io.FileWatcher.cpp
        Core.Io.MappedFile.cpp
        Core.Io.cpp
        Core.Logger.cpp
        Core.Memory.Arena.cpp
        Core.Memory.PagePool.cpp
        Core.Memory.Poison.cpp
        Core.Memory.cpp
        Core.Opaque.cpp
        Core.Service.cpp
        Core.Timer.cpp
        Core.UnitTest.cpp
        ${HAL_PLATFORM_SOURCES}
)

setup_ppr_project(EngineCore
    EXTERNAL_SYSTEM_PRIVATE_DEPS rapidhash
)
