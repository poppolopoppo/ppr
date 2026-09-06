add_library(engine.tests STATIC)

target_sources(engine.tests
    PUBLIC
        FILE_SET CXX_MODULES FILES
            Tests.cppm
    PRIVATE
        Tests.cpp
)

setup_ppr_project(engine.tests
    INTERNAL_PUBLIC_DEPS engine.core
)

target_include_directories(engine.tests PRIVATE ${CMAKE_SOURCE_DIR}/lib/engine/tests/include)
