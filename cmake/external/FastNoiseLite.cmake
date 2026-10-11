# FastNoiseLite v1.1.1, resolved from upstream refs/tags/v1.1.1 on 2026-10-11.
# Immutable source pin; licenses/provenance: third_party/FastNoiseLite/.
CPMAddPackage(
    NAME FastNoiseLite
    GITHUB_REPOSITORY Auburn/FastNoiseLite
    GIT_TAG 7ccfbc16eb1c932568f177d63a9ba51d89bbe516
    DOWNLOAD_ONLY YES
)

include("${CMAKE_CURRENT_LIST_DIR}/PatchFastNoiseLite.cmake")

if(NOT TARGET ppr.fast_noise_lite)
    add_library(ppr.fast_noise_lite INTERFACE IMPORTED GLOBAL)
    target_include_directories(ppr.fast_noise_lite SYSTEM INTERFACE "${_fnl_include_dir}")
endif()
