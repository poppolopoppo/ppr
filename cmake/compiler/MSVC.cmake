# Configure vcpkg toolchain if available
if (NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if (DEFINED ENV{VCPKG_ROOT})
        set(CMAKE_TOOLCHAIN_FILE
                "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
                CACHE STRING "")
    endif ()
else ()
    message(STATUS "Using preconfigured cmake toolchain file: ${CMAKE_TOOLCHAIN_FILE}")
endif ()

if (PPR_EDIT_AND_CONTINUE)
    # /ZI (EditAndContinue) required for VS Edit & Continue. Forces /Gy+/FC, needs
    # /DEBUG:FULL+/INCREMENTAL, conflicts with /OPT:REF/ICF and /LTCG.
    # /DEBUG:FASTLINK is forbidden here (LNK4075 with /INCREMENTAL).
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug>:EditAndContinue>" CACHE STRING "" FORCE)
    # EnC link contract (live-only: this branch is PPR_EDIT_AND_CONTINUE-scoped,
    # so msvc-rel static+LTCG is untouched): /DEBUG:FULL + /INCREMENTAL +
    # /OPT:NOREF,NOICF + /LTCG:OFF. /PDBTMCACHE speeds EnC session startup.
    # Each flag is its own genex element so Ninja/cl quoting stays one-token-per-flag.
    add_link_options(
        "$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Debug>>:LINKER:/DEBUG:FULL>"
        "$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Debug>>:LINKER:/INCREMENTAL>"
        "$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Debug>>:LINKER:/OPT:NOREF>"
        "$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Debug>>:LINKER:/OPT:NOICF>"
        "$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Debug>>:LINKER:/LTCG:OFF>"
        "$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Debug>>:LINKER:/PDBTMCACHE>"
    )
else ()
    # Release ships a shared PDB (/Zi) for crash dumps; non-Release non-EnC
    # stays on per-object debug info (/Z7) for ccache support and to avoid
    # PDB lock contention in parallel builds.
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Release>:ProgramDatabase>$<$<NOT:$<CONFIG:Release>>:Embedded>" CACHE STRING "" FORCE)
endif ()

# Release link contract (shipping: CONFIG:Release-genexed, unconditional on
# Release — the EnC validator requires Debug so the two contracts never meet):
# /LTCG + /OPT:REF,ICF + /INCREMENTAL:NO for a fully optimized image, plus
# /DEBUG with a stripped PDB (/PDBSTRIPPED without a filename derives the
# stripped PDB name from the full PDB name).
add_link_options(
    "$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:LINKER:/LTCG>"
    "$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:LINKER:/OPT:REF>"
    "$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:LINKER:/OPT:ICF>"
    "$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:LINKER:/INCREMENTAL:NO>"
    "$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:LINKER:/DEBUG>"
    "$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:LINKER:/PDBSTRIPPED>"
)

# Enable C++ exceptions (required by the C++ Standard Library module)
add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/EHsc>")

# Ensure /utf-8 is applied to every target that builds or imports modules.
add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")

# /bigobj is needed for large module interface TUs. Applying it globally (rather
# than as a per-target PRIVATE option on engine.tests.core/engine.tests.app) keeps all
# @cmake_cxx_std synth targets consistent — per-target /bigobj creates a divergent
# synth that triggers "Disagreement of the location of the 'std' module" errors.
add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/bigobj>")

# Release profile: pin /O2 /Ob2 explicitly (/DNDEBUG comes from CMAKE_BUILD_TYPE),
# plus whole-program optimization (/GL) and global-data optimization (/Gw) paired
# with /Zc:checkGwOdr (exists since VS 2022 17.5; repo toolchain floor is 17.x).
# /arch:AVX2 stays opt-in via PPR_ENABLE_AVX2 (default OFF — not default min-spec).
# Applied globally so module synth targets (including the std module BMI) see
# exactly the same flags — divergent flags break module BCI location matching.
option(PPR_RELEASE_PERF_FLAGS "Enable extra release-only performance flags (/O2 /Ob2 /GL /Gw /Zc:checkGwOdr)" ON)
option(PPR_ENABLE_AVX2 "Enable AVX2 codegen (/arch:AVX2, requires a 2013+ CPU) on Release" OFF)
if (PPR_RELEASE_PERF_FLAGS)
    # Each flag must be its own genex element: a single "/arch:AVX2 /Gw" string
    # is quoted as one argv token and cl.exe rejects it with D9002.
    add_compile_options(
        "$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/O2>"
        "$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/Ob2>"
        "$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/GL>"
        "$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/Gw>"
        "$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/Zc:checkGwOdr>"
    )
    # /GL is incompatible with /ZI — the Release-only genex above keeps them
    # disjoint (EnC is Debug-only by validator). No Debug line gains /GL.
    if (PPR_ENABLE_AVX2)
        add_compile_options(
            "$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/arch:AVX2>"
        )
    endif ()
endif ()

# The /Zc:__cplusplus compiler option enables the __cplusplus preprocessor macro to report an updated
# value for recent C++ language standards support.
# By default, Visual Studio always returns the value 199711L for the __cplusplus preprocessor macro.
# https://learn.microsoft.com/en-us/cpp/build/reference/zc-cplusplus?view=msvc-170
add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/Zc:__cplusplus>")

set(PPR_PROJECT_WARNINGS_CXX
        /permissive- # standards conformance mode for MSVC compiler.

        /W4 # Baseline reasonable warnings

        /wd4201 # nonstandard extension used: nameless struct/union
        /w14242 # 'identifier': conversion from 'type1' to 'type1', possible loss of data
        /w14254 # 'operator': conversion from 'type1': field to 'type2': field, possible loss of data
        /w14263 # 'function': member function does not override any base class virtual member function
        /w14265 # 'classname': class has virtual functions, but destructor is not virtual instances of this class may not be destructed correctly
        /w14287 # 'operator': unsigned/negative constant mismatch
        /we4289 # nonstandard extension used: 'variable': loop control variable declared in the for-loop is used outside the for-loop scope
        /w14296 # 'operator': expression is always 'boolean_value'
        /w14311 # 'variable': pointer truncation from 'type1' to 'type2'
        /w14545 # expression before comma evaluates to a function which is missing an argument list
        /w14546 # function call before comma missing argument list
        /w14547 # 'operator': operator before comma has no effect; expected operator with side-effect
        /w14549 # 'operator': operator before comma has no effect; did you intend 'operator'?
        /w14555 # expression has no effect; expected expression with side- effect
        /w14619 # pragma warning: there is no warning number 'number'
        /w14640 # Enable warning on thread un-safe static member initialization
        /w14826 # Conversion from 'type1' to 'type_2' is sign-extended. This may cause unexpected runtime behavior.
        /w14905 # wide string literal cast to 'LPSTR'
        /w14906 # string literal cast to 'LPWSTR'
        /w14928 # illegal copy-initialization; more than one user-defined conversion has been implicitly applied

        /wd5050 # possible incompatible environment while importing module 'std': _UTF8 is defined in current command line and not in module command line
)

if (PPR_ENABLE_SANITIZER_ADDRESS)
    add_compile_options("/D_ANNOTATE_STL")
endif ()

if (PPR_WARNINGS_AS_ERRORS)
    set(PPR_PROJECT_WARNINGS_CXX ${PPR_PROJECT_WARNINGS_CXX} /WX)
endif ()
