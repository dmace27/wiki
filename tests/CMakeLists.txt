find_package(Catch2 3 CONFIG REQUIRED)
find_package(nlohmann_json_schema_validator CONFIG REQUIRED)

# Compile all Catch2 test cases into one test executable.
add_executable(kc_tests
  integration/compiler_tests.cpp
  integration/extractor_tests.cpp
  integration/model_runner_tests.cpp
  integration/source_importer_tests.cpp
  integration/vault_writer_tests.cpp
  integration/migration_runner_tests.cpp
  integration/project_initializer_tests.cpp
  unit/domain_json_tests.cpp
  unit/extraction_adapter_tests.cpp
  unit/id_tests.cpp
  unit/proposal_validator_tests.cpp
  unit/schema_fixture_tests.cpp
  unit/sha256_tests.cpp
)

target_link_libraries(
  kc_tests
  PRIVATE
    Catch2::Catch2WithMain
    kc::compiler
    kc::extraction
    kc::import
    kc::models
    kc::storage
    kc::vault
    nlohmann_json_schema_validator::validator
    kc_project_options
)

target_compile_definitions(
  kc_tests
  PRIVATE
    KC_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    KC_TEST_MIGRATIONS_DIR="${PROJECT_SOURCE_DIR}/migrations"
    KC_TEST_SCHEMAS_DIR="${PROJECT_SOURCE_DIR}/schemas"
)

# Turn individual Catch2 TEST_CASE blocks into tests visible to CTest.
include(Catch)
catch_discover_tests(kc_tests)

# Exercise the actual CLI as a subprocess in addition to calling libraries.
add_test(
  NAME kc_cli_init
  COMMAND
    ${CMAKE_COMMAND}
      -DKC_EXE=$<TARGET_FILE:kc>
      -DKC_TEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/cli-init-project
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/kc_init_test.cmake
)

add_test(
  NAME kc_cli_import
  COMMAND
    ${CMAKE_COMMAND}
      -DKC_EXE=$<TARGET_FILE:kc>
      -DKC_TEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/cli-import-project
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/kc_import_test.cmake
)

add_test(
  NAME kc_cli_extract
  COMMAND
    ${CMAKE_COMMAND}
      -DKC_EXE=$<TARGET_FILE:kc>
      -DKC_TEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/cli-extract-project
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/kc_extract_test.cmake
)

add_test(
  NAME kc_cli_compile
  COMMAND
    ${CMAKE_COMMAND}
      -DKC_EXE=$<TARGET_FILE:kc>
      -DKC_TEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/cli-compile-project
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/kc_compile_test.cmake
)

add_test(
  NAME kc_cli_vault
  COMMAND
    ${CMAKE_COMMAND}
      -DKC_EXE=$<TARGET_FILE:kc>
      -DKC_TEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/cli-vault-project
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/kc_vault_test.cmake
)
