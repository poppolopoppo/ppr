# ---------------------------------------------------------------------------
# ppr_add_test_entry
# ---------------------------------------------------------------------------
# Registers one CTest entry and owns the properties every entry shares.
# The entry name mirrors the owning target: `<target>` in the single
# suite-per-target form, `<target>.<qualifier>` when one target backs several
# entries (suite shards, tool profile validations). The working directory and
# PATH prepend always point at the owning target's file directory so runtime
# DLLs resolve beside the executed executable.
#
# Usage:
#   ppr_add_test_entry(TARGET <target>
#     [NAME <qualifier>]  # omit for the one-suite-per-target entry name
#     [FILTER <path>]     # invoke with --run-test <path>; one token, no ';'
#     [EXCLUDE <path>]    # invoke with --skip-test <path>; comma list; may combine with FILTER
#     [LOOPS]             # invoke with --loop ${PPR_TEST_LOOPS}
#     [ARGS <word>...]    # raw extra command words (exclusive with FILTER)
#     [RUN_SERIAL]
#     [PROCESSORS <count>]     # CTest PROCESSORS scheduling property
#     [RESOURCE_LOCK <name>...] # CTest RESOURCE_LOCK property; names joined with ';'
#     TIMEOUT <seconds>   # required
#     LABELS <label>...)  # required; bare words (function args lose ';')
function(ppr_add_test_entry)
  set(options LOOPS RUN_SERIAL)
  set(oneValueArgs TARGET NAME FILTER EXCLUDE PROCESSORS TIMEOUT)
  set(multiValueArgs ARGS RESOURCE_LOCK LABELS)
  cmake_parse_arguments(PPR_TEST_ENTRY "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  if(NOT PPR_TEST_ENTRY_TARGET)
    message(FATAL_ERROR "ppr_add_test_entry: TARGET is required")
  endif()
  if(PPR_TEST_ENTRY_KEYWORDS_MISSING_VALUES)
    message(FATAL_ERROR "ppr_add_test_entry(${PPR_TEST_ENTRY_TARGET}): keyword called without a value: "
      "'${PPR_TEST_ENTRY_KEYWORDS_MISSING_VALUES}'")
  endif()
  if(PPR_TEST_ENTRY_FILTER AND PPR_TEST_ENTRY_ARGS)
    message(FATAL_ERROR "ppr_add_test_entry(${PPR_TEST_ENTRY_TARGET}): FILTER and ARGS are exclusive")
  endif()
  if(PPR_TEST_ENTRY_LOOPS AND NOT PPR_TEST_LOOPS)
    message(FATAL_ERROR "ppr_add_test_entry(${PPR_TEST_ENTRY_TARGET}): LOOPS requires PPR_TEST_LOOPS; "
      "check include order against lib/engine/tests")
  endif()
  if(NOT PPR_TEST_ENTRY_TIMEOUT)
    message(FATAL_ERROR "ppr_add_test_entry(${PPR_TEST_ENTRY_TARGET}): TIMEOUT is required")
  endif()
  if(NOT PPR_TEST_ENTRY_LABELS)
    message(FATAL_ERROR "ppr_add_test_entry(${PPR_TEST_ENTRY_TARGET}): LABELS is required")
  endif()

  if(PPR_TEST_ENTRY_NAME)
    set(ppr_test_entry_name "${PPR_TEST_ENTRY_TARGET}.${PPR_TEST_ENTRY_NAME}")
  else()
    set(ppr_test_entry_name "${PPR_TEST_ENTRY_TARGET}")
  endif()

  set(ppr_test_entry_command "${PPR_TEST_ENTRY_TARGET}")
  if(PPR_TEST_ENTRY_FILTER)
    list(APPEND ppr_test_entry_command --run-test "${PPR_TEST_ENTRY_FILTER}")
  endif()
  if(PPR_TEST_ENTRY_EXCLUDE)
    list(APPEND ppr_test_entry_command --skip-test "${PPR_TEST_ENTRY_EXCLUDE}")
  endif()
  if(PPR_TEST_ENTRY_LOOPS)
    list(APPEND ppr_test_entry_command --loop "${PPR_TEST_LOOPS}")
  endif()
  if(PPR_TEST_ENTRY_ARGS)
    list(APPEND ppr_test_entry_command ${PPR_TEST_ENTRY_ARGS})
  endif()

  add_test(NAME "${ppr_test_entry_name}" COMMAND ${ppr_test_entry_command})

  # Direct quoted values: builtin commands keep quoted ';' verbatim, while the
  # $"{VAR}" round-trip would re-split it.
  set_tests_properties("${ppr_test_entry_name}" PROPERTIES
    TIMEOUT "${PPR_TEST_ENTRY_TIMEOUT}"
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:${PPR_TEST_ENTRY_TARGET}>"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:${PPR_TEST_ENTRY_TARGET}>"
  )
  set_tests_properties("${ppr_test_entry_name}" PROPERTIES
    LABELS "${PPR_TEST_ENTRY_LABELS}"
  )
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND PPR_ENABLE_SANITIZER_ADDRESS)
    # Preserve driver symbols for the narrowly scoped, documented NVIDIA cache
    # suppressions. Existing LSAN_OPTIONS and all engine leak checks stay active.
    set_property(TEST "${ppr_test_entry_name}" APPEND PROPERTY ENVIRONMENT_MODIFICATION
      "VK_LOADER_DISABLE_DYNAMIC_LIBRARY_UNLOADING=set:1"
      "LSAN_OPTIONS=string_append::suppressions=${CMAKE_SOURCE_DIR}/cmake/sanitizers/nvidia.lsan.supp"
    )
  endif()
  if(PPR_ENABLE_SANITIZER_UNDEFINED AND NOT MSVC)
    # A diagnostic is a failed check, not a passing test with an ignored warning.
    set_property(TEST "${ppr_test_entry_name}" APPEND PROPERTY ENVIRONMENT_MODIFICATION
      "UBSAN_OPTIONS=string_append::halt_on_error=1:print_stacktrace=1"
    )
  endif()
  if(PPR_TEST_ENTRY_RUN_SERIAL)
    set_tests_properties("${ppr_test_entry_name}" PROPERTIES RUN_SERIAL "TRUE")
  endif()
  if(PPR_TEST_ENTRY_PROCESSORS)
    set_tests_properties("${ppr_test_entry_name}" PROPERTIES PROCESSORS "${PPR_TEST_ENTRY_PROCESSORS}")
  endif()
  if(PPR_TEST_ENTRY_RESOURCE_LOCK)
    # RESOURCE_LOCK is a ';'-separated property: join before handing it over,
    # or the additional names would be parsed as more property names.
    list(JOIN PPR_TEST_ENTRY_RESOURCE_LOCK ";" ppr_test_entry_resource_lock)
    set_tests_properties("${ppr_test_entry_name}" PROPERTIES RESOURCE_LOCK "${ppr_test_entry_resource_lock}")
  endif()

  unset(ppr_test_entry_name)
  unset(ppr_test_entry_command)
  unset(ppr_test_entry_resource_lock)
endfunction()
