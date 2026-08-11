# Exercise the review/apply CLI shape without constructing a model proposal.
# Successful rendering is covered by the deterministic VaultWriter integration
# tests; this black-box test verifies command routing, JSON, and exit codes.
if(NOT DEFINED KC_EXE OR NOT DEFINED KC_TEST_ROOT)
  message(FATAL_ERROR "KC_EXE and KC_TEST_ROOT are required")
endif()

file(REMOVE_RECURSE "${KC_TEST_ROOT}")
execute_process(
  COMMAND "${KC_EXE}" --project "${KC_TEST_ROOT}" init --quiet
  RESULT_VARIABLE init_status
  ERROR_VARIABLE init_error
)
if(NOT init_status EQUAL 0)
  message(FATAL_ERROR "kc init failed (${init_status}): ${init_error}")
endif()

set(missing_proposal "prp_01J00000000000000000000000")
execute_process(
  COMMAND "${KC_EXE}" --project "${KC_TEST_ROOT}" --json proposal approve
          "${missing_proposal}"
  RESULT_VARIABLE approve_status
  OUTPUT_VARIABLE approve_output
  ERROR_VARIABLE approve_error
)
if(NOT approve_status EQUAL 3)
  message(FATAL_ERROR
    "missing approval should exit 3 (${approve_status}): "
    "${approve_error} ${approve_output}")
endif()
if(NOT approve_output MATCHES "\"command\":\"proposal approve\"")
  message(FATAL_ERROR "approval output used the wrong command: ${approve_output}")
endif()
if(NOT approve_output MATCHES "\"code\":\"proposal_not_found\"")
  message(FATAL_ERROR "approval output omitted proposal_not_found: ${approve_output}")
endif()

execute_process(
  COMMAND "${KC_EXE}" --project "${KC_TEST_ROOT}" --json apply
          "${missing_proposal}"
  RESULT_VARIABLE apply_status
  OUTPUT_VARIABLE apply_output
  ERROR_VARIABLE apply_error
)
if(NOT apply_status EQUAL 3)
  message(FATAL_ERROR
    "missing apply should exit 3 (${apply_status}): "
    "${apply_error} ${apply_output}")
endif()
if(NOT apply_output MATCHES "\"command\":\"apply\"")
  message(FATAL_ERROR "apply output used the wrong command: ${apply_output}")
endif()
if(NOT apply_output MATCHES "\"code\":\"proposal_not_found\"")
  message(FATAL_ERROR "apply output omitted proposal_not_found: ${apply_output}")
endif()
