# FreeType — font rasterizer backend for the vendored ImGui FreeType wrapper.
#
# Included ONLY when PPR_IMGUI_FREETYPE is ON (see cmake/Dependencies.cmake),
# which is only ever set by the opt-in preset msvc-dev-freetype. The default
# configuration neither installs nor links this dependency.
#
# Version: vcpkg manifest feature `imgui-freetype` -> port `freetype` 2.13.3
# (port-version 0), resolved from the repo baseline in vcpkg.json. The port is
# a CONFIG package exposing `Freetype::Freetype`; the port layout ships
# `include/ft2build.h` + `include/freetype/`, which is the include shape
# `imgui_freetype.cpp` expects.
#
# License: FTL (FreeType Project License) or GPL-2.0-or-later, per the port's
# vcpkg.json; PPR selects the FTL. Notice text and the license file are
# retained in third_party/FreeType/ (see its README.md for pin, provenance and
# the binary-distribution disclaimer requirement).

find_package(Freetype CONFIG REQUIRED)

# Mark FreeType includes as SYSTEM to suppress warnings from external headers
# (same treatment as cmake/external/GLFW.cmake and the Mango targets).
get_target_property(freetype_inc Freetype::Freetype INTERFACE_INCLUDE_DIRECTORIES)
if(freetype_inc)
    set_target_properties(Freetype::Freetype PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${freetype_inc}")
endif()

set_target_properties(Freetype::Freetype PROPERTIES CXX_MODULE_STD OFF)
