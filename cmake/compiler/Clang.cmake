
set(PPR_PROJECT_WARNINGS_CXX
  -Wall
  -Wextra # reasonable and standard
  -Wshadow # warn the user if a variable declaration shadows one from a parent context
  -Wnon-virtual-dtor # warn the user if a class with virtual functions has a non-virtual destructor. This helps
  -Wno-assume # ignore invalid assume with potential side-effects
  # catch hard to track down memory errors
  -Wold-style-cast # warn for c-style casts
  -Wcast-align # warn for potential performance problem casts
  -Wunused # warn on anything being unused
  -Woverloaded-virtual # warn if you overload (not override) a virtual function
  -Wpedantic # warn if non-standard C++ is used
  -Wconversion # warn on type conversions that may lose data
  -Wsign-conversion # warn on sign conversions
  -Wnull-dereference # warn if a null dereference is detected
  -Wdouble-promotion # warn if float is implicit promoted to double
  -Wformat=2 # warning on security issues around functions that format output (ie printf)
  -Wimplicit-fallthrough # warn on statements that fallthrough without an explicit annotation
  -Wextra-semi # Warn about semicolon after in-class function definition.
  # The compiler's own `std`/`std.compat` module sources use the reserved module
  # name `std`; silence that warning so the synthesized std BMI precompiles
  # (which inherit these PRIVATE options) succeed under -Werror. Project code
  # never declares a module named `std`, so this changes no project diagnostic.
  -Wno-reserved-module-identifier
)

# `##__VA_ARGS__` comma-swallowing in include/pP/Macros.h is a deliberate GNU
# extension used for optional log arguments; -Wpedantic rejects it under
# -Werror, and only Clang implements this specific diagnostic.
if(CMAKE_CXX_COMPILER_ID MATCHES ".*Clang")
  list(APPEND PPR_PROJECT_WARNINGS_CXX -Wno-gnu-zero-variadic-macro-arguments)
endif()

# Add libc++ include paths for module support
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++")

# ------------------------------------------------------------------
# optimized-build debug info and frame chain
# ------------------------------------------------------------------
# CMake's Release profile is `-O3 -DNDEBUG` with no `-g`, and Clang omits the
# frame pointer at -O1+ on x86-64. The resulting image carries `.symtab` only
# (function names, no lines or inline frames) and no frame chain, so
# `nsys --backtrace=fp` (its default) cannot unwind and `perf --call-graph fp`
# gets nothing either. Restore what CPU samplers need without moving the
# optimization level:
#   -g2                          full DWARF (lines, types, inline, variables)
#   -gz=zlib                     compress .debug_* (SHF_COMPRESSED; binutils
#                                perf, LLVM tools, nsys and gdb all read it)
#   -fdebug-info-for-profiling   discriminators/prologue markers for sampled PCs
#   -fno-omit-frame-pointer      invariant rbp chain through every function
#   -mno-omit-leaf-frame-pointer also keep it in leaf functions (x86 only)
# -g and -gz never change generated code. The frame-pointer pair is the only
# flag here with a codegen effect (~0-2% typical, measured per workload) and is
# what makes fp unwinding work at all. PPR_DEBUG_INFO=OFF restores the previous
# minimal optimized image for size-sensitive packaging.
option(PPR_DEBUG_INFO "Emit DWARF and retain frame pointers in optimized builds for profiling/backtraces" ON)
if(PPR_DEBUG_INFO)
  include(CheckCXXCompilerFlag)

  set(PPR_DEBUG_INFO_FLAGS -g2)
  check_cxx_compiler_flag(-gz=zlib PPR_HAS_GZ_ZLIB)
  if(PPR_HAS_GZ_ZLIB)
    list(APPEND PPR_DEBUG_INFO_FLAGS -gz=zlib)
  endif()
  check_cxx_compiler_flag(-fdebug-info-for-profiling PPR_HAS_DEBUG_INFO_FOR_PROFILING)
  if(PPR_HAS_DEBUG_INFO_FOR_PROFILING)
    list(APPEND PPR_DEBUG_INFO_FLAGS -fdebug-info-for-profiling)
  endif()
  list(APPEND PPR_DEBUG_INFO_FLAGS -fno-omit-frame-pointer)

  # Non-x86 targets keep the leaf frame whenever -fno-omit-frame-pointer is set
  # and reject the -m flag.
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    list(APPEND PPR_DEBUG_INFO_FLAGS -mno-omit-leaf-frame-pointer)
  endif()

  # One genex element per flag so Ninja keeps one argv token per flag, applied
  # globally so the module synth targets (including the `std` BMI) see the same
  # set as their importers. Debug already passes -g and keeps frame pointers.
  foreach(_ppr_debug_info_flag IN LISTS PPR_DEBUG_INFO_FLAGS)
    add_compile_options("$<$<NOT:$<CONFIG:Debug>>:${_ppr_debug_info_flag}>")
  endforeach()
  unset(_ppr_debug_info_flag)

  # The compile-time -gz=zlib only compresses the input objects; GNU ld and lld
  # re-emit .debug_* uncompressed unless told otherwise, which turns app.game
  # from ~24 MB into ~200 MB. Ask the linker for the same compression.
  include(CheckLinkerFlag)
  check_linker_flag(CXX "-Wl,--compress-debug-sections=zlib" PPR_HAS_COMPRESS_DEBUG_SECTIONS)
  if(PPR_HAS_COMPRESS_DEBUG_SECTIONS)
    add_link_options("$<$<NOT:$<CONFIG:Debug>>:-Wl,--compress-debug-sections=zlib>")
  endif()
endif()

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

# Locate the libc++ modules manifest without pinning an LLVM major version.
# Ask the compiler for its own manifest first, then probe llvm-config,
# LLVM_DIR, and the compiler resource dir, then fall back to well-known
# versioned/multiarch locations. Only set the variable when a candidate
# resolves so newer/older toolchains keep working.
set(PPR_LIBCXX_COMPILER_MANIFEST)
if(CMAKE_CXX_COMPILER AND NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
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

# An existing override or cached selection is tried first so a working choice
# stays stable, but it is only kept when it still resolves.
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

if(PPR_LIBCXX_MODULES_JSON)
    get_filename_component(PPR_LIBCXX_MODULES_JSON "${PPR_LIBCXX_MODULES_JSON}" ABSOLUTE)
    set(PPR_LIBCXX_MODULES_JSON "${PPR_LIBCXX_MODULES_JSON}" CACHE FILEPATH "libc++ modules manifest" FORCE)
    set(CMAKE_CXX_STDLIB_MODULES_JSON "${PPR_LIBCXX_MODULES_JSON}")
    message(STATUS "Using libc++ modules manifest: ${CMAKE_CXX_STDLIB_MODULES_JSON}")
else()
    message(STATUS "libc++.modules.json not found; leaving CMAKE_CXX_STDLIB_MODULES_JSON unset (CMake will use its default lookup)")
endif()
unset(PPR_LIBCXX_MANIFEST_CANDIDATES)
unset(PPR_LIBCXX_COMPILER_MANIFEST)
unset(PPR_LIBCXX_MODULES_JSON_HINTS)

if(PPR_WARNINGS_AS_ERRORS)
  set(PPR_PROJECT_WARNINGS_CXX ${PPR_PROJECT_WARNINGS_CXX} -Werror)
endif()
