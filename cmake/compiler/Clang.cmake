
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

# The libc++ modules manifest is discovered by cmake/LibcxxModulesManifest.cmake,
# which root CMakeLists.txt includes before project() so CMake's own probe
# cannot record GCC's libstdc++ manifest for a libc++ build. Including it here
# keeps the post-project selection path intact; the second pass is a no-op.
include(LibcxxModulesManifest)

if(PPR_WARNINGS_AS_ERRORS)
  set(PPR_PROJECT_WARNINGS_CXX ${PPR_PROJECT_WARNINGS_CXX} -Werror)
endif()
