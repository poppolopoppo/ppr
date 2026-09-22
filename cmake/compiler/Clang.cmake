
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
)

# Add libc++ include paths for module support
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++")

# Locate the libc++ modules manifest without pinning an LLVM major version.
# Probe llvm-config, LLVM_DIR, and the compiler resource dir first, then fall
# back to well-known versioned/multiarch locations. Only set the variable when
# the file exists so newer/older toolchains keep working.
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
find_file(
    PPR_LIBCXX_MODULES_JSON
    NAMES libc++.modules.json
    PATHS
        ${PPR_LIBCXX_MODULES_JSON_HINTS}
        /usr/lib/llvm-22/lib
        /usr/lib/llvm-20/lib
        /usr/lib/x86_64-linux-gnu
        /usr/lib/llvm/lib
)
if(PPR_LIBCXX_MODULES_JSON)
    set(CMAKE_CXX_STDLIB_MODULES_JSON "${PPR_LIBCXX_MODULES_JSON}")
    message(STATUS "Using libc++ modules manifest: ${CMAKE_CXX_STDLIB_MODULES_JSON}")
else()
    message(STATUS "libc++.modules.json not found; leaving CMAKE_CXX_STDLIB_MODULES_JSON unset (CMake will use its default lookup)")
endif()
unset(PPR_LIBCXX_MODULES_JSON_HINTS)

if(PPR_WARNINGS_AS_ERRORS)
  set(PPR_PROJECT_WARNINGS_CXX ${PPR_PROJECT_WARNINGS_CXX} -Werror)
endif()
