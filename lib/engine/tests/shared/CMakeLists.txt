add_library(EngineTestsShared STATIC)

target_sources(EngineTestsShared
    PUBLIC
        FILE_SET CXX_MODULES FILES
            Tests.cppm
    PRIVATE
        Tests.cpp
)

setup_ppr_project(EngineTestsShared
    INTERNAL_PUBLIC_DEPS EngineCore
)

target_include_directories(EngineTestsShared PRIVATE ${CMAKE_SOURCE_DIR}/lib/engine/tests/include)
