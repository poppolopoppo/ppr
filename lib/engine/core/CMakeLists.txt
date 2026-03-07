add_library(EngineCore)

target_sources(EngineCore
    PUBLIC
        FILE_SET CXX_MODULES FILES
            Core.Assert.cppm
            Core.Containers.cppm
            Core.Containers.HashMap.cppm
            Core.Containers.SparseVector.cppm
            Core.Containers.StableVector.cppm
            Core.Enums.cppm
            Core.FunctionRef.cppm
            Core.HAL.cppm
            Core.Memory.cppm
            Core.Memory.Arena.cppm
            Core.Memory.PagePool.cppm
            Core.Opaque.cppm
            Core.Strings.cppm

            Core.cppm
    PRIVATE
        ${PPR_HAL_PLATFORM}/Core.HAL.${PPR_HAL_PLATFORM}.cpp
)

setup_ppr_project(EngineCore
    EXTERNAL_SYSTEM_PRIVATE_DEPS rapidhash
)
