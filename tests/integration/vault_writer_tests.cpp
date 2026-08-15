#include "../test_support.hpp"
#include "kc/domain/id.hpp"
#include "kc/import/sha256.hpp"
#include "kc/import/source_importer.hpp"
#include "kc/storage/database.hpp"
#include "kc/storage/project_initializer.hpp"
#include "kc/vault/vault_writer.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

namespace {

kc::storage::InitOptions init_options(const std::filesystem::path &root) {
  return {.project_root = root,
          .vault_path = "vault",
          .migration_directory = KC_TEST_MIGRATIONS_DIR};
}

void write_file(const std::filesystem::path &path,
                const std::string_view content) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << content;
}

std::string read_file(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

/// Quote trusted test data as one SQLite string literal.
std::string sql_string(const std::string_view value) {
  std::string quoted{"'"};
  for (const auto character : value) {
    quoted.push_back(character);
    if (character == '\'') {
      quoted.push_back('\'');
    }
  }
  quoted.push_back('\'');
  return quoted;
}

struct ProposalFixture {
  kc::domain::ProposalId proposal_id;
  kc::domain::PageId page_id;
  kc::source_import::ImportResult imported;
};

nlohmann::json proposal_json(
    const kc::domain::PageId &page_id,
    const std::optional<kc::domain::ArticleId> &article_id = std::nullopt,
    const std::string_view explanation =
        "A Markov chain moves between states.") {
  const auto operation = article_id ? "update_article" : "create_article";
  nlohmann::json article{{"title", "Markov Chains"},
                         {"slug", "markov-chains"},
                         {"aliases", nlohmann::json::array({"Markov chain"})}};
  if (article_id) {
    article["article_id"] = article_id->value;
  }
  const nlohmann::json citation{{"page_id", page_id.value},
                                {"start_char", 2},
                                {"end_char", 14},
                                {"quote", "Markov chain"}};
  return {
      {"schema_version", 1},
      {"operation", operation},
      {"article", std::move(article)},
      {"sections",
       nlohmann::json::array(
           {{{"key", "working_explanation"},
             {"heading", "My working explanation"},
             {"blocks",
              nlohmann::json::array(
                  {{{"kind", "paragraph"},
                    {"text", explanation},
                    {"citations", nlohmann::json::array({citation})}}})}},
            {{"key", "key_ideas"},
             {"heading", "Key ideas"},
             {"blocks",
              nlohmann::json::array(
                  {{{"kind", "bullet"},
                    {"text", "The next state depends on the current state."},
                    {"citations", nlohmann::json::array({citation})}}})}}})},
      {"related_concepts",
       nlohmann::json::array(
           {{{"title", "Conditional Probability"},
             {"reason", "Transitions use conditional probabilities."},
             {"citations", nlohmann::json::array({citation})}}})}};
}

/// Create one immutable PDF page plus the normalized rows a compiler would
/// commit. This isolates writer tests from PDF and model runtimes.
ProposalFixture seed_create_proposal(const std::filesystem::path &project_root,
                                     kc::storage::Database &database) {
  const auto source_path = project_root.parent_path() / "probability-notes.pdf";
  write_file(source_path, "%PDF immutable probability notes");
  kc::source_import::SourceImporter importer(project_root);
  auto imported = importer.import_file(source_path);
  const auto page_id = kc::domain::generate_page_id();
  const auto proposal_id = kc::domain::generate_proposal_id();

  database.execute(
      "INSERT INTO source_pages(page_id, source_version_id, page_number, "
      "text, text_status, text_sha256) VALUES (" +
      sql_string(page_id.value) + "," +
      sql_string(imported.version.source_version_id.value) +
      ",3,'A Markov chain has the Markov property.','reviewed'," +
      sql_string(kc::source_import::sha256_text(
          "A Markov chain has the Markov property.")) +
      ");");
  const auto payload = proposal_json(page_id).dump();
  database.execute(
      "INSERT INTO proposals(proposal_id, operation, payload_json, status, "
      "created_at) VALUES (" +
      sql_string(proposal_id.value) + ",'create_article'," +
      sql_string(payload) + ",'pending','2026-08-11T12:00:00Z');");
  for (const auto &[section, block] :
       {std::pair<std::string_view, int>{"working_explanation", 0},
        {"key_ideas", 0},
        {"related_concepts", 0}}) {
    database.execute(
        "INSERT INTO proposal_citations(proposal_id, section_key, "
        "block_index, page_id, start_char, end_char, quote) VALUES (" +
        sql_string(proposal_id.value) + "," + sql_string(section) + "," +
        std::to_string(block) + "," + sql_string(page_id.value) +
        ",2,14,'Markov chain');");
  }
  return {proposal_id, page_id, std::move(imported)};
}

kc::domain::ProposalId seed_update_proposal(
    kc::storage::Database &database, const kc::domain::PageId &page_id,
    const kc::domain::ArticleId &article_id,
    const std::string_view explanation =
        "An approved update still preserves my surrounding notes.") {
  const auto proposal_id = kc::domain::generate_proposal_id();
  const auto payload = proposal_json(page_id, article_id, explanation).dump();
  database.execute(
      "INSERT INTO proposals(proposal_id, article_id, operation, payload_json, "
      "status, created_at) VALUES (" +
      sql_string(proposal_id.value) + "," + sql_string(article_id.value) +
      ",'update_article'," + sql_string(payload) +
      ",'pending','2026-08-11T12:01:00Z');");
  for (const auto &[section, block] :
       {std::pair<std::string_view, int>{"working_explanation", 0},
        {"key_ideas", 0},
        {"related_concepts", 0}}) {
    database.execute(
        "INSERT INTO proposal_citations(proposal_id, section_key, "
        "block_index, page_id, start_char, end_char, quote) VALUES (" +
        sql_string(proposal_id.value) + "," + sql_string(section) + "," +
        std::to_string(block) + "," + sql_string(page_id.value) +
        ",2,14,'Markov chain');");
  }
  return proposal_id;
}

} // namespace

TEST_CASE(
    "approved proposals render source-linked Markdown and durable state") {
  kc::test::TemporaryDirectory temporary;
  const auto project_root = temporary.path() / "project";
  const auto initialized =
      kc::storage::initialize_project(init_options(project_root));
  kc::storage::Database database(initialized.state_path);
  const auto fixture = seed_create_proposal(project_root, database);
  kc::vault::VaultWriter writer(project_root);

  writer.approve(fixture.proposal_id);
  const auto applied = writer.apply(fixture.proposal_id);

  const auto article_path = project_root / applied.vault_path;
  const auto content = read_file(article_path);
  CHECK(content.find("# Markov Chains") != std::string::npos);
  CHECK(content.find("<!-- kc:managed:start id=\"knowledge-compiler\" -->") !=
        std::string::npos);
  CHECK(content.find("- [[Conditional Probability]]") != std::string::npos);
  CHECK(content.find("[^" + fixture.page_id.value + "]") != std::string::npos);
  CHECK(content.find("#page=3") != std::string::npos);
  CHECK_FALSE(applied.backup_path.has_value());
  CHECK(kc::source_import::sha256_file(article_path) == applied.content_sha256);

  const auto source_directory = project_root / "vault/_sources";
  REQUIRE(std::filesystem::is_directory(source_directory));
  CHECK(std::ranges::distance(
            std::filesystem::directory_iterator(source_directory),
            std::filesystem::directory_iterator{}) == 1);
  CHECK(
      database.scalar_text("SELECT status FROM proposals WHERE proposal_id = " +
                           sql_string(fixture.proposal_id.value)) == "applied");
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM apply_runs") == 1);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM article_citations") == 3);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM article_fts") == 1);
  CHECK(database.scalar_text("SELECT body FROM article_fts") == content);
}

TEST_CASE(
    "updates atomically replace existing articles, preserve outside bytes, "
    "and back up the complete old file") {
  kc::test::TemporaryDirectory temporary;
  const auto project_root = temporary.path() / "project";
  const auto initialized =
      kc::storage::initialize_project(init_options(project_root));
  kc::storage::Database database(initialized.state_path);
  const auto fixture = seed_create_proposal(project_root, database);
  kc::vault::VaultWriter writer(project_root);
  writer.approve(fixture.proposal_id);
  const auto created = writer.apply(fixture.proposal_id);

  const auto article_path = project_root / created.vault_path;
  auto user_edited = read_file(article_path);
  user_edited += "\n## My additions\r\n\r\nKeep these bytes exactly.\r\n";
  write_file(article_path, user_edited);
  const auto article_id = kc::domain::ArticleId{
      database.scalar_text("SELECT article_id FROM articles")};
  const auto update_id =
      seed_update_proposal(database, fixture.page_id, article_id);

  constexpr std::string_view start_marker =
      "<!-- kc:managed:start id=\"knowledge-compiler\" -->";
  constexpr std::string_view end_marker = "<!-- kc:managed:end -->";
  const auto old_start = user_edited.find(start_marker) + start_marker.size();
  const auto old_end = user_edited.find(end_marker, old_start);
  const auto old_prefix = user_edited.substr(0, old_start);
  const auto old_suffix = user_edited.substr(old_end);

  writer.approve(update_id);
  const auto updated = writer.apply(update_id);
  const auto new_content = read_file(article_path);
  const auto new_start = new_content.find(start_marker) + start_marker.size();
  const auto new_end = new_content.find(end_marker, new_start);

  CHECK(new_content.substr(0, new_start) == old_prefix);
  CHECK(new_content.substr(new_end) == old_suffix);
  CHECK(new_content.find("approved update") != std::string::npos);
  REQUIRE(updated.backup_path.has_value());
  CHECK(read_file(project_root / *updated.backup_path) == user_edited);
  CHECK(database.scalar_text("SELECT previous_content_sha256 FROM apply_runs "
                             "ORDER BY rowid DESC LIMIT 1") ==
        kc::source_import::sha256_text(user_edited));
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM article_fts") == 1);
}

TEST_CASE("writer rejects review bypasses and unapproved file collisions") {
  kc::test::TemporaryDirectory temporary;
  const auto project_root = temporary.path() / "project";
  const auto initialized =
      kc::storage::initialize_project(init_options(project_root));
  kc::storage::Database database(initialized.state_path);
  const auto fixture = seed_create_proposal(project_root, database);
  kc::vault::VaultWriter writer(project_root);

  CHECK_THROWS_AS(writer.apply(fixture.proposal_id), kc::vault::VaultError);
  writer.approve(fixture.proposal_id);
  const auto collision = project_root / "vault/Markov Chains.md";
  write_file(collision, "User-owned article with no managed markers.\n");
  CHECK_THROWS_AS(writer.apply(fixture.proposal_id), kc::vault::VaultError);

  CHECK(read_file(collision) ==
        "User-owned article with no managed markers.\n");
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM articles") == 0);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM apply_runs") == 0);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM article_fts") == 0);

  // The dangerous path is possible only through an explicit caller option,
  // and it retains the displaced user file as a recovery backup.
  const auto explicitly_applied =
      writer.apply(fixture.proposal_id, {.allow_overwrite_user_file = true});
  REQUIRE(explicitly_applied.backup_path.has_value());
  CHECK(read_file(project_root / *explicitly_applied.backup_path) ==
        "User-owned article with no managed markers.\n");
  CHECK(read_file(collision).find("# Markov Chains") != std::string::npos);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM articles") == 1);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM apply_runs") == 1);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM article_fts") == 1);
}

TEST_CASE("malformed managed markers abort before backup, audit, or indexing") {
  kc::test::TemporaryDirectory temporary;
  const auto project_root = temporary.path() / "project";
  const auto initialized =
      kc::storage::initialize_project(init_options(project_root));
  kc::storage::Database database(initialized.state_path);
  const auto fixture = seed_create_proposal(project_root, database);
  kc::vault::VaultWriter writer(project_root);
  writer.approve(fixture.proposal_id);
  const auto created = writer.apply(fixture.proposal_id);

  const auto article_path = project_root / created.vault_path;
  auto malformed = read_file(article_path);
  const auto marker = malformed.find("<!-- kc:managed:end -->");
  malformed.erase(marker, std::string("<!-- kc:managed:end -->").size());
  write_file(article_path, malformed);
  const auto update_id =
      seed_update_proposal(database, fixture.page_id, created.article_id);
  writer.approve(update_id);

  CHECK_THROWS_AS(writer.apply(update_id), kc::vault::VaultError);
  CHECK(read_file(article_path) == malformed);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM apply_runs") == 1);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM article_fts") == 1);
  CHECK(
      database.scalar_text("SELECT status FROM proposals WHERE proposal_id = " +
                           sql_string(update_id.value)) == "approved");
}
