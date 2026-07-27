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
  const auto repeated = importer.import_file(original);

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
  const auto changed = importer.import_file(original);

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
  CHECK(results[0].source.source_kind == kc::domain::SourceKind::markdown);
  CHECK(results[0].version.media_type == "text/markdown");
  CHECK(results[1].source.source_kind == kc::domain::SourceKind::text);
  CHECK(results[1].version.media_type == "text/plain");
  CHECK(results[2].source.source_kind == kc::domain::SourceKind::pdf);
  CHECK(results[2].version.media_type == "application/pdf");

  kc::storage::Database database(initialized.state_path);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM sources") == 3);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM source_versions") == 3);
}

TEST_CASE("content deduplication applies across input filenames") {
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
  CHECK_FALSE(second.version_created);
  CHECK(second.source.source_id == first.source.source_id);
  CHECK(second.version.source_version_id == first.version.source_version_id);

  kc::storage::Database database(initialized.state_path);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM sources") == 1);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM source_versions") == 1);
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
