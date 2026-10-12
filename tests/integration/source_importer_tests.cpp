#include "kc/import/source_importer.hpp"
#include "kc/storage/database.hpp"
#include "kc/storage/project_initializer.hpp"
#include "../test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

void write_file(const std::filesystem::path& path, const std::string& content) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << content;
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

kc::storage::InitOptions init_options(const std::filesystem::path& root) {
  return {
      .project_root = root,
      .vault_path = "vault",
      .migration_directory = KC_TEST_MIGRATIONS_DIR};
}

}  // namespace

TEST_CASE("source imports are immutable, versioned, and idempotent") {
  kc::test::TemporaryDirectory temporary;
  const auto project_root = temporary.path() / "project";
  const auto original = temporary.path() / "probability-notes.md";
  const auto initialized =
      kc::storage::initialize_project(init_options(project_root));
  write_file(original, "Markov chains, first revision\n");

  kc::source_import::SourceImporter importer(project_root);
  const auto first = importer.import_file(original);
  const auto repeated = importer.import_file(original, {.source_id = first.source.source_id});

  CHECK(first.source_created);
  CHECK(first.version_created);
  CHECK_FALSE(repeated.source_created);
  CHECK_FALSE(repeated.version_created);
  CHECK(repeated.source.source_id == first.source.source_id);
  CHECK(repeated.version.source_version_id == first.version.source_version_id);
  CHECK_FALSE(first.version.stored_path.is_absolute());
  CHECK(read_file(project_root / first.version.stored_path) ==
        "Markov chains, first revision\n");
  const auto retained_permissions =
      std::filesystem::status(project_root / first.version.stored_path).permissions();
  CHECK((retained_permissions & std::filesystem::perms::owner_write) ==
        std::filesystem::perms::none);

  write_file(original, "Markov chains, second revision\n");
  const auto changed = importer.import_file(original, {.source_id = first.source.source_id});

  CHECK_FALSE(changed.source_created);
  CHECK(changed.version_created);
  CHECK(changed.source.source_id == first.source.source_id);
  CHECK(changed.version.source_version_id != first.version.source_version_id);
  CHECK(changed.version.sha256 != first.version.sha256);
  CHECK(read_file(project_root / first.version.stored_path) ==
        "Markov chains, first revision\n");
  CHECK(read_file(project_root / changed.version.stored_path) ==
        "Markov chains, second revision\n");

  std::filesystem::remove(original);
  CHECK(std::filesystem::is_regular_file(project_root / first.version.stored_path));
  CHECK(std::filesystem::is_regular_file(project_root / changed.version.stored_path));

  kc::storage::Database database(initialized.state_path);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM sources") == 1);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM source_versions") == 2);
  const auto stored_path =
      database.scalar_text("SELECT stored_path FROM source_versions ORDER BY imported_at LIMIT 1");
  CHECK_FALSE(std::filesystem::path(stored_path).is_absolute());
  CHECK(stored_path.find(temporary.path().string()) == std::string::npos);
}

TEST_CASE("Markdown, text, and PDF sources are registered with their media types") {
  kc::test::TemporaryDirectory temporary;
  const auto project_root = temporary.path() / "project";
  const auto initialized =
      kc::storage::initialize_project(init_options(project_root));
  const auto markdown = temporary.path() / "notes.markdown";
  const auto text = temporary.path() / "notes.txt";
  const auto pdf = temporary.path() / "notes.PDF";
  write_file(markdown, "# Notes\n");
  write_file(text, "plain notes\n");
  write_file(pdf, "%PDF-1.7 fixture bytes\n");

  kc::source_import::SourceImporter importer(project_root);
  const auto results = importer.import_files({markdown, text, pdf});

  REQUIRE(results.size() == 3);
  REQUIRE(results[0].result);
  REQUIRE(results[1].result);
  REQUIRE(results[2].result);
  CHECK(results[0].result->source.source_kind == kc::domain::SourceKind::markdown);
  CHECK(results[0].result->version.media_type == "text/markdown");
  CHECK(results[1].result->source.source_kind == kc::domain::SourceKind::text);
  CHECK(results[1].result->version.media_type == "text/plain");
  CHECK(results[2].result->source.source_kind == kc::domain::SourceKind::pdf);
  CHECK(results[2].result->version.media_type == "application/pdf");

  kc::storage::Database database(initialized.state_path);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM sources") == 3);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM source_versions") == 3);
}

TEST_CASE("identical bytes reuse storage without merging logical sources") {
  kc::test::TemporaryDirectory temporary;
  const auto project_root = temporary.path() / "project";
  const auto initialized =
      kc::storage::initialize_project(init_options(project_root));
  const auto first_path = temporary.path() / "first.txt";
  const auto second_path = temporary.path() / "second.txt";
  write_file(first_path, "identical bytes\n");
  write_file(second_path, "identical bytes\n");

  kc::source_import::SourceImporter importer(project_root);
  const auto first = importer.import_file(first_path);
  const auto second = importer.import_file(second_path);

  CHECK(first.version_created);
  CHECK(second.source_created);
  CHECK(second.version_created);
  CHECK(second.deduplicated);
  CHECK(second.source.source_id != first.source.source_id);
  CHECK(second.version.source_version_id != first.version.source_version_id);
  CHECK(second.version.stored_path == first.version.stored_path);
  CHECK(second.source.display_name == "second.txt");
  CHECK(second.version.original_filename == "second.txt");

  kc::storage::Database database(initialized.state_path);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM sources") == 2);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM source_versions") == 2);
}

TEST_CASE("unsupported source extensions are rejected without state changes") {
  kc::test::TemporaryDirectory temporary;
  const auto project_root = temporary.path() / "project";
  const auto initialized =
      kc::storage::initialize_project(init_options(project_root));
  const auto unsupported = temporary.path() / "notes.rtf";
  write_file(unsupported, "unsupported\n");

  kc::source_import::SourceImporter importer(project_root);
  CHECK_THROWS_AS(importer.import_file(unsupported),
                  kc::source_import::ImportError);

  kc::storage::Database database(initialized.state_path);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM sources") == 0);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM source_versions") == 0);
}

TEST_CASE("renamed explicit revisions preserve identity and restore historical bytes") {
  kc::test::TemporaryDirectory temporary;
  const auto root = temporary.path() / "project";
  const auto initialized = kc::storage::initialize_project(init_options(root));
  const auto original = temporary.path() / "original.md";
  const auto renamed = temporary.path() / "renamed.markdown";
  write_file(original, "Revision A\n");
  kc::source_import::SourceImporter importer(root);
  const auto a = importer.import_file(original);
  const kc::source_import::ImportOptions options{.source_id = a.source.source_id};

  write_file(renamed, "Revision A\n");
  const auto unchanged = importer.import_file(renamed, options);
  CHECK_FALSE(unchanged.version_created);
  CHECK(unchanged.deduplicated);
  CHECK(unchanged.version.source_version_id == a.version.source_version_id);
  CHECK(unchanged.source.display_name == "original.md");
  CHECK(unchanged.version.original_filename == "original.md");

  write_file(renamed, "Revision B\n");
  const auto b = importer.import_file(renamed, options);
  CHECK(b.version_created);
  CHECK_FALSE(b.source_created);
  CHECK_FALSE(b.deduplicated);
  CHECK(b.version.original_filename == "renamed.markdown");
  CHECK(b.source.display_name == "original.md");

  write_file(renamed, "Revision A\n");
  const auto restored = importer.import_file(renamed, options);
  CHECK(restored.version_created);
  CHECK(restored.restored);
  CHECK(restored.deduplicated);
  CHECK(restored.version.source_version_id != a.version.source_version_id);
  CHECK(restored.version.stored_path == a.version.stored_path);
  const auto repeated = importer.import_file(original, options);
  CHECK_FALSE(repeated.version_created);
  CHECK(repeated.version.source_version_id == restored.version.source_version_id);

  kc::storage::Database database(initialized.state_path);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM sources") == 1);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM source_versions") == 3);
  CHECK(database.scalar_text(
            "SELECT source_version_id FROM source_versions ORDER BY rowid DESC LIMIT 1") ==
        restored.version.source_version_id.value);
  CHECK(read_file(root / b.version.stored_path) == "Revision B\n");
}

TEST_CASE("explicit revisions reject absent archived and incompatible sources") {
  kc::test::TemporaryDirectory temporary;
  const auto root = temporary.path() / "project";
  const auto initialized = kc::storage::initialize_project(init_options(root));
  const auto markdown = temporary.path() / "notes.md";
  const auto text = temporary.path() / "notes.txt";
  write_file(markdown, "same bytes\n");
  write_file(text, "same bytes\n");
  kc::source_import::SourceImporter importer(root);
  const auto source = importer.import_file(markdown);
  CHECK_THROWS_AS(importer.import_file(markdown, {.source_id = kc::domain::SourceId{"src_missing"}}),
                  kc::source_import::ImportError);
  CHECK_THROWS_AS(importer.import_file(text, {.source_id = source.source.source_id}),
                  kc::source_import::ImportError);
  kc::storage::Database database(initialized.state_path);
  database.execute("UPDATE sources SET archived_at = '2026-10-12T12:00:00Z'");
  CHECK_THROWS_AS(importer.import_file(markdown, {.source_id = source.source.source_id}),
                  kc::source_import::ImportError);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM sources") == 1);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM source_versions") == 1);
}

TEST_CASE("batch imports report durable successes around every failed input") {
  kc::test::TemporaryDirectory temporary;
  const auto root = temporary.path() / "project";
  const auto initialized = kc::storage::initialize_project(init_options(root));
  const auto first = temporary.path() / "first.md";
  const auto last = temporary.path() / "last.md";
  const auto missing = temporary.path() / "missing.md";
  const auto unsupported = temporary.path() / "notes.rtf";
  write_file(first, "first\n");
  write_file(last, "last\n");
  write_file(unsupported, "unsupported\n");
  kc::source_import::SourceImporter importer(root);
  const auto results = importer.import_files({first, missing, unsupported, last});
  REQUIRE(results.size() == 4);
  REQUIRE(results[0].result);
  REQUIRE(results[1].failure);
  REQUIRE(results[2].failure);
  REQUIRE(results[3].result);
  CHECK(results[1].input_path == missing);
  CHECK(results[1].failure->kind == kc::source_import::ImportErrorKind::unavailable_input);
  CHECK(results[2].failure->kind == kc::source_import::ImportErrorKind::invalid_input);
  CHECK_FALSE(results[1].result);
  CHECK_FALSE(results[0].failure);
  kc::storage::Database database(initialized.state_path);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM sources") == 2);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM source_versions") == 2);
  CHECK(std::filesystem::is_regular_file(root / results[0].result->version.stored_path));
  CHECK(std::filesystem::is_regular_file(root / results[3].result->version.stored_path));
}

TEST_CASE("batch state failures preserve successes and verify reused retained bytes") {
  kc::test::TemporaryDirectory temporary;
  const auto root = temporary.path() / "project";
  const auto initialized = kc::storage::initialize_project(init_options(root));
  const auto original = temporary.path() / "original.txt";
  const auto first = temporary.path() / "first.txt";
  const auto last = temporary.path() / "last.txt";
  write_file(original, "original bytes\n");
  write_file(first, "first bytes\n");
  write_file(last, "last bytes\n");
  kc::source_import::SourceImporter importer(root);
  const auto source = importer.import_file(original);
  const auto retained = root / source.version.stored_path;
  std::filesystem::permissions(retained, std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::add);
  write_file(retained, "tampered bytes\n");
  const auto results = importer.import_files({first, original, last});
  REQUIRE(results.size() == 3);
  REQUIRE(results[0].result);
  REQUIRE(results[1].failure);
  REQUIRE(results[2].result);
  CHECK(results[1].failure->kind == kc::source_import::ImportErrorKind::state_error);
  CHECK(results[1].failure->message.find("SHA-256") != std::string::npos);
  CHECK_FALSE(results[1].result);
  kc::storage::Database database(initialized.state_path);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM sources") == 3);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM source_versions") == 3);
}

TEST_CASE("legacy projects require the source identity migration before importing") {
  kc::test::TemporaryDirectory temporary;
  const auto root = temporary.path() / "project";
  const auto old_migrations = temporary.path() / "old-migrations";
  std::filesystem::create_directory(old_migrations);
  for (const auto* name : {"001_initial.sql", "002_proposal_review_reason.sql"}) {
    std::filesystem::copy_file(std::filesystem::path(KC_TEST_MIGRATIONS_DIR) / name,
                               old_migrations / name);
  }
  auto options = init_options(root);
  options.migration_directory = old_migrations;
  const auto initialized = kc::storage::initialize_project(options);
  try {
    kc::source_import::SourceImporter legacy(root);
    FAIL("legacy hash uniqueness must not be used with the new identity rules");
  } catch (const kc::source_import::ImportError& error) {
    CHECK(error.kind() == kc::source_import::ImportErrorKind::invalid_project);
    CHECK(std::string(error.what()).find("kc init") != std::string::npos);
  }
  static_cast<void>(kc::storage::initialize_project(init_options(root)));
  const auto input = temporary.path() / "Notes.md";
  write_file(input, "new identity\n");
  kc::source_import::SourceImporter importer(root);
  const auto first = importer.import_file(input);
  const auto second = importer.import_file(input);
  CHECK(first.source.source_id != second.source.source_id);
  CHECK(second.deduplicated);
  kc::storage::Database database(initialized.state_path);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM sources") == 2);
}
