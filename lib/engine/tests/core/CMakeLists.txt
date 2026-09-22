add_executable(engine.tests.core main.cpp)

target_sources(engine.tests.core
        PUBLIC
        FILE_SET CXX_MODULES FILES
        Core.Tests.cppm
)

target_sources(engine.tests.core
        PRIVATE
        Core.Tests.cpp
        Core.Memory.PagePool.Tests.cpp
        Core.Memory.Arena.Tests.cpp
        Core.Memory.Slab.Tests.cpp
        Core.Allocator.Tests.cpp
        Core.Buffer.Tests.cpp
        Core.SafePtr.Tests.cpp
        Core.Poisoning.Tests.cpp
        Core.Containers.Core.Tests.cpp
        Core.Containers.FlatMap.Tests.cpp
        Core.Containers.HashMap.Tests.cpp
        Core.Containers.StableVector.Tests.cpp
        Core.Containers.SparseVector.Tests.cpp
        Core.Concurrency.Channel.Tests.cpp
        Core.Concurrency.Event.Tests.cpp
        Core.Concurrency.Context.Tests.cpp
        Core.Io.Tests.cpp
        Core.Io.FileWatcher.Tests.cpp
        Core.HAL.Tests.cpp
        Core.Enums.Tests.cpp
        Core.Math.Tests.cpp
        Core.Strings.Tests.cpp
        Core.Utility.Tests.cpp
        Core.Opaque.Tests.cpp
        Core.Service.Tests.cpp
)

setup_ppr_project(engine.tests.core
        INTERNAL_PUBLIC_DEPS
        engine.core
        engine.math
        engine.shader
        engine.rhi
        engine.tests
)

target_include_directories(engine.tests.core PRIVATE ${CMAKE_SOURCE_DIR}/lib/engine/tests/include)

# MSVC module workaround for __tlregdtor C++ linkage issue with thread_local destructors in modules
if (MSVC AND PPR_ENABLE_SANITIZER_ADDRESS)
    # When using C++23 modules with /MT, the compiler generates C++ mangled references
    # to __tlregdtor (for thread_local destructors), but libcmt.lib provides it as C linkage.
    # This provides the C++ mangled version that delegates to the C version.
    message(AUTHOR_WARNING "engine.tests.core: enable temporary workaround for thread_local linker issue with modules and ASAN")
    #    target_sources(engine.core PRIVATE
    #        ${PPR_HAL_PLATFORM}/MSVC.tlregdtor.cpp
    #    )
    target_link_options(engine.tests.core PRIVATE "/alternatename:?__tlregdtor@@YAHXZ=__tlregdtor")
endif ()

add_test(NAME EngineCoreUnitTests COMMAND engine.tests.core WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

set_tests_properties(EngineCoreUnitTests
        PROPERTIES
        TIMEOUT 120
        LABELS "unit;engine;core"
)
