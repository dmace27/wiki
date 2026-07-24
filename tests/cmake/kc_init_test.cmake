# This script is a black-box test: it runs the built executable exactly as a
# user would, then checks its exit status, output contract, and created files.
if(NOT DEFINED KC_EXE OR NOT DEFINED KC_TEST_ROOT)
  message(FATAL_ERROR "KC_EXE and KC_TEST_ROOT are required")
endif()

# Start from a known-empty project directory.
file(REMOVE_RECURSE "${KC_TEST_ROOT}")
# Run init again to prove it validates rather than duplicating or failing.
execute_process(
  COMMAND "${KC_EXE}" init --project "${KC_TEST_ROOT}" --json --vault vault
  RESULT_VARIABLE init_status
  OUTPUT_VARIABLE init_output
  ERROR_VARIABLE init_error
)

if(NOT init_status EQUAL 0)
  message(FATAL_ERROR "kc init failed (${init_status}): ${init_error}")
endif()
if(NOT init_output MATCHES "\"ok\":true")
  message(FATAL_ERROR "kc init did not emit the JSON success contract: ${init_output}")
endif()
if(NOT EXISTS "${KC_TEST_ROOT}/kc.json")
  message(FATAL_ERROR "kc init did not create kc.json")
endif()
if(NOT EXISTS "${KC_TEST_ROOT}/.knowledge-compiler/state.sqlite")
  message(FATAL_ERROR "kc init did not create state.sqlite")
endif()

execute_process(
  COMMAND "${KC_EXE}" --project "${KC_TEST_ROOT}" --quiet init
  RESULT_VARIABLE second_status
  ERROR_VARIABLE second_error
)
if(NOT second_status EQUAL 0)
  message(FATAL_ERROR "second kc init was not idempotent: ${second_error}")
endif()
