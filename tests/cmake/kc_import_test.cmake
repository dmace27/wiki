# Exercise `kc import` as a user would, including project discovery, JSON
# output, idempotence, changed-content versioning, and retained originals.
if(NOT DEFINED KC_EXE OR NOT DEFINED KC_TEST_ROOT)
  message(FATAL_ERROR "KC_EXE and KC_TEST_ROOT are required")
endif()

file(REMOVE_RECURSE "${KC_TEST_ROOT}")
file(MAKE_DIRECTORY "${KC_TEST_ROOT}/work")

execute_process(
  COMMAND "${KC_EXE}" --project "${KC_TEST_ROOT}" init --quiet
  RESULT_VARIABLE init_status
  ERROR_VARIABLE init_error
)
if(NOT init_status EQUAL 0)
  message(FATAL_ERROR "kc init failed (${init_status}): ${init_error}")
endif()

set(source_file "${KC_TEST_ROOT}/work/probability.txt")
file(WRITE "${source_file}" "first revision\n")
execute_process(
  COMMAND "${KC_EXE}" import "${source_file}" --json
  WORKING_DIRECTORY "${KC_TEST_ROOT}/work"
  RESULT_VARIABLE first_status
  OUTPUT_VARIABLE first_output
  ERROR_VARIABLE first_error
)
if(NOT first_status EQUAL 0)
  message(FATAL_ERROR "kc import failed (${first_status}): ${first_error}")
endif()
if(NOT first_output MATCHES "\"version_created\":true")
  message(FATAL_ERROR "first import did not create a version: ${first_output}")
endif()
if(NOT first_output MATCHES "\"stored_path\":\"sources/")
  message(FATAL_ERROR "import did not report a project-relative retained path: ${first_output}")
endif()

execute_process(
  COMMAND "${KC_EXE}" --project "${KC_TEST_ROOT}" --json import "${source_file}"
  RESULT_VARIABLE repeated_status
  OUTPUT_VARIABLE repeated_output
  ERROR_VARIABLE repeated_error
)
if(NOT repeated_status EQUAL 0)
  message(FATAL_ERROR "repeated import failed (${repeated_status}): ${repeated_error}")
endif()
if(NOT repeated_output MATCHES "\"deduplicated\":true")
  message(FATAL_ERROR "repeated import was not deduplicated: ${repeated_output}")
endif()

file(WRITE "${source_file}" "second revision\n")
execute_process(
  COMMAND "${KC_EXE}" --project "${KC_TEST_ROOT}" --json import "${source_file}"
  RESULT_VARIABLE changed_status
  OUTPUT_VARIABLE changed_output
  ERROR_VARIABLE changed_error
)
if(NOT changed_status EQUAL 0)
  message(FATAL_ERROR "changed import failed (${changed_status}): ${changed_error}")
endif()
if(NOT changed_output MATCHES "\"version_created\":true")
  message(FATAL_ERROR "changed content did not create a version: ${changed_output}")
endif()

file(REMOVE "${source_file}")
file(GLOB_RECURSE retained_files
  LIST_DIRECTORIES false
  "${KC_TEST_ROOT}/sources/*"
)
list(LENGTH retained_files retained_count)
if(NOT retained_count EQUAL 2)
  message(FATAL_ERROR "expected two retained immutable versions, found ${retained_count}")
endif()
