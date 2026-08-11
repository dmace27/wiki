# Exercise extraction review and correction through the real terminal CLI.
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
file(WRITE "${source_file}" "Markov chaim notes need review.\n")
execute_process(
  COMMAND "${KC_EXE}" --project "${KC_TEST_ROOT}" --json import "${source_file}"
  RESULT_VARIABLE import_status
  OUTPUT_VARIABLE import_output
  ERROR_VARIABLE import_error
)
if(NOT import_status EQUAL 0 OR
   NOT import_output MATCHES "\"source_id\":\"(src_[0-9A-HJKMNP-TV-Z]+)\"")
  message(FATAL_ERROR "kc import failed or omitted source ID: ${import_error} ${import_output}")
endif()
set(source_id "${CMAKE_MATCH_1}")

execute_process(
  COMMAND "${KC_EXE}" --project "${KC_TEST_ROOT}" extract "${source_id}" --quiet
  RESULT_VARIABLE extract_status
  ERROR_VARIABLE extract_error
)
if(NOT extract_status EQUAL 0)
  message(FATAL_ERROR "kc extract failed (${extract_status}): ${extract_error}")
endif()

execute_process(
  COMMAND "${KC_EXE}" --project "${KC_TEST_ROOT}" --json review extraction
          "${source_id}"
  RESULT_VARIABLE review_status
  OUTPUT_VARIABLE review_output
  ERROR_VARIABLE review_error
)
if(NOT review_status EQUAL 0)
  message(FATAL_ERROR "kc review extraction failed (${review_status}): ${review_error}")
endif()
if(NOT review_output MATCHES "\"image_path\":null" OR
   NOT review_output MATCHES "\"text_status\":\"native\"" OR
   NOT review_output MATCHES "Markov chaim notes need review")
  message(FATAL_ERROR "review output did not combine image, text, and status: ${review_output}")
endif()

execute_process(
  COMMAND "${KC_EXE}" --project "${KC_TEST_ROOT}" --json review extraction
          "${source_id}" --page 1 --text "Corrected Markov chain notes."
  RESULT_VARIABLE correction_status
  OUTPUT_VARIABLE correction_output
  ERROR_VARIABLE correction_error
)
if(NOT correction_status EQUAL 0)
  message(FATAL_ERROR "page correction failed (${correction_status}): ${correction_error}")
endif()
if(NOT correction_output MATCHES "Corrected Markov chain notes" OR
   NOT correction_output MATCHES "\"text_status\":\"reviewed\"" OR
   NOT correction_output MATCHES "\"corrected\":\\{")
  message(FATAL_ERROR "correction was not returned as reviewed: ${correction_output}")
endif()

set(missing_proposal "prp_01J00000000000000000000000")
execute_process(
  COMMAND "${KC_EXE}" --project "${KC_TEST_ROOT}" --json proposal show
          "${missing_proposal}"
  RESULT_VARIABLE show_status
  OUTPUT_VARIABLE show_output
)
if(NOT show_status EQUAL 3 OR
   NOT show_output MATCHES "\"code\":\"proposal_not_found\"")
  message(FATAL_ERROR "missing proposal show used the wrong contract: ${show_output}")
endif()

execute_process(
  COMMAND "${KC_EXE}" --project "${KC_TEST_ROOT}" --json proposal reject
          "${missing_proposal}" --reason "not ready"
  RESULT_VARIABLE reject_status
  OUTPUT_VARIABLE reject_output
)
if(NOT reject_status EQUAL 3 OR
   NOT reject_output MATCHES "\"code\":\"proposal_not_found\"")
  message(FATAL_ERROR "missing proposal reject used the wrong contract: ${reject_output}")
endif()
