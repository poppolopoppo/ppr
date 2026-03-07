# rapidhash - Very fast, high quality, platform-independent
# Family of three hash functions: rapidhash, rapidhashMicro and rapidhashNano
# Used by Chromium, Folly's F14, Fuchsia, Ninja, JuliaLang, ziglang, fb303, zxc, among others

CPMAddPackage(
        NAME rapidhash
        GITHUB_REPOSITORY Nicoshev/rapidhash
        GIT_TAG master
)

# Create interface target for stb
if(NOT TARGET rapidhash)
    add_library(rapidhash INTERFACE IMPORTED GLOBAL)
    target_include_directories(rapidhash SYSTEM INTERFACE ${rapidhash_SOURCE_DIR})
endif()
