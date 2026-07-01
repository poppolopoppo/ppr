add_library(EngineCore)

set(HAL_PLATFORM_SOURCES
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
    ${PPR_HAL_PLATFORM}/Core.HAL.${PPR_HAL_PLATFORM}.Random.cpp
)

if(PPR_HAL_PLATFORM STREQUAL "windows")
    list(APPEND HAL_PLATFORM_SOURCES
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
            Core.Containers.cppm
            Core.Containers.FlatMap.cppm
            Core.Containers.HashMap.cppm
            Core.Containers.SparseVector.cppm
            Core.Containers.StableVector.cppm
            Core.Enums.cppm
            Core.FunctionRef.cppm
            Core.Hashing.cppm
            Core.InputKey.cppm
            Core.Io.cppm
            Core.Io.FileWatcher.cppm
            Core.Io.MappedFile.cppm
            Core.Utility.cppm
            Core.HAL.cppm
            Core.Logger.cppm
            Core.Memory.cppm
            Core.Memory.Allocator.cppm
            Core.Memory.Arena.cppm
            Core.Memory.PagePool.cppm
            Core.Memory.Pointer.cppm
            Core.Memory.Poison.cppm
            Core.Opaque.cppm
            Core.Service.cppm
            Core.Strings.cppm
            Core.Timer.cppm
            Core.Types.cppm
            Core.UnitTest.cppm

            Core.cppm
    PRIVATE
        Core.Assert.cpp
        Core.Logger.cpp
        Core.Timer.cpp
        Core.UnitTest.cpp
        Core.Memory.Poison.cpp
        Core.Memory.cpp
        Core.Memory.PagePool.cpp
        Core.Memory.Arena.cpp
        Core.Opaque.cpp
        Core.Concurrency.Context.cpp
        Core.Concurrency.Channel.cpp
        Core.Concurrency.Event.cpp
        Core.Io.cpp
        Core.Io.FileWatcher.cpp
        Core.InputKey.cpp
        Core.Containers.StableVector.cpp
        Core.Containers.FlatMap.cpp
        Core.Containers.HashMap.cpp
        ${HAL_PLATFORM_SOURCES}
)

setup_ppr_project(EngineCore
    EXTERNAL_SYSTEM_PRIVATE_DEPS rapidhash glfw
)
