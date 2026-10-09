# toml++ - TOML parser backing the config file codec (private dependency).
# vcpkg manifest mode provides the tomlplusplus config package when a vcpkg
# toolchain is active; this CPM package is the pinned fallback for builds
# without vcpkg (or when the config package is otherwise undiscoverable).

find_package(tomlplusplus CONFIG QUIET)

if(NOT tomlplusplus_FOUND AND NOT TARGET tomlplusplus::tomlplusplus)
    CPMAddPackage(
        NAME tomlplusplus
        GITHUB_REPOSITORY marzer/tomlplusplus
        GIT_TAG v3.4.0
        OPTIONS
            # Header-only codec use; never build upstream extras. Unknown
            # option names are inert cache entries if upstream renames them.
            "TOMLPP_BUILD_EXAMPLES OFF"
            "TOMLPP_BUILD_TESTS OFF"
    )
endif()

# Normalize the imported target name when the CPM sub-build uses its
# non-namespaced spelling.
if(NOT TARGET tomlplusplus::tomlplusplus AND TARGET tomlplusplus_tomlplusplus)
    add_library(tomlplusplus::tomlplusplus ALIAS tomlplusplus_tomlplusplus)
endif()
