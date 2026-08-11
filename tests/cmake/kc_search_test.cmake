# Exercise the real search subcommand's routing, JSON contract, project
# discovery, populated-index output, and literal-query validation.
if(NOT DEFINED KC_EXE OR NOT DEFINED KC_SEED_EXE OR
   NOT DEFINED KC_TEST_ROOT)
  message(FATAL_ERROR "KC_EXE, KC_SEED_EXE, and KC_TEST_ROOT are required")
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

execute_process(
  COMMAND "${KC_SEED_EXE}" "${KC_TEST_ROOT}"
  RESULT_VARIABLE seed_status
  ERROR_VARIABLE seed_error
)
if(NOT seed_status EQUAL 0)
  message(FATAL_ERROR "could not seed search fixture (${seed_status}): ${seed_error}")
endif()

execute_process(
  COMMAND "${KC_EXE}" --json search "markov chains"
  WORKING_DIRECTORY "${KC_TEST_ROOT}/work"
  RESULT_VARIABLE search_status
  OUTPUT_VARIABLE search_output
  ERROR_VARIABLE search_error
)
if(NOT search_status EQUAL 0)
  message(FATAL_ERROR "kc search failed (${search_status}): ${search_error}")
endif()
string(FIND "${search_output}" "\"command\":\"search\"" command_position)
string(FIND "${search_output}" "\"title\":\"Markov Chains\"" title_position)
string(FIND "${search_output}" "\"vault_path\":\"vault/Markov Chains.md\"" path_position)
if(command_position EQUAL -1 OR title_position EQUAL -1 OR path_position EQUAL -1)
  message(FATAL_ERROR "search output violated its JSON contract: ${search_output}")
endif()

execute_process(
  COMMAND "${KC_EXE}" --project "${KC_TEST_ROOT}" --json search "---"
  RESULT_VARIABLE invalid_status
  OUTPUT_VARIABLE invalid_output
  ERROR_VARIABLE invalid_error
)
if(NOT invalid_status EQUAL 2 OR
   NOT invalid_output MATCHES "\"code\":\"invalid_query\"")
  message(FATAL_ERROR
    "invalid search used the wrong contract (${invalid_status}): "
    "${invalid_error} ${invalid_output}")
endif()
