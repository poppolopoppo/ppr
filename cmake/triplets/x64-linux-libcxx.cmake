# PPR overlay triplet: plain x64-linux plus libc++.
#
# PPR builds Clang targets with -stdlib=libc++
# (cmake/compiler/Clang.cmake). Prebuilt C++ ports (fmt, simdjson,
# tomlplusplus, ...) must share that standard library: archives built against
# libstdc++ export std::__cxx11 symbols that never resolve against libc++
# objects (std::__1). This triplet keeps the x64-linux layout and adds only
# the stdlib selection, so final links stay single-runtime.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_CMAKE_SYSTEM_NAME Linux)

# GCC does not understand -stdlib=libc++; ports must build with Clang.
# Respect an explicit user override, otherwise default to it.
if(NOT DEFINED ENV{CC})
    set(ENV{CC} "clang")
endif()
if(NOT DEFINED ENV{CXX})
    set(ENV{CXX} "clang++")
endif()

set(VCPKG_CXX_FLAGS "-stdlib=libc++")
set(VCPKG_C_FLAGS "")
set(VCPKG_LINKER_FLAGS "-stdlib=libc++")
