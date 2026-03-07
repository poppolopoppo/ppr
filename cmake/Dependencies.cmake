# Include CPM
include(CPM)

# Load external dependencies (each in its own modular file)
# NOTE: vcpkg-managed dependencies (fmt, zlib, libdeflate, zstd, lcms, glfw, etc.)
# are installed automatically via vcpkg manifest mode from vcpkg.json
# Only CPM packages below are source-based or not in vcpkg registry
include(external/GLFW)
include(external/Mango)
include(external/rapidhash)
include(external/SlangRHI)
include(external/STB)
