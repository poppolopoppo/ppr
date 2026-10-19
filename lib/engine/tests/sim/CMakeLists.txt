add_executable(engine.tests.sim main.cpp)

target_sources(engine.tests.sim
    PUBLIC
        FILE_SET CXX_MODULES FILES
            Sim.Tests.cppm
    )

target_sources(engine.tests.sim
    PRIVATE
        Sim.Tests.cpp
        Sim.ChunkGrid.Tests.cpp
        Sim.Ecs.Tests.cpp
        Sim.Tick.Tests.cpp
        Sim.Snapshot.Tests.cpp
        Sim.Physics.Tests.cpp
        Sim.WorldGen.Tests.cpp
        Sim.Asset.Bindings.Tests.cpp
    )

setup_ppr_project(engine.tests.sim
    INTERNAL_PUBLIC_DEPS engine.core engine.math engine.sim engine.tests
)

target_link_libraries(engine.tests.sim PRIVATE engine.physics)

target_include_directories(engine.tests.sim PRIVATE ${CMAKE_SOURCE_DIR}/lib/engine/tests/include)

# MSVC module workaround for __tlregdtor C++ linkage issue with thread_local destructors in modules
if(MSVC AND PPR_ENABLE_SANITIZER_ADDRESS)
    target_link_options(engine.tests.sim PRIVATE "/alternatename:?__tlregdtor@@YAHXZ=__tlregdtor")
endif()

# World-gen runs several full-grid (4096x4096) generate passes per suite run;
# the debug build (Od+RTC+ASan) measures ~69s for the suite, so this timeout
# keeps genuine margin over that reality instead of sitting near it.
ppr_add_test_entry(TARGET engine.tests.sim
    LOOPS
    TIMEOUT 300
    LABELS unit engine sim
)
