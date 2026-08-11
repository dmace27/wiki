#include "kc/storage/database.hpp"
#include "kc/storage/migration_runner.hpp"
#include "../test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

// Verifies the happy path and the key promise that applied migrations are not
// run a second time.
TEST_CASE("initial migration creates the documented database and is idempotent") {
  kc::test::TemporaryDirectory temporary;
  const auto database_path = temporary.path() / "state.sqlite";
  kc::storage::Database database(database_path);
  kc::storage::MigrationRunner migrations(database, KC_TEST_MIGRATIONS_DIR);

  const auto first = migrations.apply_all();
  const auto second = migrations.apply_all();

  REQUIRE(first.applied_versions == std::vector<int>{1, 2});
  CHECK(second.applied_versions.empty());
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM schema_migrations;") == 2);
  CHECK(database.scalar_integer(
            "SELECT COUNT(*) FROM pragma_table_info('proposals') "
            "WHERE name = 'review_reason';") == 1);
  CHECK(database.scalar_integer(
            "SELECT COUNT(*) FROM sqlite_master WHERE name IN ("
            "'schema_migrations','sources','source_versions','source_pages','extraction_runs',"
            "'model_runs','articles','proposals','proposal_citations','article_citations',"
            "'apply_runs','article_fts');") == 12);
  CHECK(database.scalar_integer("PRAGMA foreign_keys;") == 1);
  CHECK(database.scalar_text("PRAGMA journal_mode;") == "wal");
}

// A deliberately invalid second script proves that each migration is atomic.
TEST_CASE("a failing migration rolls back its partial changes") {
  kc::test::TemporaryDirectory temporary;
  const auto migration_directory = temporary.path() / "migrations";
  std::filesystem::create_directory(migration_directory);
  std::filesystem::copy_file(std::filesystem::path(KC_TEST_MIGRATIONS_DIR) / "001_initial.sql",
                             migration_directory / "001_initial.sql");
  {
    std::ofstream invalid(migration_directory / "002_invalid.sql");
    invalid << "CREATE TABLE should_roll_back(id INTEGER);\n"
               "THIS IS NOT SQL;\n";
  }

  kc::storage::Database database(temporary.path() / "state.sqlite");
  kc::storage::MigrationRunner migrations(database, migration_directory);
  CHECK_THROWS_AS(migrations.apply_all(), kc::storage::StorageError);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM schema_migrations;") == 1);
  CHECK(database.scalar_integer(
            "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='should_roll_back';") == 0);
}

// Existing projects must gain review metadata without rebuilding or losing
// their already-compiled proposal history.
TEST_CASE("proposal review migration upgrades an existing version-one project") {
  kc::test::TemporaryDirectory temporary;
  const auto first_migration_directory = temporary.path() / "first-migration";
  std::filesystem::create_directory(first_migration_directory);
  std::filesystem::copy_file(
      std::filesystem::path(KC_TEST_MIGRATIONS_DIR) / "001_initial.sql",
      first_migration_directory / "001_initial.sql");

  kc::storage::Database database(temporary.path() / "state.sqlite");
  kc::storage::MigrationRunner initial(database, first_migration_directory);
  REQUIRE(initial.apply_all().applied_versions == std::vector<int>{1});
  database.execute(
      "INSERT INTO proposals(proposal_id, operation, payload_json, status, "
      "created_at) VALUES ('prp_existing', 'create_article', '{}', 'pending', "
      "'2026-08-11T12:00:00Z')");

  kc::storage::MigrationRunner upgrade(database, KC_TEST_MIGRATIONS_DIR);
  CHECK(upgrade.apply_all().applied_versions == std::vector<int>{2});
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM proposals") == 1);
  CHECK(database.scalar_integer(
            "SELECT COUNT(*) FROM pragma_table_info('proposals') "
            "WHERE name = 'review_reason';") == 1);
}
