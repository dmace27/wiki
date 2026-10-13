#include "../test_support.hpp"
#include "../contract_cases.hpp"
#include "kc/domain/id.hpp"
#include "kc/review/review_service.hpp"
#include "kc/search/search_index.hpp"
#include "kc/import/sha256.hpp"
#include "kc/updates/article_updates.hpp"
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
  // These writer fixtures deliberately exercise explicit replacement. The
  // production compiler captures the same base before model generation.
  database.execute("INSERT INTO proposal_update_bases(proposal_id, base_apply_run_id, managed_sha256, update_mode) "
      "SELECT " + sql_string(proposal_id.value) + ",apply_run_id,managed_sha256,'regenerate' "
      "FROM apply_runs WHERE article_id = " + sql_string(article_id.value) + " ORDER BY rowid DESC LIMIT 1");
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

  kc::test::approve_preview(writer, fixture.proposal_id);
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
  kc::test::approve_preview(writer, fixture.proposal_id);
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

  kc::test::approve_preview(writer, update_id);
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
  kc::test::approve_preview(writer, fixture.proposal_id);
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
  kc::review::ReviewService(project_root).revoke_approval(fixture.proposal_id);
  kc::test::approve_preview(writer, fixture.proposal_id, {.allow_overwrite_user_file = true});
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
  kc::test::approve_preview(writer, fixture.proposal_id);
  const auto created = writer.apply(fixture.proposal_id);

  const auto article_path = project_root / created.vault_path;
  auto malformed = read_file(article_path);
  const auto marker = malformed.find("<!-- kc:managed:end -->");
  malformed.erase(marker, std::string("<!-- kc:managed:end -->").size());
  write_file(article_path, malformed);
  const auto update_id =
      seed_update_proposal(database, fixture.page_id, created.article_id);
  CHECK_THROWS_AS(kc::test::approve_preview(writer, update_id), kc::vault::VaultError);
  // Even a previously approved/corrupt legacy status cannot bypass the check.
  database.execute("UPDATE proposals SET status = 'approved' WHERE proposal_id = " + sql_string(update_id.value));
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
  kc::test::approve_preview(writer, fixture.proposal_id);
  const auto first = writer.apply(fixture.proposal_id);
  const auto update = seed_update_proposal(database, fixture.page_id, first.article_id);
  database.execute("UPDATE proposals SET payload_json = json_set(payload_json, "
                   "'$.article.title', 'markov chains') WHERE proposal_id = " + sql_string(update.value));
  const auto immutable_payload = database.scalar_text(
      "SELECT payload_json FROM proposals WHERE proposal_id = " + sql_string(update.value));
  kc::review::ReviewService review(project_root);
  CHECK(review.review_proposal(update).proposal.article.title == "Markov Chains");
  kc::test::approve_preview(writer, update);
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
  kc::test::approve_preview(writer, fixture.proposal_id);
  // Another process claims a distinct concept with this slug after approval.
  database.execute("INSERT INTO articles(article_id,title,slug,vault_path,created_at,updated_at) "
                   "VALUES ('art_01J00000000000000000000000','Markov-Chains','markov-chains',"
                   "'vault/Markov-Chains.md','t','t')");
  CHECK_THROWS_AS(writer.apply(fixture.proposal_id), kc::vault::VaultError);
  CHECK(std::filesystem::is_empty(project_root / "vault"));
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM apply_runs") == 0);
  CHECK(database.scalar_text("SELECT status FROM proposals") == "approved");
  database.execute("UPDATE proposals SET status = 'pending'");
  CHECK_THROWS_AS(kc::test::approve_preview(writer, fixture.proposal_id), kc::vault::VaultError);
  CHECK(database.scalar_text("SELECT status FROM proposals") == "pending");
}

TEST_CASE("apply cannot overwrite a different tracked destination with an explicit flag") {
  kc::test::TemporaryDirectory temporary;
  const auto project_root = temporary.path() / "project";
  const auto initialized = kc::storage::initialize_project(init_options(project_root));
  kc::storage::Database database(initialized.state_path);
  const auto fixture = seed_create_proposal(project_root, database);
  kc::vault::VaultWriter writer(project_root);
  kc::test::approve_preview(writer, fixture.proposal_id);
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
  kc::test::approve_preview(writer, fixture.proposal_id);
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
  kc::test::approve_preview(writer, fixture.proposal_id);
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
  kc::test::approve_preview(writer, fixture.proposal_id);
  const auto created = writer.apply(fixture.proposal_id);
  const auto path = root / created.vault_path;
  auto content = read_file(path);
  auto metadata = YAML::Load(content.substr(4, content.find("\n---\n", 4) - 4));
  REQUIRE(metadata["aliases"].IsSequence());
  CHECK(metadata["aliases"].size() == 0U);
  content.replace(content.find("aliases: []"), std::string("aliases: []").size(), "aliases:");
  write_file(path, content);
  const auto update = seed_update_proposal(database, fixture.page_id, created.article_id);
  kc::test::approve_preview(writer, update);
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
  kc::test::approve_preview(writer, fixture.proposal_id);
  const auto created = writer.apply(fixture.proposal_id);
  const auto path = root / created.vault_path;
  auto content = read_file(path);
  const auto start = content.find("aliases:");
  content.replace(start, content.find("\n---\n", start) - start, aliases);
  write_file(path, content);
  const auto old_index = database.scalar_text("SELECT body FROM article_fts");
  const auto update = seed_update_proposal(database, fixture.page_id, created.article_id);
  CHECK_FALSE(writer.preview(update).valid);
  CHECK_THROWS_AS(kc::test::approve_preview(writer, update), kc::vault::VaultError);
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
  CHECK_THROWS_AS(kc::test::approve_preview(writer, fixture.proposal_id), kc::vault::VaultError);
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
  kc::test::approve_preview(writer, fixture.proposal_id);
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
  kc::test::approve_preview(writer, fixture.proposal_id);
  const auto created = writer.apply(fixture.proposal_id);
  const auto path = root / created.vault_path;
  const auto content = read_file(path) + (marker
      ? "\n<!-- kc:managed:start id=\"other\" -->\n"
      : "\n[^" + fixture.page_id.value + "]: [False source](wrong.pdf)\n");
  write_file(path, content);
  const auto update = seed_update_proposal(database, fixture.page_id, created.article_id);
  CHECK_FALSE(writer.preview(update).valid);
  CHECK_THROWS_AS(kc::test::approve_preview(writer, update), kc::vault::VaultError);
  CHECK_THROWS_AS(writer.apply(update), kc::vault::VaultError);
  CHECK(read_file(path) == content);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM apply_runs") == 1);
}

TEST_CASE("common invalid JSON contracts cannot bypass review approval or apply") {
  kc::test::TemporaryDirectory temporary;
  const auto root = temporary.path() / "project";
  const auto initialized = kc::storage::initialize_project(init_options(root));
  kc::storage::Database database(initialized.state_path);
  const auto seeded = seed_create_proposal(root, database);
  kc::vault::VaultWriter writer(root);
  kc::review::ReviewService review(root);
  for (const auto& fixture : kc::test::contract_cases()) {
    if (kc::test::case_contract(fixture) != kc::domain::JsonContract::article_proposal ||
        fixture.at("runtime_valid").get<bool>()) {
      continue;
    }
    CAPTURE(fixture.at("name"));
    const auto document = kc::test::case_document(fixture);
    database.execute("UPDATE proposals SET status = 'pending', payload_json = " + sql_string(document.dump()));
    CHECK_THROWS_AS(review.review_proposal(seeded.proposal_id), kc::review::ReviewError);
    CHECK_THROWS_AS(kc::test::approve_preview(writer, seeded.proposal_id), kc::vault::VaultError);
    CHECK(database.scalar_text("SELECT status FROM proposals") == "pending");
    // Corrupt/legacy approvals are revalidated, not trusted as a parsing bypass.
    database.execute("UPDATE proposals SET status = 'approved'");
    CHECK_THROWS_AS(writer.apply(seeded.proposal_id), kc::vault::VaultError);
    CHECK(database.scalar_text("SELECT status FROM proposals") == "approved");
  }
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM articles") == 0);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM article_fts") == 0);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM apply_runs") == 0);
  CHECK(std::filesystem::is_empty(root / "vault"));
  CHECK(std::filesystem::is_empty(root / ".knowledge-compiler/backups"));
}

TEST_CASE("read-only create preview is deterministic and apply writes exactly the approved Markdown", "[review-preview]") {
  kc::test::TemporaryDirectory temporary;
  const auto root = temporary.path() / "project";
  const auto initialized = kc::storage::initialize_project(init_options(root));
  kc::storage::Database database(initialized.state_path);
  const auto fixture = seed_create_proposal(root, database);
  kc::vault::VaultWriter writer(root);
  const auto preview = writer.preview(fixture.proposal_id);
  REQUIRE(preview.valid);
  REQUIRE(preview.after);
  CHECK_FALSE(preview.before);
  CHECK(preview.destination == "vault/Markov Chains.md");
  CHECK(preview.token == writer.preview(fixture.proposal_id).token);
  CHECK(preview.after->find("article_id: \"" + preview.article_id->value + "\"") != std::string::npos);
  CHECK(preview.after->find("#page=3") != std::string::npos);
  CHECK(preview.diff.find("--- /dev/null") != std::string::npos);
  CHECK(preview.diff.find("+aliases:") != std::string::npos);
  CHECK_FALSE(std::filesystem::exists(root / "vault/_sources"));
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM apply_runs") == 0);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM proposal_approvals") == 0);
  CHECK(database.scalar_text("SELECT status FROM proposals") == "pending");
  CHECK_THROWS_AS(writer.approve(fixture.proposal_id), kc::vault::VaultError);
  CHECK_THROWS_AS(writer.approve(fixture.proposal_id, std::string(64,'0')), kc::vault::VaultError);
  writer.approve(fixture.proposal_id, preview.token);
  CHECK_FALSE(std::filesystem::exists(root / preview.destination));
  CHECK_FALSE(std::filesystem::exists(root / "vault/_sources"));
  CHECK(writer.preview(fixture.proposal_id).matches_approval == true);
  const auto applied = writer.apply(fixture.proposal_id);
  CHECK(applied.article_id == preview.article_id);
  CHECK(read_file(root / applied.vault_path) == *preview.after);
  CHECK(database.scalar_text("SELECT body FROM article_fts") == *preview.after);
}

TEST_CASE("update preview shows exact removals and preserves metadata and surrounding bytes", "[review-preview]") {
  kc::test::TemporaryDirectory temporary;
  const auto root = temporary.path() / "project";
  const auto initialized = kc::storage::initialize_project(init_options(root));
  kc::storage::Database database(initialized.state_path);
  const auto fixture = seed_create_proposal(root, database);
  kc::vault::VaultWriter writer(root);
  kc::test::approve_preview(writer, fixture.proposal_id);
  const auto created = writer.apply(fixture.proposal_id);
  const auto path = root / created.vault_path;
  const auto surrounding = "\r\n## Personal material\r\nKeep \"every\" user byte.\r\n";
  const auto before = read_file(path) + surrounding;
  write_file(path, before);
  const auto update = seed_update_proposal(database, fixture.page_id, created.article_id, "A revised explanation is proposed.");
  const auto review = kc::review::ReviewService(root).review_proposal(update);
  const auto& preview = review.preview;
  REQUIRE(preview.valid);
  CHECK(preview.before == before);
  CHECK(preview.after->ends_with(surrounding));
  CHECK(preview.diff.find("-A Markov chain moves between states") != std::string::npos);
  CHECK(preview.diff.find("+A revised explanation is proposed") != std::string::npos);
  CHECK(preview.after->substr(0, before.find("<!-- kc:managed:start")) == before.substr(0, before.find("<!-- kc:managed:start")));
  CHECK(read_file(path) == before);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM apply_runs") == 1);
  writer.approve(update, preview.token);
  CHECK(read_file(path) == before);
  static_cast<void>(writer.apply(update));
  CHECK(read_file(path) == preview.after);
}

TEST_CASE("untracked collision preview requires matching consent at approval and apply", "[review-preview]") {
  const auto empty_collision = GENERATE(false, true);
  kc::test::TemporaryDirectory temporary;
  const auto root = temporary.path() / "project";
  const auto initialized = kc::storage::initialize_project(init_options(root));
  kc::storage::Database database(initialized.state_path);
  const auto fixture = seed_create_proposal(root, database);
  kc::vault::VaultWriter writer(root);
  const auto path = root / "vault/Markov Chains.md";
  const auto user = empty_collision ? std::string{} : "# My user article\nImportant unrelated content.\n";
  write_file(path,user);
  const auto blocked = writer.preview(fixture.proposal_id);
  CHECK_FALSE(blocked.valid);
  CHECK(blocked.untracked_collision);
  CHECK(blocked.before == user);
  REQUIRE(blocked.after);
  REQUIRE_FALSE(blocked.issues.empty());
  CHECK(blocked.diff.find("+article_id:") != std::string::npos);
  if (!empty_collision) CHECK(blocked.diff.find("-Important unrelated content.") != std::string::npos);
  CHECK(read_file(path) == user);
  const auto consent = writer.preview(fixture.proposal_id, {.allow_overwrite_user_file = true});
  REQUIRE(consent.valid);
  CHECK(consent.token != blocked.token);
  CHECK_THROWS_AS(writer.approve(fixture.proposal_id, consent.token), kc::vault::VaultError);
  writer.approve(fixture.proposal_id, consent.token, {.allow_overwrite_user_file = true});
  CHECK_THROWS_AS(writer.apply(fixture.proposal_id), kc::vault::VaultError);
  CHECK(read_file(path) == user);
  const auto applied = writer.apply(fixture.proposal_id, {.allow_overwrite_user_file = true});
  CHECK(read_file(path) == consent.after);
  CHECK(read_file(root / *applied.backup_path) == user);
}

TEST_CASE("preview reports invalid proposals and evidence without copying sources or changing review state", "[review-preview]") {
  const auto damage = GENERATE("payload","citations","source","unreviewed","image_metadata");
  CAPTURE(damage);
  kc::test::TemporaryDirectory temporary;
  const auto root = temporary.path() / "project";
  const auto initialized = kc::storage::initialize_project(init_options(root));
  kc::storage::Database database(initialized.state_path);
  const auto fixture = seed_create_proposal(root, database);
  if (std::string_view(damage) == "payload") database.execute("UPDATE proposals SET payload_json = '{}'");
  else if (std::string_view(damage) == "citations") database.execute("UPDATE proposal_citations SET quote='wrong'");
  else if (std::string_view(damage) == "unreviewed") database.execute("UPDATE source_pages SET text_status='ocr_unreviewed'");
  else if (std::string_view(damage) == "image_metadata") database.execute("UPDATE source_pages SET image_sha256='hash-with-no-image'");
  else {
    const auto retained = root / fixture.imported.version.stored_path;
    std::filesystem::permissions(retained,std::filesystem::perms::owner_write,std::filesystem::perm_options::add);
    write_file(retained,"tampered evidence");
  }
  kc::vault::VaultWriter writer(root);
  const auto preview = writer.preview(fixture.proposal_id);
  CHECK_FALSE(preview.valid);
  REQUIRE_FALSE(preview.issues.empty());
  const auto data = kc::vault::preview_json(preview);
  CHECK(data["valid"] == false);
  CHECK_FALSE(data["issues"].empty());
  CHECK_THROWS_AS(writer.approve(fixture.proposal_id, preview.token), kc::vault::VaultError);
  CHECK(database.scalar_text("SELECT status FROM proposals") == "pending");
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM proposal_approvals") == 0);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM apply_runs") == 0);
  CHECK_FALSE(std::filesystem::exists(root / "vault/_sources"));
}

TEST_CASE("changed preview inputs require fresh approval and preserve all user changes", "[review-preview]") {
  const auto change = GENERATE("notes","aliases","source_label","config","trailing_root");
  CAPTURE(change);
  kc::test::TemporaryDirectory temporary;
  const auto root = temporary.path() / "project";
  const auto initialized = kc::storage::initialize_project(init_options(root));
  kc::storage::Database database(initialized.state_path);
  const auto fixture = seed_create_proposal(root, database);
  kc::vault::VaultWriter writer(root);
  kc::test::approve_preview(writer, fixture.proposal_id);
  const auto created = writer.apply(fixture.proposal_id);
  const auto path = root / created.vault_path;
  const auto update = seed_update_proposal(database, fixture.page_id, created.article_id);
  const auto old = writer.preview(update);
  writer.approve(update, old.token);
  auto content = read_file(path);
  if (std::string_view(change) == "notes") content += "\nUnrelated user addition after approval.\n";
  else if (std::string_view(change) == "aliases") content.replace(content.find("Markov chain\""), std::string("Markov chain").size(), "User alias");
  else if (std::string_view(change) == "source_label") database.execute("UPDATE sources SET display_name='Renamed source.pdf'");
  else {
    auto config = kc::test::read_json(root / "kc.json");
    if (std::string_view(change) == "trailing_root") config["paths"]["vault"] = "vault/";
    else config["vault"]["source_directory"] = "_reviewed_sources";
    write_file(root / "kc.json", config.dump());
  }
  write_file(path,content);
  // A fresh writer observes configuration changes as it does in separate CLI invocations.
  kc::vault::VaultWriter current(root);
  const auto changed = current.preview(update);
  REQUIRE(changed.valid);
  CHECK(changed.token != old.token);
  CHECK(changed.matches_approval == false);
  CHECK_THROWS_AS(current.apply(update), kc::vault::VaultError);
  CHECK(read_file(path) == content);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM apply_runs") == 1);
  kc::review::ReviewService(root).revoke_approval(update);
  CHECK_THROWS_AS(current.approve(update,old.token), kc::vault::VaultError);
  current.approve(update,changed.token);
  static_cast<void>(current.apply(update));
  CHECK(read_file(path) == changed.after);
}

TEST_CASE("unused approvals can be revoked or rejected without any implicit vault write", "[review-preview]") {
  const auto reject = GENERATE(false,true);
  kc::test::TemporaryDirectory temporary;
  const auto root = temporary.path() / "project";
  const auto initialized = kc::storage::initialize_project(init_options(root));
  kc::storage::Database database(initialized.state_path);
  const auto fixture = seed_create_proposal(root,database);
  kc::vault::VaultWriter writer(root);
  kc::review::ReviewService review(root);
  CHECK_THROWS_AS(review.revoke_approval(fixture.proposal_id),kc::review::ReviewError);
  kc::test::approve_preview(writer,fixture.proposal_id);
  if (reject) review.reject_proposal(fixture.proposal_id,"Found an extraction error");
  else review.revoke_approval(fixture.proposal_id);
  CHECK(database.scalar_text("SELECT status FROM proposals") == (reject ? "rejected" : "pending"));
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM proposal_approvals") == 0);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM articles") == 0);
  CHECK_FALSE(std::filesystem::exists(root / "vault/_sources"));
  CHECK_THROWS_AS(writer.apply(fixture.proposal_id),kc::vault::VaultError);
  if (!reject) {
    kc::test::approve_preview(writer,fixture.proposal_id);
    const auto applied = writer.apply(fixture.proposal_id);
    const auto bytes = read_file(root / applied.vault_path);
    CHECK_THROWS_AS(review.revoke_approval(fixture.proposal_id),kc::review::ReviewError);
    CHECK_THROWS_AS(review.reject_proposal(fixture.proposal_id),kc::review::ReviewError);
    CHECK(read_file(root / applied.vault_path) == bytes);
  }
}

TEST_CASE("preview catches source-copy filesystem conflicts before approval", "[review-preview]") {
  const auto conflict = GENERATE("retained_symlink","copy_symlink","parent_file","article_overlap");
  CAPTURE(conflict);
  kc::test::TemporaryDirectory temporary;
  const auto root = temporary.path() / "project";
  const auto initialized = kc::storage::initialize_project(init_options(root));
  kc::storage::Database database(initialized.state_path);
  const auto fixture = seed_create_proposal(root,database);
  const auto retained = root / fixture.imported.version.stored_path;
  if (std::string_view(conflict) == "retained_symlink") {
    const auto alternate = retained.parent_path() / "alternate.pdf";
    std::filesystem::rename(retained,alternate);
    std::filesystem::create_symlink(alternate.filename(),retained);
  } else if (std::string_view(conflict) == "copy_symlink") {
    const auto alternate = root / "vault/alternate.pdf";
    std::filesystem::copy_file(retained,alternate);
    const auto directory = root / "vault/_sources";
    std::filesystem::create_directory(directory);
    std::filesystem::create_symlink("../alternate.pdf",directory /
        (fixture.imported.source.source_id.value + "-" + fixture.imported.version.source_version_id.value + ".pdf"));
  } else if (std::string_view(conflict) == "parent_file") {
    write_file(root / "vault/_sources","A file cannot contain source copies.");
  } else {
    auto config = kc::test::read_json(root / "kc.json");
    config["vault"]["source_directory"] = "Markov Chains.md";
    write_file(root / "kc.json",config.dump());
  }
  kc::vault::VaultWriter writer(root);
  const auto preview = writer.preview(fixture.proposal_id);
  CHECK_FALSE(preview.valid);
  REQUIRE_FALSE(preview.issues.empty());
  CHECK_THROWS_AS(writer.approve(fixture.proposal_id,preview.token),kc::vault::VaultError);
  CHECK(database.scalar_text("SELECT status FROM proposals") == "pending");
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM apply_runs") == 0);
  CHECK_FALSE(std::filesystem::exists(root / "vault/Markov Chains.md"));
}

TEST_CASE("invalid user article encoding produces a safe JSON preview diagnostic without modifying bytes", "[review-preview]") {
  const auto nul = GENERATE(false,true);
  const auto tracked = GENERATE(false,true);
  const auto consent = GENERATE(false,true);
  kc::test::TemporaryDirectory temporary;
  const auto root = temporary.path() / "project";
  const auto initialized = kc::storage::initialize_project(init_options(root));
  kc::storage::Database database(initialized.state_path);
  const auto fixture = seed_create_proposal(root,database);
  kc::vault::VaultWriter writer(root);
  const auto path = root / "vault/Markov Chains.md";
  auto proposal_id = fixture.proposal_id;
  if (tracked) {
    kc::test::approve_preview(writer,fixture.proposal_id);
    const auto created = writer.apply(fixture.proposal_id);
    proposal_id = seed_update_proposal(database,fixture.page_id,created.article_id);
  } else {
    write_file(path,"Untracked user content.\n");
  }
  const auto malformed = read_file(path) + (nul ? std::string("\0",1) : std::string("\xff",1));
  write_file(path,malformed);
  const kc::vault::ApplyOptions options{.allow_overwrite_user_file = consent};
  const auto preview = writer.preview(proposal_id,options);
  CHECK_FALSE(preview.valid);
  CHECK(preview.untracked_collision == !tracked);
  const auto json = kc::vault::preview_json(preview);
  CHECK_NOTHROW(json.dump());
  CHECK(json["before"].is_null());
  CHECK(json["before_sha256"] == kc::source_import::sha256_text(malformed));
  CHECK_THROWS_AS(writer.approve(proposal_id,preview.token,options),kc::vault::VaultError);
  CHECK(read_file(path) == malformed);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM apply_runs") == (tracked ? 1 : 0));
}

#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sqlite3.h>
#include <cerrno>

namespace {
/// The child exits without stack unwinding, precisely bypassing every catch
/// and RAII cleanup. Parent opens a fresh connection to trigger restart recovery.
void terminate_apply(const std::filesystem::path& root, const kc::domain::ProposalId& proposal,
                     std::string_view boundary, bool overwrite) {
  const auto child = fork();
  REQUIRE(child >= 0);
  if (child == 0) {
    try {
      kc::vault::VaultWriter writer(root,[&](std::string_view point) {
        if (point == boundary) _exit(77);
      });
      static_cast<void>(writer.apply(proposal,{.allow_overwrite_user_file=overwrite}));
    } catch (...) { _exit(78); }
    _exit(79); // Missing checkpoint must fail the test, not silently pass.
  }
  int status = 0;
  REQUIRE(waitpid(child,&status,0) == child);
  REQUIRE(WIFEXITED(status));
  REQUIRE(WEXITSTATUS(status) == 77);
}

bool committed_boundary(std::string_view point) {
  return point == "after_database_commit" || point == "before_cleanup" || point == "after_cleanup";
}

const std::vector<std::string_view> all_apply_boundaries{
    "before_journal","after_journal","before_backup","after_backup",
    "before_source_copies","before_source_publish","after_source_publish","after_source_copies",
    "before_article_stage","after_article_stage","before_article_replace","after_article_exchange",
    "after_article_replace","before_database_publish","after_database_publish",
    "before_database_commit","after_database_commit","before_cleanup","after_cleanup"};

void no_active_artifacts(const std::filesystem::path& root) {
  for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
    CHECK_FALSE(entry.path().filename().string().ends_with(".tmp"));
    CHECK_FALSE(entry.path().filename().string().ends_with(".pending"));
    if (entry.path().parent_path().filename() == "transactions" && entry.path().extension()==".json") {
      FAIL("active journal survived successful recovery");
    }
  }
}
}

TEST_CASE("termination at every apply durability boundary recovers create and collision consistently", "[apply-recovery]") {
  for (const bool collision : {false,true}) for (const auto point : all_apply_boundaries) {
    CAPTURE(collision,point);
    kc::test::TemporaryDirectory temporary;
    const auto root = temporary.path() / "project";
    const auto initialized = kc::storage::initialize_project(init_options(root));
    kc::storage::Database database(initialized.state_path);
    const auto fixture = seed_create_proposal(root,database);
    const auto article = root / "vault/Markov Chains.md";
    const std::string prior = "# Untracked user page\nDo not lose these bytes.\n";
    if (collision) write_file(article,prior);
    kc::vault::VaultWriter writer(root);
    const auto preview = writer.preview(fixture.proposal_id,{.allow_overwrite_user_file=collision});
    REQUIRE(preview.valid);
    writer.approve(fixture.proposal_id,preview.token,{.allow_overwrite_user_file=collision});
    terminate_apply(root,fixture.proposal_id,point,collision);
    kc::storage::Database restarted(initialized.state_path);
    const bool committed = committed_boundary(point);
    CHECK(restarted.scalar_text("SELECT status FROM proposals") == (committed ? "applied" : "approved"));
    CHECK(restarted.scalar_integer("SELECT COUNT(*) FROM articles") == (committed ? 1 : 0));
    CHECK(restarted.scalar_integer("SELECT COUNT(*) FROM apply_runs") == (committed ? 1 : 0));
    CHECK(restarted.scalar_integer("SELECT COUNT(*) FROM article_citations") == (committed ? 3 : 0));
    CHECK(restarted.scalar_integer("SELECT COUNT(*) FROM article_fts") == (committed ? 1 : 0));
    CHECK(restarted.scalar_integer("SELECT COUNT(*) FROM article_snapshots") == (committed ? 1 : 0));
    if (committed) CHECK(read_file(article) == *preview.after);
    else if (collision) CHECK(read_file(article) == prior);
    else CHECK_FALSE(std::filesystem::exists(article));
    // Copy paths come from the renderer/config rather than a hard-coded guess.
    const auto config = initialized.config;
    std::size_t copies = 0;
    if (std::filesystem::exists(root / config.paths.vault / config.vault.source_directory)) {
      for (const auto& entry : std::filesystem::recursive_directory_iterator(root / config.paths.vault / config.vault.source_directory)) {
        if (entry.is_regular_file()) ++copies;
      }
    }
    CHECK(copies == (committed ? 1U : 0U));
    no_active_artifacts(root);
    // Repeated open must be idempotent, including recovery after cleanup.
    kc::storage::Database twice(initialized.state_path);
    CHECK(twice.scalar_integer("SELECT COUNT(*) FROM apply_runs") == (committed ? 1 : 0));
  }
}

TEST_CASE("termination during updates and restores keeps active history citations Markdown and search together", "[apply-recovery]") {
  for (const bool restoring : {false,true}) for (const auto point : all_apply_boundaries) {
    if (point == "before_source_publish" || point == "after_source_publish") continue;
    CAPTURE(restoring,point);
    kc::test::TemporaryDirectory temporary;
    const auto root = temporary.path() / "project";
    const auto initialized = kc::storage::initialize_project(init_options(root));
    kc::storage::Database database(initialized.state_path);
    const auto fixture = seed_create_proposal(root,database);
    kc::vault::VaultWriter writer(root);
    kc::test::approve_preview(writer,fixture.proposal_id);
    const auto first = writer.apply(fixture.proposal_id);
    const auto first_bytes = read_file(root / first.vault_path);
    const auto update = seed_update_proposal(database,fixture.page_id,first.article_id,"Changed explanation for a second revision.");
    const auto preview = writer.preview(update);
    REQUIRE(preview.valid);
    writer.approve(update,preview.token);
    std::string prior = first_bytes;
    std::string next = *preview.after;
    std::string before_run = first.apply_run_id.value;
    std::string after_run;
    if (restoring) {
      const auto second = writer.apply(update);
      prior = next; next = first_bytes; before_run = second.apply_run_id.value; after_run = first.apply_run_id.value;
      const auto restore = writer.restore_preview(first.apply_run_id);
      const auto token = restore.at("preview_sha256").get<std::string>();
      const auto child = fork();
      REQUIRE(child>=0);
      if (child==0) {
        try {
          kc::vault::VaultWriter interrupted(root,[&](std::string_view current) { if (current==point) _exit(77); });
          static_cast<void>(interrupted.restore(first.apply_run_id,token));
        } catch (...) { _exit(78); }
        _exit(79);
      }
      int status=0; REQUIRE(waitpid(child,&status,0)==child); REQUIRE(WIFEXITED(status)); REQUIRE(WEXITSTATUS(status)==77);
    } else terminate_apply(root,update,point,false);
    kc::storage::Database restarted(initialized.state_path);
    const bool committed = committed_boundary(point);
    const auto bytes = committed ? next : prior;
    CHECK(read_file(root / first.vault_path) == bytes);
    CHECK(restarted.scalar_text("SELECT body FROM article_fts") == bytes);
    CHECK(restarted.scalar_text("SELECT content_sha256 FROM articles") == kc::source_import::sha256_text(bytes));
    CHECK(restarted.scalar_integer("SELECT COUNT(*) FROM article_citations") == 3);
    CHECK(restarted.scalar_text("SELECT status FROM proposals WHERE proposal_id="+sql_string(update.value)) ==
        (restoring || committed ? "applied" : "approved"));
    CHECK(restarted.scalar_integer("SELECT COUNT(*) FROM restore_runs") == (restoring && committed ? 1 : 0));
    if (!committed) CHECK(restarted.scalar_text("SELECT current_apply_run_id FROM articles") == before_run);
    else if (restoring) CHECK(restarted.scalar_text("SELECT current_apply_run_id FROM articles") == after_run);
    else CHECK(restarted.scalar_text("SELECT current_apply_run_id FROM articles") != before_run);
    no_active_artifacts(root);
  }
}

TEST_CASE("external saves during staging exchange publication and recovery are never overwritten silently", "[apply-recovery]") {
  for (const auto point : {"before_article_stage","before_article_replace","after_article_exchange","before_database_commit"}) {
    CAPTURE(point);
    kc::test::TemporaryDirectory temporary;
    const auto root=temporary.path()/"project";
    const auto initialized=kc::storage::initialize_project(init_options(root));
    kc::storage::Database database(initialized.state_path);
    const auto fixture=seed_create_proposal(root,database);
    const auto article=root/"vault/Markov Chains.md";
    const std::string old="Original untracked file\n";
    const std::string user="A concurrent editor save must survive\n";
    write_file(article,old);
    kc::vault::VaultWriter writer(root,[&](std::string_view current) {
      if (current==point) {
        // Model Obsidian's atomic-save behavior, rather than cooperative locks.
        const auto edit=root/"vault/editor-save.md"; write_file(edit,user); std::filesystem::rename(edit,article);
      }
    });
    const auto preview=writer.preview(fixture.proposal_id,{.allow_overwrite_user_file=true});
    writer.approve(fixture.proposal_id,preview.token,{.allow_overwrite_user_file=true});
    CHECK_THROWS(writer.apply(fixture.proposal_id,{.allow_overwrite_user_file=true}));
    CHECK(read_file(article)==user);
    CHECK(database.scalar_text("SELECT status FROM proposals")=="approved");
    CHECK(database.scalar_integer("SELECT COUNT(*) FROM article_fts")==0);
    if (std::string_view(point)=="after_article_exchange" || std::string_view(point)=="before_database_commit") {
      CHECK_THROWS(kc::storage::Database(initialized.state_path));
      kc::storage::keep_recovery_conflicts(root);
      CHECK(read_file(article)==user);
      CHECK(std::filesystem::is_directory(root/".knowledge-compiler/recovery-conflicts"));
    }
    kc::storage::Database restart(initialized.state_path);
    CHECK(restart.scalar_integer("SELECT COUNT(*) FROM apply_runs")==0);
  }
}

TEST_CASE("failed disk permission locked-file and commit boundaries roll back through the durable journal", "[apply-recovery]") {
  for (const auto code : {ENOSPC,EACCES,EBUSY}) for (const auto point : {"before_backup","before_source_publish","before_article_replace"}) {
    CAPTURE(code,point);
    kc::test::TemporaryDirectory temporary;
    const auto root=temporary.path()/"project";
    const auto initialized=kc::storage::initialize_project(init_options(root));
    kc::storage::Database database(initialized.state_path);
    const auto fixture=seed_create_proposal(root,database);
    const auto article=root/"vault/Markov Chains.md";
    write_file(article,"User collision\n");
    kc::vault::VaultWriter writer(root,[&](std::string_view current) {
      if (current==point) throw std::system_error(code,std::generic_category(),"injected OS failure");
    });
    const auto preview=writer.preview(fixture.proposal_id,{.allow_overwrite_user_file=true});
    writer.approve(fixture.proposal_id,preview.token,{.allow_overwrite_user_file=true});
    CHECK_THROWS(writer.apply(fixture.proposal_id,{.allow_overwrite_user_file=true}));
    CHECK(read_file(article)=="User collision\n");
    kc::storage::Database restart(initialized.state_path);
    CHECK(restart.scalar_integer("SELECT COUNT(*) FROM apply_runs")==0);
    CHECK(restart.scalar_integer("SELECT COUNT(*) FROM article_fts")==0);
    no_active_artifacts(root);
  }
  SECTION("SQLite publication failure after article exchange") {
    kc::test::TemporaryDirectory temporary;
    const auto root=temporary.path()/"project";
    const auto initialized=kc::storage::initialize_project(init_options(root));
    kc::storage::Database database(initialized.state_path);
    const auto fixture=seed_create_proposal(root,database);
    database.execute("CREATE TRIGGER fail_publication BEFORE INSERT ON apply_runs BEGIN SELECT RAISE(ABORT,'injected publication failure'); END");
    kc::vault::VaultWriter writer(root); kc::test::approve_preview(writer,fixture.proposal_id);
    CHECK_THROWS(writer.apply(fixture.proposal_id));
    CHECK_FALSE(std::filesystem::exists(root/"vault/Markov Chains.md"));
    CHECK(database.scalar_text("SELECT status FROM proposals")=="approved");
    no_active_artifacts(root);
  }
}

TEST_CASE("failed recovery retains the journal and succeeds on retry without trusting a damaged backup", "[apply-recovery]") {
  kc::test::TemporaryDirectory temporary;
  const auto root=temporary.path()/"project";
  const auto initialized=kc::storage::initialize_project(init_options(root));
  kc::storage::Database database(initialized.state_path);
  const auto fixture=seed_create_proposal(root,database);
  const auto article=root/"vault/Markov Chains.md";
  const std::string old="User's pre-apply content\n"; write_file(article,old);
  kc::vault::VaultWriter writer(root);
  const auto preview=writer.preview(fixture.proposal_id,{.allow_overwrite_user_file=true});
  writer.approve(fixture.proposal_id,preview.token,{.allow_overwrite_user_file=true});
  terminate_apply(root,fixture.proposal_id,"after_article_replace",true);
  // Real directory permission denial makes recovery's sibling write fail.
  REQUIRE(chmod((root/"vault").c_str(),0500)==0);
  CHECK_THROWS(kc::storage::Database(initialized.state_path));
  CHECK(read_file(article)==*preview.after);
  REQUIRE(chmod((root/"vault").c_str(),0700)==0);
  kc::storage::Database restarted(initialized.state_path);
  CHECK(read_file(article)==old);
  CHECK(restarted.scalar_integer("SELECT COUNT(*) FROM apply_runs")==0);
  no_active_artifacts(root);
}
#endif

TEST_CASE("history restore requires current reviewed bytes and reactivates structured evidence and search", "[apply-recovery]") {
  kc::test::TemporaryDirectory temporary;
  const auto root=temporary.path()/"project";
  const auto initialized=kc::storage::initialize_project(init_options(root));
  kc::storage::Database database(initialized.state_path);
  const auto fixture=seed_create_proposal(root,database);
  kc::vault::VaultWriter writer(root); kc::test::approve_preview(writer,fixture.proposal_id);
  const auto first=writer.apply(fixture.proposal_id);
  const auto first_bytes=read_file(root/first.vault_path);
  const auto newer_page=kc::domain::generate_page_id();
  database.execute("INSERT INTO source_pages(page_id,source_version_id,page_number,text,text_status,text_sha256) "
      "SELECT "+sql_string(newer_page.value)+",source_version_id,4,text,text_status,text_sha256 FROM source_pages WHERE page_id="+sql_string(fixture.page_id.value));
  const auto update=seed_update_proposal(database,newer_page,first.article_id,"Novel second revision words.");
  kc::test::approve_preview(writer,update); const auto second=writer.apply(update);
  CHECK(database.scalar_text("SELECT page_id FROM article_citations LIMIT 1")==newer_page.value);
  const auto before=writer.restore_preview(first.apply_run_id);
  CHECK(before.at("after")==first_bytes);
  CHECK_THROWS(writer.restore(first.apply_run_id,""));
  const auto newer=read_file(root/first.vault_path)+"\nA new user note\n";
  write_file(root/first.vault_path,newer);
  CHECK_THROWS(writer.restore(first.apply_run_id,before.at("preview_sha256").get<std::string>()));
  CHECK(read_file(root/first.vault_path)==newer);
  const auto review=writer.restore_preview(first.apply_run_id);
  CHECK(review.at("diff").get<std::string>().find("-A new user note")!=std::string::npos);
  static_cast<void>(writer.restore(first.apply_run_id,review.at("preview_sha256").get<std::string>()));
  CHECK(read_file(root/first.vault_path)==first_bytes);
  CHECK(database.scalar_text("SELECT body FROM article_fts")==first_bytes);
  CHECK(database.scalar_text("SELECT current_apply_run_id FROM articles")==first.apply_run_id.value);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM article_citations")==3);
  CHECK(database.scalar_text("SELECT page_id FROM article_citations LIMIT 1")==fixture.page_id.value);
  const auto history=writer.history(first.article_id);
  REQUIRE(history.at("revisions").size()==2);
  CHECK(history.at("revisions")[0].at("current")==true);
  CHECK(history.at("revisions")[1].at("current")==false);
  CHECK(history.at("restores")[0].at("from_apply_run_id")==second.apply_run_id.value);
  const auto backup=history.at("restores")[0].at("backup_path").get<std::string>();
  CHECK(read_file(root/backup)==newer);
  // Compiler accumulation follows the restored structured revision, not rowid.
  const auto base=kc::updates::load_base(database.native_handle(),root,initialized.config,first.article_id);
  CHECK(base.apply_run_id==first.apply_run_id);
  CHECK(base.proposal.sections.front().blocks.front().text=="A Markov chain moves between states.");
}

#if !defined(_WIN32)
TEST_CASE("termination during recovery itself is idempotent before and after each restoration step", "[apply-recovery]") {
  for (const auto point : {"before_recovery_stage","after_recovery_stage","before_recovery_replace",
                          "after_recovery_exchange","after_recovery_replace","before_recovery_cleanup","after_recovery_cleanup"}) {
    CAPTURE(point);
    kc::test::TemporaryDirectory temporary;
    const auto root=temporary.path()/"project";
    const auto initialized=kc::storage::initialize_project(init_options(root));
    kc::storage::Database database(initialized.state_path);
    const auto fixture=seed_create_proposal(root,database);
    const auto article=root/"vault/Markov Chains.md";
    const std::string old="Original user bytes survive repeated process deaths.\n";
    write_file(article,old);
    kc::vault::VaultWriter writer(root);
    const auto preview=writer.preview(fixture.proposal_id,{.allow_overwrite_user_file=true});
    writer.approve(fixture.proposal_id,preview.token,{.allow_overwrite_user_file=true});
    terminate_apply(root,fixture.proposal_id,"after_article_replace",true);
    const auto child=fork(); REQUIRE(child>=0);
    if (child==0) {
      try {
        kc::storage::Database recovery(initialized.state_path,kc::storage::OpenMode::read_write,false);
        kc::storage::recover_on_open(recovery,initialized.state_path,false,[&](std::string_view current) { if (current==point) _exit(77); });
      } catch (...) { _exit(78); }
      _exit(79);
    }
    int status=0; REQUIRE(waitpid(child,&status,0)==child); REQUIRE(WIFEXITED(status)); REQUIRE(WEXITSTATUS(status)==77);
    kc::storage::Database restarted(initialized.state_path);
    CHECK(read_file(article)==old);
    CHECK(restarted.scalar_text("SELECT status FROM proposals")=="approved");
    CHECK(restarted.scalar_integer("SELECT COUNT(*) FROM article_fts")==0);
    CHECK(restarted.scalar_integer("SELECT COUNT(*) FROM apply_runs")==0);
    no_active_artifacts(root);
  }
}

namespace {
// SQLite's actual commit hook rejects publication at COMMIT, after all SQL and
// file changes have executed. This differs from a pre-commit thrown exception.
bool reject_next_sqlite_commit=false;
int reject_commit(void*) { return reject_next_sqlite_commit ? 1 : 0; }
int install_commit_hook(sqlite3* connection,char**,const sqlite3_api_routines*) {
  sqlite3_commit_hook(connection,reject_commit,nullptr);
  return SQLITE_OK;
}
class CommitHookFixture {
 public:
  CommitHookFixture() {
    if (sqlite3_auto_extension(reinterpret_cast<void(*)()>(install_commit_hook))!=SQLITE_OK) throw std::runtime_error("cannot install commit failure hook");
  }
  ~CommitHookFixture() { reject_next_sqlite_commit=false; sqlite3_cancel_auto_extension(reinterpret_cast<void(*)()>(install_commit_hook)); }
};
}
TEST_CASE("a real SQLite commit rejection restores bytes and leaves proposal citations and search unpublished", "[apply-recovery]") {
  kc::test::TemporaryDirectory temporary;
  const auto root=temporary.path()/"project";
  const auto initialized=kc::storage::initialize_project(init_options(root));
  kc::storage::Database database(initialized.state_path);
  const auto fixture=seed_create_proposal(root,database);
  const auto article=root/"vault/Markov Chains.md";
  write_file(article,"Previous user bytes\n");
  CommitHookFixture hooks;
  kc::vault::VaultWriter writer(root,[](std::string_view point) {
    if (point=="before_database_commit") reject_next_sqlite_commit=true;
  });
  const auto preview=writer.preview(fixture.proposal_id,{.allow_overwrite_user_file=true});
  writer.approve(fixture.proposal_id,preview.token,{.allow_overwrite_user_file=true});
  CHECK_THROWS(writer.apply(fixture.proposal_id,{.allow_overwrite_user_file=true}));
  reject_next_sqlite_commit=false;
  kc::storage::Database restart(initialized.state_path);
  CHECK(read_file(article)=="Previous user bytes\n");
  CHECK(restart.scalar_text("SELECT status FROM proposals")=="approved");
  CHECK(restart.scalar_integer("SELECT COUNT(*) FROM article_citations")==0);
  CHECK(restart.scalar_integer("SELECT COUNT(*) FROM article_fts")==0);
  no_active_artifacts(root);
}

TEST_CASE("recovery refuses escaping journals and never removes unrelated files", "[apply-recovery]") {
  kc::test::TemporaryDirectory temporary;
  const auto root=temporary.path()/"project";
  const auto initialized=kc::storage::initialize_project(init_options(root));
  kc::storage::Database database(initialized.state_path);
  const auto fixture=seed_create_proposal(root,database);
  kc::vault::VaultWriter writer(root); kc::test::approve_preview(writer,fixture.proposal_id);
  terminate_apply(root,fixture.proposal_id,"after_journal",false);
  const auto journal_dir=root/".knowledge-compiler/transactions";
  const auto entry=std::filesystem::directory_iterator(journal_dir)->path();
  auto journal=kc::test::read_json(entry);
  const auto external=temporary.path()/"external.md";
  write_file(external,"Never touch outside bytes\n");
  journal["destination"]="../external.md";
  write_file(entry,journal.dump());
  CHECK_THROWS(kc::storage::Database(initialized.state_path));
  CHECK_THROWS(kc::storage::keep_recovery_conflicts(root));
  CHECK(read_file(external)=="Never touch outside bytes\n");
}
#endif

#if !defined(_WIN32)
TEST_CASE("partial backup and article staging writes from disk exhaustion are reconciled safely", "[apply-recovery]") {
  for (const auto point : {"before_backup","before_article_stage"}) {
    CAPTURE(point);
    kc::test::TemporaryDirectory temporary;
    const auto root=temporary.path()/"project";
    const auto initialized=kc::storage::initialize_project(init_options(root));
    kc::storage::Database database(initialized.state_path);
    const auto fixture=seed_create_proposal(root,database);
    const auto article=root/"vault/Markov Chains.md";
    const std::string old="Complete existing user content\n"; write_file(article,old);
    kc::vault::VaultWriter writer(root,[&](std::string_view current) {
      if (current==point) {
        const auto dir=root/".knowledge-compiler/transactions";
        const auto journal=kc::test::read_json(std::filesystem::directory_iterator(dir)->path());
        const auto field=std::string_view(point)=="before_backup" ? "backup" : "temporary";
        const auto partial=root/journal.at(field).get<std::string>();
        std::filesystem::create_directories(partial.parent_path());
        write_file(partial,std::string_view(point)=="before_backup" ? "Complete" : "---\npartly staged");
        throw std::system_error(ENOSPC,std::generic_category(),"injected short write with disk full");
      }
    });
    const auto preview=writer.preview(fixture.proposal_id,{.allow_overwrite_user_file=true});
    writer.approve(fixture.proposal_id,preview.token,{.allow_overwrite_user_file=true});
    CHECK_THROWS(writer.apply(fixture.proposal_id,{.allow_overwrite_user_file=true}));
    CHECK(read_file(article)==old);
    kc::storage::Database restart(initialized.state_path);
    CHECK(restart.scalar_text("SELECT status FROM proposals")=="approved");
    CHECK(restart.scalar_integer("SELECT COUNT(*) FROM apply_runs")==0);
    no_active_artifacts(root);
  }
}

TEST_CASE("a concurrent untracked create cannot be replaced in the exclusive publication gap", "[apply-recovery]") {
  kc::test::TemporaryDirectory temporary;
  const auto root=temporary.path()/"project";
  const auto initialized=kc::storage::initialize_project(init_options(root));
  kc::storage::Database database(initialized.state_path);
  const auto fixture=seed_create_proposal(root,database);
  const auto article=root/"vault/Markov Chains.md";
  kc::vault::VaultWriter writer(root,[&](std::string_view current) {
    if (current=="before_article_replace") write_file(article,"New user-owned file in the rename gap\n");
  });
  kc::test::approve_preview(writer,fixture.proposal_id);
  CHECK_THROWS(writer.apply(fixture.proposal_id));
  CHECK(read_file(article)=="New user-owned file in the rename gap\n");
  kc::storage::keep_recovery_conflicts(root);
  kc::storage::Database restart(initialized.state_path);
  CHECK(restart.scalar_integer("SELECT COUNT(*) FROM articles")==0);
  CHECK(restart.scalar_text("SELECT status FROM proposals")=="approved");
}

#if defined(__APPLE__)
TEST_CASE("native immutable file locks reject replacement without losing approved or user state", "[apply-recovery]") {
  kc::test::TemporaryDirectory temporary;
  const auto root=temporary.path()/"project";
  const auto initialized=kc::storage::initialize_project(init_options(root));
  kc::storage::Database database(initialized.state_path);
  const auto fixture=seed_create_proposal(root,database);
  const auto article=root/"vault/Markov Chains.md";
  write_file(article,"Locked user file\n");
  kc::vault::VaultWriter writer(root);
  const auto preview=writer.preview(fixture.proposal_id,{.allow_overwrite_user_file=true});
  writer.approve(fixture.proposal_id,preview.token,{.allow_overwrite_user_file=true});
  REQUIRE(chflags(article.c_str(),UF_IMMUTABLE)==0);
  struct Unlock { std::filesystem::path path; ~Unlock() { chflags(path.c_str(),0); } } unlock{article};
  CHECK_THROWS(writer.apply(fixture.proposal_id,{.allow_overwrite_user_file=true}));
  CHECK(read_file(article)=="Locked user file\n");
  CHECK(database.scalar_text("SELECT status FROM proposals")=="approved");
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM article_fts")==0);
  no_active_artifacts(root);
}
#endif
#endif

TEST_CASE("restore refuses damaged history or changed rendering paths and reports retained unowned artifacts", "[apply-recovery]") {
  const auto damage=GENERATE("snapshot","source","configuration");
  CAPTURE(damage);
  kc::test::TemporaryDirectory temporary;
  const auto root=temporary.path()/"project";
  const auto initialized=kc::storage::initialize_project(init_options(root));
  kc::storage::Database database(initialized.state_path);
  const auto fixture=seed_create_proposal(root,database);
  kc::vault::VaultWriter writer(root); kc::test::approve_preview(writer,fixture.proposal_id);
  const auto applied=writer.apply(fixture.proposal_id);
  const auto before=read_file(root/applied.vault_path);
  if (std::string_view(damage)=="snapshot") database.execute("UPDATE article_snapshots SET content='corrupt snapshot'");
  else if (std::string_view(damage)=="source") {
    std::filesystem::permissions(root/fixture.imported.version.stored_path,std::filesystem::perms::owner_write,std::filesystem::perm_options::add);
    write_file(root/fixture.imported.version.stored_path,"tampered source");
  }
  else {
    auto config=kc::test::read_json(root/"kc.json"); config["vault"]["source_directory"]="_different_sources";
    write_file(root/"kc.json",config.dump());
  }
  CHECK_THROWS(kc::vault::VaultWriter(root).restore_preview(applied.apply_run_id));
  CHECK(read_file(root/applied.vault_path)==before);
  const auto orphan=root/".knowledge-compiler/backups/legacy-unowned.md";
  write_file(orphan,"Unknown ownership must be preserved\n");
  const auto legacy_temp=root/"vault/user.md.tmp-legacy";
  write_file(legacy_temp,"Unknown user staging content\n");
  const auto inventory=writer.recovery_inventory();
  CHECK(inventory.at("retained_artifacts").size()==2);
  CHECK(read_file(orphan)=="Unknown ownership must be preserved\n");
  CHECK(read_file(legacy_temp)=="Unknown user staging content\n");
}

#if !defined(_WIN32)
TEST_CASE("post-exchange writes to a displaced inode are retained in the recovery archive", "[apply-recovery]") {
  kc::test::TemporaryDirectory temporary;
  const auto root=temporary.path()/"project";
  const auto initialized=kc::storage::initialize_project(init_options(root));
  kc::storage::Database database(initialized.state_path);
  const auto fixture=seed_create_proposal(root,database);
  const auto article=root/"vault/Markov Chains.md";
  write_file(article,"Original untracked content\n");
  kc::vault::VaultWriter writer(root,[&](std::string_view point) {
    if (point=="before_cleanup") {
      const auto journal=kc::test::read_json(std::filesystem::directory_iterator(root/".knowledge-compiler/transactions")->path());
      write_file(root/journal.at("temporary").get<std::string>(),"Editor wrote the displaced inode after commit\n");
    }
  });
  const auto preview=writer.preview(fixture.proposal_id,{.allow_overwrite_user_file=true});
  writer.approve(fixture.proposal_id,preview.token,{.allow_overwrite_user_file=true});
  static_cast<void>(writer.apply(fixture.proposal_id,{.allow_overwrite_user_file=true}));
  CHECK(read_file(article)==*preview.after);
  const auto inventory=writer.recovery_inventory();
  REQUIRE(inventory.at("conflict_archives").size()==1);
  CHECK(read_file(root/inventory.at("conflict_archives")[0].get<std::string>())=="Editor wrote the displaced inode after commit\n");
}

TEST_CASE("keeping a recovery conflict preserves binary user bytes and rejects unowned temporary paths", "[apply-recovery]") {
  const auto damage=GENERATE("binary","unowned");
  kc::test::TemporaryDirectory temporary;
  const auto root=temporary.path()/"project";
  const auto initialized=kc::storage::initialize_project(init_options(root));
  kc::storage::Database database(initialized.state_path);
  const auto fixture=seed_create_proposal(root,database);
  const auto article=root/"vault/Markov Chains.md";
  write_file(article,"Original untracked article\n");
  kc::vault::VaultWriter writer(root);
  const auto preview=writer.preview(fixture.proposal_id,{.allow_overwrite_user_file=true});
  writer.approve(fixture.proposal_id,preview.token,{.allow_overwrite_user_file=true});
  terminate_apply(root,fixture.proposal_id,"after_article_replace",true);
  const auto journal_path=std::filesystem::directory_iterator(root/".knowledge-compiler/transactions")->path();
  if (std::string_view(damage)=="unowned") {
    const auto unrelated=root/"vault/Unrelated.md";
    write_file(unrelated,"Do not delete this user-owned file\n");
    auto journal=kc::test::read_json(journal_path); journal["temporary"]="vault/Unrelated.md";
    write_file(journal_path,journal.dump());
    CHECK_THROWS(kc::storage::keep_recovery_conflicts(root));
    CHECK(read_file(unrelated)=="Do not delete this user-owned file\n");
  } else {
    const std::string bytes("user\0\xFF",6);
    write_file(article,bytes);
    CHECK_THROWS(kc::storage::Database(initialized.state_path));
    CHECK_NOTHROW(kc::storage::keep_recovery_conflicts(root));
    CHECK(read_file(article)==bytes);
    CHECK(std::filesystem::is_directory(root/".knowledge-compiler/recovery-conflicts"));
  }
}
#endif

TEST_CASE("restoring identical A bytes after B never resurrects an old approval or restore token", "[apply-recovery]") {
  kc::test::TemporaryDirectory temporary;
  const auto root=temporary.path()/"project";
  const auto initialized=kc::storage::initialize_project(init_options(root));
  kc::storage::Database database(initialized.state_path);
  const auto fixture=seed_create_proposal(root,database);
  kc::vault::VaultWriter writer(root); kc::test::approve_preview(writer,fixture.proposal_id);
  const auto first=writer.apply(fixture.proposal_id);
  const auto bytes=read_file(root/first.vault_path);
  const auto old=seed_update_proposal(database,fixture.page_id,first.article_id,"Old pending update awaiting apply.");
  const auto approval=writer.preview(old); REQUIRE(approval.valid); writer.approve(old,approval.token);
  const auto second=seed_update_proposal(database,fixture.page_id,first.article_id,"Intervening B revision.");
  kc::test::approve_preview(writer,second); static_cast<void>(writer.apply(second));
  const auto restoring=writer.restore_preview(first.apply_run_id);
  static_cast<void>(writer.restore(first.apply_run_id,restoring.at("preview_sha256").get<std::string>()));
  REQUIRE(read_file(root/first.vault_path)==bytes);
  const auto after=writer.preview(old);
  CHECK(after.token!=approval.token);
  CHECK(after.matches_approval==false);
  CHECK_THROWS(writer.apply(old));
  const auto repeated=writer.restore_preview(first.apply_run_id);
  static_cast<void>(writer.restore(first.apply_run_id,repeated.at("preview_sha256").get<std::string>()));
  REQUIRE(read_file(root/first.vault_path)==bytes);
  CHECK_THROWS(writer.restore(first.apply_run_id,repeated.at("preview_sha256").get<std::string>()));
}

#if !defined(_WIN32)
TEST_CASE("a concurrent project opener reconciles committed cleanup without racing the writer", "[apply-recovery]") {
  kc::test::TemporaryDirectory temporary;
  const auto root=temporary.path()/"project";
  const auto initialized=kc::storage::initialize_project(init_options(root));
  kc::storage::Database database(initialized.state_path);
  const auto fixture=seed_create_proposal(root,database);
  pid_t child=-1;
  kc::vault::VaultWriter writer(root,[&](std::string_view point) {
    if (point!="before_cleanup") return;
    int ready[2]{}; REQUIRE(pipe(ready)==0);
    child=fork(); REQUIRE(child>=0);
    if (child==0) {
      close(ready[0]);
      const char signal='r';
      if (write(ready[1],&signal,1)!=1) _exit(78);
      close(ready[1]);
      try {
        kc::storage::Database opened(initialized.state_path);
        if (opened.scalar_integer("SELECT COUNT(*) FROM apply_runs")!=1 ||
            opened.scalar_text("SELECT status FROM proposals")!="applied") _exit(78);
      } catch (...) { _exit(78); }
      _exit(77);
    }
    close(ready[1]);
    char signal=0; REQUIRE(read(ready[0],&signal,1)==1); close(ready[0]);
    // Return to cleanup while the new opener is attempting project recovery.
  });
  kc::test::approve_preview(writer,fixture.proposal_id);
  const auto applied=writer.apply(fixture.proposal_id);
  int status=0; REQUIRE(waitpid(child,&status,0)==child); REQUIRE(WIFEXITED(status)); REQUIRE(WEXITSTATUS(status)==77);
  CHECK(read_file(root/applied.vault_path)==database.scalar_text("SELECT body FROM article_fts"));
  no_active_artifacts(root);
}
#endif
