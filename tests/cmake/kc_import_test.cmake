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

string(JSON source_id GET "${first_output}" result sources 0 source_id)
string(JSON first_version GET "${first_output}" result sources 0 source_version_id)

execute_process(
  COMMAND "${KC_EXE}" --project "${KC_TEST_ROOT}" --json import --source "${source_id}" "${source_file}"
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
  COMMAND "${KC_EXE}" --project "${KC_TEST_ROOT}" --json import --source "${source_id}" "${source_file}"
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

# Returning to historical bytes creates a fresh latest revision, then becomes
# idempotent. The two distinct snapshots still occupy only two retained files.
file(WRITE "${source_file}" "first revision\n")
execute_process(
  COMMAND "${KC_EXE}" --project "${KC_TEST_ROOT}" --json import --source "${source_id}" "${source_file}"
  RESULT_VARIABLE restored_status OUTPUT_VARIABLE restored_output
)
string(JSON restored GET "${restored_output}" result sources 0 restored)
string(JSON restored_version GET "${restored_output}" result sources 0 source_version_id)
if(NOT restored_status EQUAL 0 OR NOT restored OR restored_version STREQUAL first_version)
  message(FATAL_ERROR "historical restoration failed: ${restored_output}")
endif()
execute_process(
  COMMAND "${KC_EXE}" --project "${KC_TEST_ROOT}" --json import --source "${source_id}" "${source_file}"
  RESULT_VARIABLE restored_repeat_status OUTPUT_VARIABLE restored_repeat_output
)
string(JSON repeated_version GET "${restored_repeat_output}" result sources 0 source_version_id)
if(NOT restored_repeat_status EQUAL 0 OR NOT repeated_version STREQUAL restored_version)
  message(FATAL_ERROR "restored revision was not idempotent: ${restored_repeat_output}")
endif()

# Batch results preserve argument order, report earlier durable successes, and
# continue after a failed middle input.
file(MAKE_DIRECTORY "${KC_TEST_ROOT}/semester-a" "${KC_TEST_ROOT}/semester-b")
set(semester_a "${KC_TEST_ROOT}/semester-a/Notes.md")
set(semester_b "${KC_TEST_ROOT}/semester-b/Notes.md")
set(missing "${KC_TEST_ROOT}/missing.md")
file(WRITE "${semester_a}" "Semester A\n")
file(WRITE "${semester_b}" "Semester B\n")
execute_process(
  COMMAND "${KC_EXE}" --project "${KC_TEST_ROOT}" --json import "${semester_a}" "${missing}" "${semester_b}"
  RESULT_VARIABLE batch_status OUTPUT_VARIABLE batch_output ERROR_VARIABLE batch_error
)
string(JSON batch_ok GET "${batch_output}" ok)
string(JSON partial GET "${batch_output}" result partial_success)
string(JSON count LENGTH "${batch_output}" result files)
string(JSON success_count LENGTH "${batch_output}" result sources)
string(JSON a_id GET "${batch_output}" result files 0 result source_id)
string(JSON b_id GET "${batch_output}" result files 2 result source_id)
string(JSON failure_code GET "${batch_output}" result files 1 error code)
if(NOT batch_status EQUAL 3 OR batch_ok OR NOT partial OR NOT count EQUAL 3
   OR NOT success_count EQUAL 2 OR a_id STREQUAL b_id OR NOT failure_code STREQUAL "source_unavailable"
   OR NOT batch_error STREQUAL "")
  message(FATAL_ERROR "batch result lost successes or identities: ${batch_output} ${batch_error}")
endif()

# Default imports of identical bytes create separate identities and share bytes.
execute_process(
  COMMAND "${KC_EXE}" --project "${KC_TEST_ROOT}" --json import "${semester_a}"
  RESULT_VARIABLE same_status OUTPUT_VARIABLE same_output
)
string(JSON same_id GET "${same_output}" result sources 0 source_id)
string(JSON reused GET "${same_output}" result sources 0 deduplicated)
string(JSON created GET "${same_output}" result sources 0 source_created)
if(NOT same_status EQUAL 0 OR same_id STREQUAL a_id OR NOT reused OR NOT created)
  message(FATAL_ERROR "default import merged identities: ${same_output}")
endif()

# Explicit revisions cannot target an ambiguous multi-file batch.
execute_process(
  COMMAND "${KC_EXE}" --project "${KC_TEST_ROOT}" --json import --source "${a_id}" "${semester_a}" "${semester_b}"
  RESULT_VARIABLE ambiguous_status OUTPUT_VARIABLE ambiguous_output
)
string(JSON ambiguous_ok GET "${ambiguous_output}" ok)
if(NOT ambiguous_status EQUAL 2 OR ambiguous_ok)
  message(FATAL_ERROR "multi-file revision was accepted: ${ambiguous_output}")
endif()

# Human output also preserves successful IDs alongside failed-file diagnostics.
execute_process(
  COMMAND "${KC_EXE}" --project "${KC_TEST_ROOT}" import "${semester_a}" "${missing}" "${semester_b}"
  RESULT_VARIABLE human_status OUTPUT_VARIABLE human_output ERROR_VARIABLE human_error
)
if(NOT human_status EQUAL 3 OR NOT human_output MATCHES "New source.*src_"
   OR NOT human_error MATCHES "missing.md")
  message(FATAL_ERROR "human batch output lost results: ${human_output} ${human_error}")
endif()

# All-file failure still returns an ordered machine-readable batch result.
execute_process(
  COMMAND "${KC_EXE}" --project "${KC_TEST_ROOT}" --json import "${missing}" "${KC_TEST_ROOT}/unsupported.rtf"
  RESULT_VARIABLE failed_status OUTPUT_VARIABLE failed_output
)
string(JSON failed_ok GET "${failed_output}" ok)
string(JSON failed_partial GET "${failed_output}" result partial_success)
string(JSON failed_sources LENGTH "${failed_output}" result sources)
string(JSON failed_code GET "${failed_output}" error code)
if(NOT failed_status EQUAL 3 OR failed_ok OR failed_partial OR NOT failed_sources EQUAL 0
   OR NOT failed_code STREQUAL "import_failed")
  message(FATAL_ERROR "all-failed batch did not report failures: ${failed_output}")
endif()

# An explicitly empty revision identity must fail rather than create a source.
execute_process(
  COMMAND "${KC_EXE}" --project "${KC_TEST_ROOT}" --json import --source "" "${semester_a}"
  RESULT_VARIABLE empty_status OUTPUT_VARIABLE empty_output
)
string(JSON empty_ok GET "${empty_output}" ok)
if(NOT empty_status EQUAL 2 OR empty_ok)
  message(FATAL_ERROR "empty revision source created a new source: ${empty_output}")
endif()
