add_library(engine.asset)

target_sources(engine.asset
    PUBLIC
        FILE_SET CXX_MODULES FILES
            Asset.cppm
            Asset.Pack.cppm
            Asset.Preparation.cppm
    PRIVATE
        Asset.Pack.cpp
        Asset.Preparation.cpp
)

setup_ppr_project(engine.asset
    INTERNAL_PUBLIC_DEPS engine.core
)
