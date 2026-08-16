add_executable(EngineCoreTests main.cpp)

target_sources(EngineCoreTests
    PUBLIC
        FILE_SET CXX_MODULES FILES
            Core.Opaque.Tests.cppm
            Core.Service.Tests.cppm
            Core.Enums.Tests.cppm
            Core.HAL.Tests.cppm
            Core.Memory.Tests.cppm
            Core.Memory.Arena.Tests.cppm
            Core.Memory.PagePool.Tests.cppm
            Core.Containers.Tests.cppm
            Core.Containers.FlatMap.Tests.cppm
            Core.Containers.StableVector.Tests.cppm
            Core.Containers.SparseVector.Tests.cppm
            Core.Containers.HashMap.Tests.cppm
            Core.Concurrency.Channel.Tests.cppm
            Core.Concurrency.Context.Tests.cppm
            Core.Concurrency.Event.Tests.cppm
            Core.Io.Tests.cppm
            Core.Io.FileWatcher.Tests.cppm
            Core.Strings.Tests.cppm
            Core.Utility.Tests.cppm
            Core.Tests.cppm
)

setup_ppr_project(EngineCoreTests
  INTERNAL_PUBLIC_DEPS EngineCore EngineShader EngineRHI EngineTestsShared
)

target_include_directories(EngineCoreTests PRIVATE ${CMAKE_SOURCE_DIR}/lib/engine/tests/include)

# MSVC module workaround for __tlregdtor C++ linkage issue with thread_local destructors in modules
if(MSVC AND PPR_ENABLE_SANITIZER_ADDRESS)
    # When using C++23 modules with /MT, the compiler generates C++ mangled references
    # to __tlregdtor (for thread_local destructors), but libcmt.lib provides it as C linkage.
    # This provides the C++ mangled version that delegates to the C version.
    message(AUTHOR_WARNING "EngineCoreTests: enable temporary workaround for thread_local linker issue with modules and ASAN")
    #    target_sources(EngineCore PRIVATE
    #        ${PPR_HAL_PLATFORM}/MSVC.tlregdtor.cpp
    #    )
    target_link_options(EngineCoreTests PRIVATE "/alternatename:?__tlregdtor@@YAHXZ=__tlregdtor")
endif()

add_test(NAME EngineCoreUnitTests COMMAND EngineCoreTests WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

set_tests_properties(EngineCoreUnitTests
    PROPERTIES
        TIMEOUT 120
        LABELS "unit;engine;core"
)
