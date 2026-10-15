add_library(engine.asset)

target_sources(engine.asset
    PUBLIC
        FILE_SET CXX_MODULES FILES
            Asset.cppm
            Asset.Pack.cppm
    PRIVATE
        Asset.Pack.cpp
)

setup_ppr_project(engine.asset
    INTERNAL_PUBLIC_DEPS engine.core
)
