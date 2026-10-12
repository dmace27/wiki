#include "../test_support.hpp"
#include "kc/domain/id.hpp"
#include "kc/review/review_service.hpp"
#include "kc/search/search_index.hpp"
#include "kc/import/sha256.hpp"
#include "kc/import/source_importer.hpp"
#include "kc/storage/database.hpp"
#include "kc/storage/project_initializer.hpp"
#include "kc/vault/vault_writer.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>
#include <catch2/generators/catch_generators.hpp>
#include "kc/domain/concept.hpp"
#include "kc/domain/path.hpp"

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

TEST_CASE("legacy case-only update proposals retain canonical title filename and search") {
  kc::test::TemporaryDirectory temporary;
  const auto project_root = temporary.path() / "project";
  const auto initialized = kc::storage::initialize_project(init_options(project_root));
  kc::storage::Database database(initialized.state_path);
  const auto fixture = seed_create_proposal(project_root, database);
  kc::vault::VaultWriter writer(project_root);
  writer.approve(fixture.proposal_id);
  const auto first = writer.apply(fixture.proposal_id);
  const auto update = seed_update_proposal(database, fixture.page_id, first.article_id);
  database.execute("UPDATE proposals SET payload_json = json_set(payload_json, "
                   "'$.article.title', 'markov chains') WHERE proposal_id = " + sql_string(update.value));
  const auto immutable_payload = database.scalar_text(
      "SELECT payload_json FROM proposals WHERE proposal_id = " + sql_string(update.value));
  kc::review::ReviewService review(project_root);
  CHECK(review.review_proposal(update).proposal.article.title == "Markov Chains");
  writer.approve(update);
  const auto second = writer.apply(update);
  CHECK(second.article_id == first.article_id);
  CHECK(second.vault_path == first.vault_path);
  CHECK(database.scalar_text("SELECT title FROM article_fts") == "Markov Chains");
  CHECK(database.scalar_text("SELECT payload_json FROM proposals WHERE proposal_id = " +
                            sql_string(update.value)) == immutable_payload);
  kc::search::SearchIndex search(project_root);
  const auto results = search.search("MARKOV CHAINS");
  REQUIRE(results.size() == 1U);
  CHECK(results.front().title == "Markov Chains");
}

TEST_CASE("approval and apply reject stale create identity before writing") {
  kc::test::TemporaryDirectory temporary;
  const auto project_root = temporary.path() / "project";
  const auto initialized = kc::storage::initialize_project(init_options(project_root));
  kc::storage::Database database(initialized.state_path);
  const auto fixture = seed_create_proposal(project_root, database);
  kc::vault::VaultWriter writer(project_root);
  writer.approve(fixture.proposal_id);
  // Another process claims a distinct concept with this slug after approval.
  database.execute("INSERT INTO articles(article_id,title,slug,vault_path,created_at,updated_at) "
                   "VALUES ('art_01J00000000000000000000000','Markov-Chains','markov-chains',"
                   "'vault/Markov-Chains.md','t','t')");
  CHECK_THROWS_AS(writer.apply(fixture.proposal_id), kc::vault::VaultError);
  CHECK(std::filesystem::is_empty(project_root / "vault"));
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM apply_runs") == 0);
  CHECK(database.scalar_text("SELECT status FROM proposals") == "approved");
  database.execute("UPDATE proposals SET status = 'pending'");
  CHECK_THROWS_AS(writer.approve(fixture.proposal_id), kc::vault::VaultError);
  CHECK(database.scalar_text("SELECT status FROM proposals") == "pending");
}

TEST_CASE("apply cannot overwrite a different tracked destination with an explicit flag") {
  kc::test::TemporaryDirectory temporary;
  const auto project_root = temporary.path() / "project";
  const auto initialized = kc::storage::initialize_project(init_options(project_root));
  kc::storage::Database database(initialized.state_path);
  const auto fixture = seed_create_proposal(project_root, database);
  kc::vault::VaultWriter writer(project_root);
  writer.approve(fixture.proposal_id);
  write_file(project_root / "vault" / "Markov Chains.md", "User notes belong to a different article.\n");
  database.execute("INSERT INTO articles(article_id,title,slug,vault_path,created_at,updated_at) "
                   "VALUES ('art_01J00000000000000000000000','Other Topic','other-topic',"
                   "'vault/Markov Chains.md','t','t')");
  CHECK_THROWS_AS(writer.apply(fixture.proposal_id, {.allow_overwrite_user_file = true}),
                  kc::vault::VaultError);
  CHECK(read_file(project_root / "vault" / "Markov Chains.md") ==
        "User notes belong to a different article.\n");
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM apply_runs") == 0);
}

TEST_CASE("apply refuses portable aliases of an untracked filename") {
  kc::test::TemporaryDirectory temporary;
  const auto project_root = temporary.path() / "project";
  const auto initialized = kc::storage::initialize_project(init_options(project_root));
  kc::storage::Database database(initialized.state_path);
  const auto fixture = seed_create_proposal(project_root, database);
  kc::vault::VaultWriter writer(project_root);
  writer.approve(fixture.proposal_id);
  const auto alias = project_root / "vault" / "Markov Chains.md.";
  write_file(alias, "Keep this user file.\n");
  const auto destination = project_root / "vault" / "Markov Chains.md";
  // Windows may resolve the dotted alias to the destination already. Such an
  // untracked file still requires explicit approval. Hosts that retain both
  // names must reject the portable alias even with an overwrite flag.
  const bool aliases_physically = std::filesystem::exists(destination);
  CHECK_THROWS_AS(writer.apply(fixture.proposal_id,
                              {.allow_overwrite_user_file = !aliases_physically}),
                  kc::vault::VaultError);
  CHECK(read_file(alias) == "Keep this user file.\n");
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM apply_runs") == 0);
}

TEST_CASE("generated YAML scalar-looking titles and aliases remain strings") {
  const std::string title = GENERATE("true", "null", "123", "1e3", "yes", "2026-10-12", "生物学", "C++");
  kc::test::TemporaryDirectory temporary;
  const auto root = temporary.path() / "project";
  const auto initialized = kc::storage::initialize_project(init_options(root));
  kc::storage::Database database(initialized.state_path);
  const auto fixture = seed_create_proposal(root, database);
  auto payload = proposal_json(fixture.page_id);
  payload["article"]["title"] = title;
  payload["article"]["slug"] = kc::domain::canonical_concept_slug(title);
  const std::vector<std::string> aliases{"true", "null", "123", "yes", "1.5", ".nan",
                                        "2026-10-12", "引用 \"quotes\" \\ slash", "光合成"};
  payload["article"]["aliases"] = aliases;
  database.execute("UPDATE proposals SET payload_json = " + sql_string(payload.dump()));
  kc::vault::VaultWriter writer(root);
  writer.approve(fixture.proposal_id);
  const auto created = writer.apply(fixture.proposal_id);
  const auto content = read_file(root / created.vault_path);
  const auto metadata = YAML::Load(content.substr(4, content.find("\n---\n", 4) - 4));
  CHECK(metadata["title"].Scalar() == title);
  // A non-specific ! tag denotes a quoted scalar, rather than an implicitly
  // typed plain scalar. Scalar() alone would not prove the YAML type.
  CHECK(metadata["title"].Tag() == "!");
  REQUIRE(metadata["aliases"].IsSequence());
  REQUIRE(metadata["aliases"].size() == aliases.size());
  for (std::size_t index = 0; index < aliases.size(); ++index) {
    CHECK(metadata["aliases"][index].Scalar() == aliases[index]);
    CHECK(metadata["aliases"][index].Tag() == "!");
  }
  CHECK_FALSE(metadata["tags"]);
}

TEST_CASE("empty aliases are an actual YAML sequence and legacy null aliases stay user-owned") {
  kc::test::TemporaryDirectory temporary;
  const auto root = temporary.path() / "project";
  const auto initialized = kc::storage::initialize_project(init_options(root));
  kc::storage::Database database(initialized.state_path);
  const auto fixture = seed_create_proposal(root, database);
  auto payload = proposal_json(fixture.page_id);
  payload["article"]["aliases"] = nlohmann::json::array();
  database.execute("UPDATE proposals SET payload_json = " + sql_string(payload.dump()));
  kc::vault::VaultWriter writer(root);
  writer.approve(fixture.proposal_id);
  const auto created = writer.apply(fixture.proposal_id);
  const auto path = root / created.vault_path;
  auto content = read_file(path);
  auto metadata = YAML::Load(content.substr(4, content.find("\n---\n", 4) - 4));
  REQUIRE(metadata["aliases"].IsSequence());
  CHECK(metadata["aliases"].size() == 0U);
  content.replace(content.find("aliases: []"), std::string("aliases: []").size(), "aliases:");
  write_file(path, content);
  const auto update = seed_update_proposal(database, fixture.page_id, created.article_id);
  writer.approve(update);
  static_cast<void>(writer.apply(update));
  CHECK(read_file(path).starts_with(content.substr(0, content.find("<!-- kc:managed:start"))));
  CHECK(database.scalar_text("SELECT aliases FROM article_fts").empty());
}

TEST_CASE("invalid user frontmatter aborts updates before files backups and FTS change") {
  const std::string aliases = GENERATE("aliases: [true]", "aliases: [123]", "aliases: [null]",
                                      "aliases: [2026-10-12]", "aliases: wrong",
                                      "aliases: [{nested: value}]", "aliases: [unclosed",
                                      "aliases: []\naliases: [duplicate]", "aliases: []\n<<: {title: fake}");
  kc::test::TemporaryDirectory temporary;
  const auto root = temporary.path() / "project";
  const auto initialized = kc::storage::initialize_project(init_options(root));
  kc::storage::Database database(initialized.state_path);
  const auto fixture = seed_create_proposal(root, database);
  kc::vault::VaultWriter writer(root);
  writer.approve(fixture.proposal_id);
  const auto created = writer.apply(fixture.proposal_id);
  const auto path = root / created.vault_path;
  auto content = read_file(path);
  const auto start = content.find("aliases:");
  content.replace(start, content.find("\n---\n", start) - start, aliases);
  write_file(path, content);
  const auto old_index = database.scalar_text("SELECT body FROM article_fts");
  const auto update = seed_update_proposal(database, fixture.page_id, created.article_id);
  writer.approve(update);
  CHECK_THROWS_AS(writer.apply(update), kc::vault::VaultError);
  CHECK(read_file(path) == content);
  CHECK(database.scalar_text("SELECT body FROM article_fts") == old_index);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM apply_runs") == 1);
  CHECK(std::filesystem::is_empty(root / ".knowledge-compiler/backups"));
}

TEST_CASE("approval and review refuse unsafe stored rendering fields") {
  const std::string field = GENERATE("heading", "related", "marker");
  kc::test::TemporaryDirectory temporary;
  const auto root = temporary.path() / "project";
  const auto initialized = kc::storage::initialize_project(init_options(root));
  kc::storage::Database database(initialized.state_path);
  const auto fixture = seed_create_proposal(root, database);
  auto payload = proposal_json(fixture.page_id);
  if (field == "heading") payload["sections"][0]["heading"] = "Heading\nSources";
  if (field == "related") payload["related_concepts"][0]["title"] = "Page]] ![[Embed";
  if (field == "marker") payload["sections"][0]["blocks"][0]["text"] = "<!-- kc:managed:end -->";
  database.execute("UPDATE proposals SET payload_json = " + sql_string(payload.dump()));
  kc::vault::VaultWriter writer(root);
  kc::review::ReviewService review(root);
  CHECK_THROWS_AS(review.review_proposal(fixture.proposal_id), kc::review::ReviewError);
  CHECK_THROWS_AS(writer.approve(fixture.proposal_id), kc::vault::VaultError);
  CHECK(database.scalar_text("SELECT status FROM proposals") == "pending");
  // Recheck previously approved legacy or manually modified state at apply.
  database.execute("UPDATE proposals SET status = 'approved'");
  CHECK_THROWS_AS(writer.apply(fixture.proposal_id), kc::vault::VaultError);
  CHECK(std::filesystem::is_empty(root / "vault"));
}

TEST_CASE("rendered prose and hostile source labels cannot forge links footnotes or embeds") {
  kc::test::TemporaryDirectory temporary;
  const auto root = temporary.path() / "project";
  const auto initialized = kc::storage::initialize_project(init_options(root));
  kc::storage::Database database(initialized.state_path);
  const auto fixture = seed_create_proposal(root, database);
  auto config = kc::test::read_json(root / "kc.json");
  config["vault"]["source_directory"] = "_sources/資料 (notes)";
  write_file(root / "kc.json", config.dump());
  const std::string hostile = "[Fake](https://evil.test) <img src=x> ![[embed]]\n[^fake]: injected\n    ```code```";
  auto payload = proposal_json(fixture.page_id, std::nullopt, hostile);
  payload["sections"][0]["heading"] = "[Heading](https://evil.test)";
  payload["related_concepts"][0]["reason"] = hostile;
  database.execute("UPDATE proposals SET payload_json = " + sql_string(payload.dump()));
  database.execute("UPDATE sources SET display_name = " + sql_string(
      "Notes](https://evil.test) <img src=x>\n[^fake]: injected"));
  kc::vault::VaultWriter writer(root);
  writer.approve(fixture.proposal_id);
  const auto applied = writer.apply(fixture.proposal_id);
  const auto content = read_file(root / applied.vault_path);
  CHECK(content.find("[Fake](") == std::string::npos);
  CHECK(content.find("https://evil.test") == std::string::npos);
  CHECK(content.find("\\<img") != std::string::npos);
  CHECK(content.find("![[embed]]") == std::string::npos);
  CHECK(content.find("\n[^fake]:") == std::string::npos);
  CHECK(content.find("```code```") == std::string::npos);
  CHECK(content.find("_sources/%E8%B3%87%E6%96%99%20%28notes%29/") != std::string::npos);
  CHECK(content.find("#page=3") != std::string::npos);
  CHECK(content.find("[\\^fake\\]\\: injected") != std::string::npos);
  CHECK(std::filesystem::is_directory(root / "vault" / kc::domain::path_from_utf8("_sources/資料 (notes)")));
}

TEST_CASE("final articles reject extra ownership markers and shadowed source footnotes") {
  const bool marker = GENERATE(false, true);
  kc::test::TemporaryDirectory temporary;
  const auto root = temporary.path() / "project";
  const auto initialized = kc::storage::initialize_project(init_options(root));
  kc::storage::Database database(initialized.state_path);
  const auto fixture = seed_create_proposal(root, database);
  kc::vault::VaultWriter writer(root);
  writer.approve(fixture.proposal_id);
  const auto created = writer.apply(fixture.proposal_id);
  const auto path = root / created.vault_path;
  const auto content = read_file(path) + (marker
      ? "\n<!-- kc:managed:start id=\"other\" -->\n"
      : "\n[^" + fixture.page_id.value + "]: [False source](wrong.pdf)\n");
  write_file(path, content);
  const auto update = seed_update_proposal(database, fixture.page_id, created.article_id);
  writer.approve(update);
  CHECK_THROWS_AS(writer.apply(update), kc::vault::VaultError);
  CHECK(read_file(path) == content);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM apply_runs") == 1);
}
