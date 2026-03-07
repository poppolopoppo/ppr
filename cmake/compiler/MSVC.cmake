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

# Ensure /utf-8 is applied to every target that builds or imports modules.
add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")

# The /Zc:__cplusplus compiler option enables the __cplusplus preprocessor macro to report an updated
# value for recent C++ language standards support.
# By default, Visual Studio always returns the value 199711L for the __cplusplus preprocessor macro.
# https://learn.microsoft.com/en-us/cpp/build/reference/zc-cplusplus?view=msvc-170
add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/Zc:__cplusplus>")

set(PROJECT_WARNINGS_CXX
        /permissive- # standards conformance mode for MSVC compiler.

        /W4 # Baseline reasonable warnings
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

if (PPR_WARNINGS_AS_ERRORS)
    set(PROJECT_WARNINGS_CXX ${PROJECT_WARNINGS_CXX} /WX)
endif ()
