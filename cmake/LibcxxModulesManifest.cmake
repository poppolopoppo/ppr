# ------------------------------------------------------------------
# libc++ modules manifest preselection
# ------------------------------------------------------------------
# CMake's `import std` support asks the compiler for `libstdc++.modules.json`
# while `project()` runs, whenever CMAKE_CXX_STDLIB_MODULES_JSON is still unset.
# GCC 16 ships that manifest, so a fresh configure of this libc++ build
# (cmake/compiler/Clang.cmake applies -stdlib=libc++) recorded libstdc++'s
# module source, and every dependent module scan then failed with
# "'bits/stdc++.h' file not found" because the libstdc++ headers are not on the
# libc++ include path. Selecting the libc++ manifest here, before `project()`,
# makes CMake skip that probe.
#
# The file is included from root CMakeLists.txt before `project()` and again
# from cmake/compiler/Clang.cmake after it; the second pass only re-asserts the
# selection, so the scan and its status message appear once per configure.
#
# Two variables carry the selection, and the split is the contract:
#   CMAKE_CXX_STDLIB_MODULES_JSON  user intent. Only a cache entry (from `-D`,
#     a preset, or CMakeCache.txt) counts; this file never cache-writes that
#     variable, so a stale automatic selection cannot masquerade as an explicit
#     choice.
#   PPR_LIBCXX_MODULES_JSON  the automatic selection recorded by this file. It
#     is a stability hint for the next configure, always re-validated before
#     use, and never treated as user intent.
# Precedence: a resolving CMAKE_CXX_STDLIB_MODULES_JSON override, then the
# recorded automatic selection, then the compiler/llvm-config/resource-dir
# hints, then the well-known fallback paths.
if(PPR_LIBCXX_MANIFEST_DISCOVERED)
    # Post-`project()` pass: re-assert the pre-`project()` selection without
    # repeating the scan or its status message.
    if(NOT CMAKE_CXX_STDLIB_MODULES_JSON AND PPR_LIBCXX_MODULES_JSON)
        set(CMAKE_CXX_STDLIB_MODULES_JSON "${PPR_LIBCXX_MODULES_JSON}")
    endif()
    return()
endif()

# Only a Clang/GNU-style driver consumes a libc++ manifest. Before `project()`
# the compiler ID and frontend variant are not detected yet, so recognize a
# non-MSVC driver by name; `cl` and `clang-cl` bring their own standard library.
# When no compiler is selected yet, detection is deferred to the post-project
# include, which then sees the frontend variant.
set(PPR_LIBCXX_FRONTEND_ELIGIBLE FALSE)
if(DEFINED CMAKE_CXX_COMPILER_FRONTEND_VARIANT)
    if(NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        set(PPR_LIBCXX_FRONTEND_ELIGIBLE TRUE)
    endif()
elseif(CMAKE_CXX_COMPILER)
    get_filename_component(_ppr_libcxx_compiler_name "${CMAKE_CXX_COMPILER}" NAME_WE)
    if(_ppr_libcxx_compiler_name MATCHES "^(clang\\+\\+|clang|g\\+\\+|gcc|cc|c\\+\\+)(-[0-9.]+)?$")
        set(PPR_LIBCXX_FRONTEND_ELIGIBLE TRUE)
    endif()
    unset(_ppr_libcxx_compiler_name)
endif()

if(NOT PPR_LIBCXX_FRONTEND_ELIGIBLE)
    unset(PPR_LIBCXX_FRONTEND_ELIGIBLE)
    return()
endif()
unset(PPR_LIBCXX_FRONTEND_ELIGIBLE)
set(PPR_LIBCXX_MANIFEST_DISCOVERED TRUE)

# A manifest declares its module sources relative to its own directory, and a
# distribution may ship more than one copy whose layout only resolves from a
# single installation prefix. Accept a candidate only when every declared
# source path exists, so a stale duplicate cannot win on directory order alone.
# `string(JSON)` reports success by setting its ERROR_VARIABLE to NOTFOUND, so
# that value (or an empty one) means the lookup succeeded.
function(ppr_libcxx_manifest_resolves manifest result_var)
    set(_resolves FALSE)

    file(READ "${manifest}" _manifest ERROR_VARIABLE _read_error)
    if(NOT _read_error)
        get_filename_component(_manifest_dir "${manifest}" DIRECTORY)

        string(JSON _module_count ERROR_VARIABLE _json_error LENGTH "${_manifest}" modules)
        if(_json_error AND NOT _json_error STREQUAL "NOTFOUND")
            set(_module_count 0)
        endif()

        if(_module_count GREATER 0)
            set(_resolves TRUE)

            set(_index 0)
            while(_index LESS _module_count)
                string(JSON _source ERROR_VARIABLE _json_error GET "${_manifest}" modules ${_index} source-path)
                if(_json_error AND NOT _json_error STREQUAL "NOTFOUND")
                    set(_resolves FALSE)
                    break()
                endif()

                if(NOT IS_ABSOLUTE "${_source}")
                    get_filename_component(_source "${_manifest_dir}/${_source}" ABSOLUTE)
                endif()
                if(NOT EXISTS "${_source}")
                    set(_resolves FALSE)
                    break()
                endif()

                math(EXPR _index "${_index} + 1")
            endwhile()
        endif()
    endif()

    set(${result_var} "${_resolves}" PARENT_SCOPE)
endfunction()

# An explicit cache entry is user intent: it wins while the manifest it names
# resolves. A non-resolving override is reported and dropped instead of being
# kept as a broken path, and discovery then decides.
set(PPR_LIBCXX_SELECTED_MANIFEST "")
if(DEFINED CACHE{CMAKE_CXX_STDLIB_MODULES_JSON})
    set(_ppr_libcxx_override "${CMAKE_CXX_STDLIB_MODULES_JSON}")

    if(_ppr_libcxx_override)
        ppr_libcxx_manifest_resolves("${_ppr_libcxx_override}" _ppr_libcxx_override_resolves)
        if(_ppr_libcxx_override_resolves)
            set(PPR_LIBCXX_SELECTED_MANIFEST "${_ppr_libcxx_override}")
        else()
            message(WARNING "Ignoring CMAKE_CXX_STDLIB_MODULES_JSON='${_ppr_libcxx_override}': the file "
                    "is missing or declares a module source that does not exist; falling back to "
                    "libc++ manifest discovery.")
            unset(CMAKE_CXX_STDLIB_MODULES_JSON CACHE)
            unset(CMAKE_CXX_STDLIB_MODULES_JSON)
        endif()
        unset(_ppr_libcxx_override_resolves)
    endif()

    unset(_ppr_libcxx_override)
endif()

# Locate the libc++ modules manifest without pinning an LLVM major version.
# Ask the compiler for its own manifest first, then probe llvm-config,
# LLVM_DIR, and the compiler resource dir, then fall back to well-known
# versioned/multiarch locations. Only set the variable when a candidate
# resolves so newer/older toolchains keep working.
set(PPR_LIBCXX_COMPILER_MANIFEST)
if(CMAKE_CXX_COMPILER)
    execute_process(
        COMMAND "${CMAKE_CXX_COMPILER}" -print-file-name=libc++.modules.json
        OUTPUT_VARIABLE PPR_LIBCXX_COMPILER_MANIFEST
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()

set(PPR_LIBCXX_MODULES_JSON_HINTS)
find_program(PPR_LLVM_CONFIG NAMES llvm-config)
if(PPR_LLVM_CONFIG)
    execute_process(
        COMMAND "${PPR_LLVM_CONFIG}" --libdir
        OUTPUT_VARIABLE PPR_LLVM_LIBDIR
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(PPR_LLVM_LIBDIR)
        list(APPEND PPR_LIBCXX_MODULES_JSON_HINTS "${PPR_LLVM_LIBDIR}")
    endif()
    unset(PPR_LLVM_LIBDIR)
endif()
if(DEFINED ENV{LLVM_DIR})
    list(APPEND PPR_LIBCXX_MODULES_JSON_HINTS "$ENV{LLVM_DIR}" "$ENV{LLVM_DIR}/lib")
endif()
if(CMAKE_CXX_COMPILER)
    execute_process(
        COMMAND "${CMAKE_CXX_COMPILER}" --print-resource-dir
        OUTPUT_VARIABLE PPR_CLANG_RESOURCE_DIR
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(PPR_CLANG_RESOURCE_DIR)
        get_filename_component(PPR_CLANG_LIBDIR "${PPR_CLANG_RESOURCE_DIR}/../.." ABSOLUTE)
        list(APPEND PPR_LIBCXX_MODULES_JSON_HINTS "${PPR_CLANG_LIBDIR}")
        unset(PPR_CLANG_LIBDIR)
    endif()
    unset(PPR_CLANG_RESOURCE_DIR)
endif()

# The recorded automatic selection is tried first so a working choice stays
# stable, but it is only kept when it still resolves.
set(PPR_LIBCXX_MANIFEST_CANDIDATES)
if(PPR_LIBCXX_MODULES_JSON)
    list(APPEND PPR_LIBCXX_MANIFEST_CANDIDATES "${PPR_LIBCXX_MODULES_JSON}")
endif()
if(PPR_LIBCXX_COMPILER_MANIFEST AND IS_ABSOLUTE "${PPR_LIBCXX_COMPILER_MANIFEST}")
    list(APPEND PPR_LIBCXX_MANIFEST_CANDIDATES "${PPR_LIBCXX_COMPILER_MANIFEST}")
endif()
foreach(_hint IN LISTS PPR_LIBCXX_MODULES_JSON_HINTS)
    list(APPEND PPR_LIBCXX_MANIFEST_CANDIDATES "${_hint}/libc++.modules.json")
endforeach()
list(APPEND PPR_LIBCXX_MANIFEST_CANDIDATES
    /usr/lib/llvm-22/lib/libc++.modules.json
    /usr/lib/llvm-20/lib/libc++.modules.json
    /usr/lib/x86_64-linux-gnu/libc++.modules.json
    /usr/lib/llvm/lib/libc++.modules.json
)

set(PPR_LIBCXX_MODULES_JSON "")
foreach(_candidate IN LISTS PPR_LIBCXX_MANIFEST_CANDIDATES)
    if(EXISTS "${_candidate}")
        ppr_libcxx_manifest_resolves("${_candidate}" _candidate_resolves)
        if(_candidate_resolves)
            set(PPR_LIBCXX_MODULES_JSON "${_candidate}")
            break()
        endif()
    endif()
endforeach()

# The automatic scan result is recorded only in PPR_LIBCXX_MODULES_JSON, never
# as a CMAKE_CXX_STDLIB_MODULES_JSON cache entry, which is reserved for user
# intent. Recording it keeps the next configure from probing again.
if(PPR_LIBCXX_MODULES_JSON)
    get_filename_component(PPR_LIBCXX_MODULES_JSON "${PPR_LIBCXX_MODULES_JSON}" ABSOLUTE)
    set(PPR_LIBCXX_MODULES_JSON "${PPR_LIBCXX_MODULES_JSON}" CACHE FILEPATH "libc++ modules manifest" FORCE)
endif()

if(PPR_LIBCXX_SELECTED_MANIFEST)
    get_filename_component(PPR_LIBCXX_SELECTED_MANIFEST "${PPR_LIBCXX_SELECTED_MANIFEST}" ABSOLUTE)
elseif(PPR_LIBCXX_MODULES_JSON)
    set(PPR_LIBCXX_SELECTED_MANIFEST "${PPR_LIBCXX_MODULES_JSON}")
endif()

if(PPR_LIBCXX_SELECTED_MANIFEST)
    set(CMAKE_CXX_STDLIB_MODULES_JSON "${PPR_LIBCXX_SELECTED_MANIFEST}")
    message(STATUS "Using libc++ modules manifest: ${CMAKE_CXX_STDLIB_MODULES_JSON}")
else()
    # No candidate resolved. Clear the effective value and both cached
    # selections before reporting, so neither CMake's probe nor the next
    # configure can consume a stale manifest path.
    unset(CMAKE_CXX_STDLIB_MODULES_JSON)
    unset(CMAKE_CXX_STDLIB_MODULES_JSON CACHE)
    unset(PPR_LIBCXX_MODULES_JSON CACHE)
    set(PPR_LIBCXX_MODULES_JSON "")

    message(STATUS "No resolvable libc++.modules.json manifest found; leaving "
            "CMAKE_CXX_STDLIB_MODULES_JSON unset for CMake's default lookup")
endif()
unset(PPR_LIBCXX_SELECTED_MANIFEST)
unset(PPR_LIBCXX_MANIFEST_CANDIDATES)
unset(PPR_LIBCXX_COMPILER_MANIFEST)
unset(PPR_LIBCXX_MODULES_JSON_HINTS)
