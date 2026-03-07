add_executable(EngineTests main.cpp)

target_sources(EngineTests
    PUBLIC
        FILE_SET CXX_MODULES FILES
            core/Core.Opaque.Tests.cppm
            core/Core.Enums.Tests.cppm
            core/Core.Memory.Tests.cppm
            core/Core.Memory.Arena.Tests.cppm
            core/Core.Memory.PagePool.Tests.cppm
            core/Core.Containers.Tests.cppm
            core/Core.Containers.StableVector.Tests.cppm
            core/Core.Containers.SparseVector.Tests.cppm
            core/Core.Containers.HashMap.Tests.cppm
            core/Core.Strings.Tests.cppm
            core/Core.Tests.cppm
)

setup_ppr_project(EngineTests
  INTERNAL_PUBLIC_DEPS EngineCore
)

add_test(NAME EngineUnitTests COMMAND EngineTests WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

set_tests_properties(EngineUnitTests
    PROPERTIES
        TIMEOUT 120
        LABELS "unit;engine"
)