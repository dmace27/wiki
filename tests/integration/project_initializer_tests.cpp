#include "kc/domain/json.hpp"
#include "kc/storage/project_initializer.hpp"
#include "../test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

// Initialization should create the full local layout and be safe to repeat.
TEST_CASE("project initialization is offline and idempotent") {
  kc::test::TemporaryDirectory temporary;
  const kc::storage::InitOptions options{
      .project_root = temporary.path() / "project",
      .vault_path = "notes",
      .migration_directory = KC_TEST_MIGRATIONS_DIR};

  const auto first = kc::storage::initialize_project(options);
  const auto second = kc::storage::initialize_project(options);

  CHECK(first.created);
  CHECK_FALSE(second.created);
  CHECK(first.config.project_id == second.config.project_id);
  CHECK(first.applied_migrations == std::vector<int>{1, 2});
  CHECK(second.applied_migrations.empty());
  CHECK(std::filesystem::is_regular_file(first.config_path));
  CHECK(std::filesystem::is_regular_file(first.state_path));
  CHECK(std::filesystem::is_directory(options.project_root / "sources"));
  CHECK(std::filesystem::is_directory(options.project_root / "notes"));
  CHECK(std::filesystem::is_directory(options.project_root / ".knowledge-compiler/pages"));
}

// Project-relative paths prevent initialization from writing outside its root.
TEST_CASE("project initialization rejects paths outside the project") {
  kc::test::TemporaryDirectory temporary;
  const kc::storage::InitOptions options{
      .project_root = temporary.path() / "project",
      .vault_path = "../outside",
      .migration_directory = KC_TEST_MIGRATIONS_DIR};

  CHECK_THROWS_AS(kc::storage::initialize_project(options), kc::storage::StorageError);
}
