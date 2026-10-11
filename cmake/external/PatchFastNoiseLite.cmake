# Make the pinned upstream's intended modulo-32-bit integer arithmetic defined
# on every supported compiler. Generate a private copy; never mutate CPM's cache.
file(READ "${FastNoiseLite_SOURCE_DIR}/Cpp/FastNoiseLite.h" _fnl_source)
string(REPLACE "\r\n" "\n" _fnl_source "${_fnl_source}")
string(SHA256 _fnl_hash "${_fnl_source}")
if(NOT _fnl_hash STREQUAL "47a29750dfe13d15651febd235f3c26b1cc57af7c6e26583b838fd7ab5215dc2")
    message(FATAL_ERROR "FastNoiseLite source differs from the reviewed v1.1.1 header")
endif()

function(ppr_patch_fnl old new expected_count)
    string(LENGTH "${_fnl_source}" before_length)
    string(REPLACE "${old}" "" removed "${_fnl_source}")
    string(LENGTH "${removed}" after_length)
    string(LENGTH "${old}" match_length)
    math(EXPR count "(${before_length} - ${after_length}) / ${match_length}")
    if(NOT count EQUAL expected_count)
        message(FATAL_ERROR "FastNoiseLite patch mismatch: ${old} (${count} != ${expected_count})")
    endif()
    string(REPLACE "${old}" "${new}" patched "${_fnl_source}")
    set(_fnl_source "${patched}" PARENT_SCOPE)
endfunction()

# Keep signed hash storage/right shifts: they select the original gradients.
# Unsigned operands define multiplication, priming and neighbour wraparound;
# conversion back to int is congruent modulo 2^32 under C++20 and later.
ppr_patch_fnl("static const int Prime" "static const unsigned int Prime" 3)
ppr_patch_fnl("hash *= 0x27d4eb2d;" "hash *= 0x27d4eb2du;" 2)
ppr_patch_fnl("hash *= hash;" "hash *= static_cast<unsigned int>(hash);" 2)
ppr_patch_fnl("hash ^= hash << 19;" "hash ^= static_cast<unsigned int>(hash) << 19;" 2)
ppr_patch_fnl("int seed = mSeed;" "unsigned int seed = static_cast<unsigned int>(mSeed);" 12)
ppr_patch_fnl("seed + 1293373;" "seed + 1293373u;" 1)
ppr_patch_fnl("seed += 1293373;" "seed += 1293373u;" 1)
foreach(axis X Y Z)
    if(axis STREQUAL "Z")
        set(count 1)
    else()
        set(count 2)
    endif()
    ppr_patch_fnl("(int)((long)Prime${axis} << 1)" "(Prime${axis} << 1)" ${count})
endforeach()

set(_fnl_include_dir "${CMAKE_CURRENT_BINARY_DIR}/ppr-fast-noise-lite")
file(MAKE_DIRECTORY "${_fnl_include_dir}")
# configure_file COPYONLY preserves timestamps when this content is unchanged.
file(WRITE "${_fnl_include_dir}/FastNoiseLite.h.in" "${_fnl_source}")
configure_file("${_fnl_include_dir}/FastNoiseLite.h.in" "${_fnl_include_dir}/FastNoiseLite.h" COPYONLY)
