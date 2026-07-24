find_package(Catch2 3 CONFIG REQUIRED)
find_package(nlohmann_json_schema_validator CONFIG REQUIRED)

# Compile all Catch2 test cases into one test executable.
add_executable(kc_tests
  integration/migration_runner_tests.cpp
  integration/project_initializer_tests.cpp
  unit/domain_json_tests.cpp
  unit/id_tests.cpp
  unit/schema_fixture_tests.cpp
)

target_link_libraries(
  kc_tests
  PRIVATE
    Catch2::Catch2WithMain
    kc::storage
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
