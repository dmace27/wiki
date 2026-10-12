# Reproduce the numeric configuration bugs through the actual CLI. Each failed
# repeated initialization must preserve both invalid user bytes and DB state.
if(NOT DEFINED KC_EXE OR NOT DEFINED KC_TEST_ROOT)
  message(FATAL_ERROR "KC_EXE and KC_TEST_ROOT are required")
endif()
file(REMOVE_RECURSE "${KC_TEST_ROOT}")
execute_process(
  COMMAND "${KC_EXE}" init --project "${KC_TEST_ROOT}" --json --vault vault
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "initial init failed: ${error} ${output}")
endif()
file(READ "${KC_TEST_ROOT}/kc.json" original)
file(SHA256 "${KC_TEST_ROOT}/.knowledge-compiler/state.sqlite" original_state_hash)

foreach(field IN ITEMS schema_version timeout_seconds)
  foreach(token IN ITEMS "1.9" "1.0" "1e0" "-1" "false" "\"1\"" "0" "4294967296" "18446744073709551616")
    if(field STREQUAL "schema_version")
      string(JSON invalid SET "${original}" schema_version "${token}")
    else()
      string(JSON invalid SET "${original}" providers llm ollama timeout_seconds "${token}")
    endif()
    # CMake's JSON serializer can normalize integral floats. Preserve the token
    # exactly by replacing only the serialized numeric field's value.
    string(REGEX REPLACE "\"${field}\"[ \t\r\n]*:[ \t\r\n]*[^,\r\n}]+"
      "\"${field}\": ${token}" invalid "${invalid}")
    file(WRITE "${KC_TEST_ROOT}/kc.json" "${invalid}")
    execute_process(
      COMMAND "${KC_EXE}" --project "${KC_TEST_ROOT}" --json init
      RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error
    )
    if(NOT status EQUAL 2 OR NOT output MATCHES "\"code\":\"invalid_project\"")
      message(FATAL_ERROR "${field}=${token} was not rejected: ${status} ${error} ${output}")
    endif()
    file(READ "${KC_TEST_ROOT}/kc.json" after)
    file(SHA256 "${KC_TEST_ROOT}/.knowledge-compiler/state.sqlite" after_state_hash)
    if(NOT after STREQUAL invalid OR NOT after_state_hash STREQUAL original_state_hash)
      message(FATAL_ERROR "failed init changed configuration or database")
    endif()
  endforeach()
endforeach()
file(WRITE "${KC_TEST_ROOT}/kc.json" "${original}")
execute_process(
  COMMAND "${KC_EXE}" --project "${KC_TEST_ROOT}" --json init
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error
)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "restored valid configuration failed: ${error} ${output}")
endif()
