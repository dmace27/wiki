# Exercise the real compile subcommand and its machine-readable validation
# contract without requiring an Ollama runtime. Successful model generation is
# covered through the injectable compiler integration test.
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

execute_process(
  COMMAND "${KC_EXE}" --project "${KC_TEST_ROOT}" --json compile
          --concept "Markov Chains"
  RESULT_VARIABLE compile_status
  OUTPUT_VARIABLE compile_output
  ERROR_VARIABLE compile_error
)
if(NOT compile_status EQUAL 3)
  message(FATAL_ERROR
    "kc compile should report unavailable evidence (${compile_status}): "
    "${compile_error} ${compile_output}")
endif()
if(NOT compile_output MATCHES "\"command\":\"compile\"")
  message(FATAL_ERROR "compile output used the wrong command: ${compile_output}")
endif()
if(NOT compile_output MATCHES "\"code\":\"no_evidence\"")
  message(FATAL_ERROR "compile output omitted no_evidence: ${compile_output}")
endif()
