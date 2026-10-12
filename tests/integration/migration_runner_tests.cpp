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

  REQUIRE(first.applied_versions == std::vector<int>{1, 2, 3, 4});
  CHECK(second.applied_versions.empty());
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM schema_migrations;") == 4);
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
  CHECK(upgrade.apply_all().applied_versions == std::vector<int>{2, 3, 4});
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM proposals") == 1);
  CHECK(database.scalar_integer(
            "SELECT COUNT(*) FROM pragma_table_info('proposals') "
            "WHERE name = 'review_reason';") == 1);
}

TEST_CASE("source identity migration preserves historical evidence and latest ordering") {
  kc::test::TemporaryDirectory temporary;
  const auto old_migrations = temporary.path() / "old-migrations";
  std::filesystem::create_directory(old_migrations);
  for (const auto* name : {"001_initial.sql", "002_proposal_review_reason.sql"}) {
    std::filesystem::copy_file(std::filesystem::path(KC_TEST_MIGRATIONS_DIR) / name,
                               old_migrations / name);
  }
  kc::storage::Database database(temporary.path() / "state.sqlite");
  kc::storage::MigrationRunner initial(database, old_migrations);
  REQUIRE(initial.apply_all().applied_versions == std::vector<int>{1, 2});
  // Sparse rowids and reversed timestamps exercise the actual latest selector.
  database.execute(
      "INSERT INTO sources VALUES ('src_old','Notes.md','markdown','later',NULL);"
      "INSERT INTO source_versions VALUES "
      "('sv_old','src_old','hash-A','Notes.md','sources/old.md','text/markdown',1,'later');"
      "INSERT INTO source_versions(rowid,source_version_id,source_id,sha256,"
      "original_filename,stored_path,media_type,byte_size,imported_at) VALUES "
      "(9,'sv_latest','src_old','hash-B','Notes.md','sources/latest.md','text/markdown',1,'earlier');"
      "INSERT INTO source_pages VALUES ('pg_old','sv_old',1,NULL,'A','native','page-hash');"
      "INSERT INTO extraction_runs VALUES ('run_old','sv_old','native',NULL,NULL,'completed','t','t',NULL);"
      "INSERT INTO articles VALUES ('art_old','Example','example','vault/Example.md','hash','t','t');"
      "INSERT INTO proposals(proposal_id,article_id,operation,payload_json,status,created_at) "
      "VALUES ('prp_old','art_old','update_article','{\"immutable\":true}','applied','t');"
      "INSERT INTO proposal_citations VALUES ('prp_old','working_explanation',0,'pg_old',0,1,'A');"
      "INSERT INTO article_citations VALUES ('art_old','working_explanation',0,'pg_old');"
      "INSERT INTO apply_runs VALUES ('apply_old','prp_old','art_old',NULL,'hash',NULL,'t');");

  kc::storage::MigrationRunner upgrade(database, KC_TEST_MIGRATIONS_DIR);
  CHECK(upgrade.apply_all().applied_versions == std::vector<int>{3, 4});
  CHECK(upgrade.apply_all().applied_versions.empty());
  CHECK(database.scalar_integer("PRAGMA foreign_keys") == 1);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM pragma_foreign_key_check") == 0);
  CHECK(database.scalar_integer("SELECT rowid FROM source_versions WHERE source_version_id = 'sv_latest'") == 9);
  CHECK(database.scalar_text("SELECT source_version_id FROM source_versions ORDER BY rowid DESC LIMIT 1") == "sv_latest");
  CHECK(database.scalar_text("SELECT stored_path FROM source_versions WHERE source_version_id = 'sv_old'") == "sources/old.md");
  CHECK(database.scalar_text("SELECT payload_json FROM proposals") == "{\"immutable\":true}");
  CHECK(database.scalar_text("SELECT quote FROM proposal_citations") == "A");
  CHECK(database.scalar_integer("SELECT evidence_revision FROM source_pages") == 0);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM source_pages WHERE image_sha256 IS NULL") == 1);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM proposal_evidence") == 0);
  CHECK(database.scalar_text("SELECT page_id FROM article_citations") == "pg_old");
  CHECK(database.scalar_text("SELECT source_version_id FROM extraction_runs") == "sv_old");
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM apply_runs") == 1);
  // Identical bytes can now be a later activation of the same logical source.
  database.execute("INSERT INTO source_versions VALUES "
                   "('sv_restored','src_old','hash-A','Notes.md','sources/old.md','text/markdown',1,'t')");
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM source_versions") == 3);
  CHECK_THROWS_AS(database.execute("DELETE FROM source_versions WHERE source_version_id = 'sv_old'"),
                  kc::storage::StorageError);
}

TEST_CASE("migration rejects broken provenance and restores foreign-key enforcement") {
  kc::test::TemporaryDirectory temporary;
  const auto migrations_path = temporary.path() / "migrations";
  std::filesystem::create_directory(migrations_path);
  std::filesystem::copy_file(std::filesystem::path(KC_TEST_MIGRATIONS_DIR) / "001_initial.sql",
                             migrations_path / "001_initial.sql");
  kc::storage::Database database(temporary.path() / "state.sqlite");
  kc::storage::MigrationRunner migrations(database, migrations_path);
  static_cast<void>(migrations.apply_all());
  {
    std::ofstream invalid(migrations_path / "002_broken_reference.sql");
    invalid << "INSERT INTO source_pages VALUES ('pg_bad','sv_missing',1,NULL,'A','native','hash');";
  }
  CHECK_THROWS_AS(migrations.apply_all(), kc::storage::StorageError);
  CHECK(database.scalar_integer("PRAGMA foreign_keys") == 1);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM source_pages") == 0);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM schema_migrations") == 1);
}
