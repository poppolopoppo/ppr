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

# MSVC module workaround for __tlregdtor C++ linkage issue with thread_local destructors in modules
if(MSVC AND PPR_ENABLE_SANITIZER_ADDRESS)
    # When using C++23 modules with /MT, the compiler generates C++ mangled references
    # to __tlregdtor (for thread_local destructors), but libcmt.lib provides it as C linkage.
    # This provides the C++ mangled version that delegates to the C version.
    message(AUTHOR_WARNING "EngineTests: enable temporary workaround for thread_local linker issue with modules and ASAN")
    #    target_sources(EngineCore PRIVATE
    #        ${PPR_HAL_PLATFORM}/MSVC.tlregdtor.cpp
    #    )
    target_link_options(EngineTests PRIVATE "/alternatename:?__tlregdtor@@YAHXZ=__tlregdtor")
endif()


add_test(NAME EngineUnitTests COMMAND EngineTests WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

set_tests_properties(EngineUnitTests
    PROPERTIES
        TIMEOUT 120
        LABELS "unit;engine"
)