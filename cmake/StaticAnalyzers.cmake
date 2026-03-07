macro(enable_cppcheck PPR_WARNINGS_AS_ERRORS CPPCHECK_OPTIONS)
  find_program(CPPCHECK cppcheck)
  if(CPPCHECK)
    if(CMAKE_GENERATOR MATCHES ".*Visual Studio.*")
      set(CPPCHECK_TEMPLATE "vs")
    else()
      set(CPPCHECK_TEMPLATE "gcc")
    endif()

    if("${CPPCHECK_OPTIONS}" STREQUAL "")
      set(CMAKE_CXX_CPPCHECK
          ${CPPCHECK}
          --template=${CPPCHECK_TEMPLATE}
          --enable=style,performance,warning,portability
          --inline-suppr
          --suppress=cppcheckError
          --suppress=internalAstError
          --suppress=unmatchedSuppression
          --suppress=passedByValue
          --suppress=syntaxError
          --suppress=preprocessorErrorDirective
          --inconclusive)
    else()
      set(CMAKE_CXX_CPPCHECK ${CPPCHECK} --template=${CPPCHECK_TEMPLATE} ${CPPCHECK_OPTIONS})
    endif()

    if(${PPR_WARNINGS_AS_ERRORS})
      list(APPEND CMAKE_CXX_CPPCHECK --error-exitcode=2)
    endif()
  else()
    message(STATUS "cppcheck requested but executable not found")
  endif()
endmacro()

macro(enable_clang_tidy target PPR_WARNINGS_AS_ERRORS)
  find_program(CLANGTIDY clang-tidy)
  if(CLANGTIDY)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES ".*Clang")
      get_target_property(TARGET_PCH ${target} INTERFACE_PRECOMPILE_HEADERS)
      if("${TARGET_PCH}" STREQUAL "TARGET_PCH-NOTFOUND")
        get_target_property(TARGET_PCH ${target} PRECOMPILE_HEADERS)
      endif()

      if(NOT ("${TARGET_PCH}" STREQUAL "TARGET_PCH-NOTFOUND"))
        message(
          SEND_ERROR
            "clang-tidy cannot be enabled with non-clang compiler and PCH, clang-tidy fails to handle gcc's PCH file")
      endif()
    endif()

    set(CLANG_TIDY_OPTIONS
        ${CLANGTIDY}
        -extra-arg=-Wno-unknown-warning-option
        -extra-arg=-Wno-ignored-optimization-argument
        -extra-arg=-Wno-unused-command-line-argument
        -p)
    if(${PPR_WARNINGS_AS_ERRORS})
      list(APPEND CLANG_TIDY_OPTIONS -warnings-as-errors=*)
    endif()

    set_target_properties(${target} PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_OPTIONS}")
  else()
    message(STATUS "clang-tidy requested but executable not found")
  endif()
endmacro()
