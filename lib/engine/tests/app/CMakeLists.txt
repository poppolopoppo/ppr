add_executable(EngineAppTests main.cpp)

target_sources(EngineAppTests
    PUBLIC
        FILE_SET CXX_MODULES FILES
            App.Tests.cppm
            App.Devices.Tests.cppm
            App.Player.Tests.cppm
            App.Dispatch.Tests.cppm
            App.Snapshot.Tests.cppm
            App.PlayerService.Tests.cppm
)

setup_ppr_project(EngineAppTests
    INTERNAL_PUBLIC_DEPS EngineApp EngineTestsShared
    EXTERNAL_SYSTEM_PRIVATE_DEPS glfw
)

if(MSVC)
    target_compile_options(EngineAppTests PRIVATE /bigobj)
endif()

# MSVC module workaround for __tlregdtor C++ linkage issue with thread_local destructors in modules
if(MSVC AND PPR_ENABLE_SANITIZER_ADDRESS)
    target_link_options(EngineAppTests PRIVATE "/alternatename:?__tlregdtor@@YAHXZ=__tlregdtor")
endif()

add_test(NAME EngineAppUnitTests COMMAND EngineAppTests WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

set_tests_properties(EngineAppUnitTests
    PROPERTIES
        TIMEOUT 120
        LABELS "unit;engine;app"
)
