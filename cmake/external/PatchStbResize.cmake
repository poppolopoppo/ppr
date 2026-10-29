# Coefficient packing is in-place and only float-aligned. Keep constant-sized
# copies defined for both overlapping ranges and addresses not aligned to u64.
file(READ "${stb_SOURCE_DIR}/stb_image_resize2.h" _stbir_source)
string(REPLACE "\r\n" "\n" _stbir_source "${_stbir_source}")
string(SHA256 _stbir_hash "${_stbir_source}")
if(NOT _stbir_hash STREQUAL "173e654634f6ccaad98f603e686ea212eec1fe8ea6d2a5e5e8056efa10ae3880")
    message(FATAL_ERROR "stb resize differs from the reviewed source pin")
endif()

# SIMD supplies STBIR_MEMCPY, so upstream's conditional string.h include can be skipped.
string(PREPEND _stbir_source "#include <string.h>\n")

string(REPLACE
    "((stbir_uint32*)(dest))[0] = ((stbir_uint32*)(src))[0];"
    "memmove((dest), (src), 4);"
    _stbir_source "${_stbir_source}")
string(REPLACE
    "((stbir_uint64*)(dest))[0] = ((stbir_uint64*)(src))[0]; ((stbir_uint64*)(dest))[1] = ((stbir_uint64*)(src))[1];"
    "memmove((dest), (src), 16);"
    _stbir_source "${_stbir_source}")
string(REPLACE
    "((stbir_uint64*)(dest))[0] = ((stbir_uint64*)(src))[0];"
    "memmove((dest), (src), 8);"
    _stbir_source "${_stbir_source}")

set(_stbir_include_dir "${CMAKE_CURRENT_BINARY_DIR}/ppr-stb")
file(MAKE_DIRECTORY "${_stbir_include_dir}")
file(WRITE "${_stbir_include_dir}/stb_image_resize2.h.in" "${_stbir_source}")
configure_file("${_stbir_include_dir}/stb_image_resize2.h.in" "${_stbir_include_dir}/stb_image_resize2.h" COPYONLY)
