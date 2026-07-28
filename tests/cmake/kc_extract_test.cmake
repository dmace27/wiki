# Exercise text extraction through the real CLI. PDF/OCR behavior is covered by
# adapter and pipeline tests so this test stays independent of local runtimes.
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
file(WRITE "${source_file}" "Markov chains have page-local evidence.\n")
execute_process(
  COMMAND "${KC_EXE}" --project "${KC_TEST_ROOT}" --json import "${source_file}"
  RESULT_VARIABLE import_status
  OUTPUT_VARIABLE import_output
  ERROR_VARIABLE import_error
)
if(NOT import_status EQUAL 0)
  message(FATAL_ERROR "kc import failed (${import_status}): ${import_error}")
endif()
if(NOT import_output MATCHES "\"source_id\":\"(src_[0-9A-HJKMNP-TV-Z]+)\"")
  message(FATAL_ERROR "could not read source ID from import output: ${import_output}")
endif()
set(source_id "${CMAKE_MATCH_1}")

# Extraction must use the retained copy, not the original path.
file(REMOVE "${source_file}")
execute_process(
  COMMAND "${KC_EXE}" extract "${source_id}" --json
  WORKING_DIRECTORY "${KC_TEST_ROOT}/work"
  RESULT_VARIABLE extract_status
  OUTPUT_VARIABLE extract_output
  ERROR_VARIABLE extract_error
)
if(NOT extract_status EQUAL 0)
  message(FATAL_ERROR "kc extract failed (${extract_status}): ${extract_error}")
endif()
if(NOT extract_output MATCHES "\"page_count\":1")
  message(FATAL_ERROR "text extraction did not create one page: ${extract_output}")
endif()
if(NOT extract_output MATCHES "\"text_status\":\"native\"")
  message(FATAL_ERROR "text extraction did not use native status: ${extract_output}")
endif()
if(NOT extract_output MATCHES "\"reused\":false")
  message(FATAL_ERROR "first extraction was unexpectedly reused: ${extract_output}")
endif()

execute_process(
  COMMAND "${KC_EXE}" --project "${KC_TEST_ROOT}" --json extract "${source_id}"
  RESULT_VARIABLE repeated_status
  OUTPUT_VARIABLE repeated_output
  ERROR_VARIABLE repeated_error
)
if(NOT repeated_status EQUAL 0)
  message(FATAL_ERROR "repeated extraction failed (${repeated_status}): ${repeated_error}")
endif()
if(NOT repeated_output MATCHES "\"reused\":true")
  message(FATAL_ERROR "repeated extraction was not idempotent: ${repeated_output}")
endif()
