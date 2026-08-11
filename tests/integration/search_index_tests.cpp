#include "kc/search/search_index.hpp"
#include "kc/storage/database.hpp"
#include "kc/storage/project_initializer.hpp"
#include "../test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

namespace {

kc::storage::InitOptions init_options(const std::filesystem::path& root) {
  return {.project_root = root,
          .vault_path = "vault",
          .migration_directory = KC_TEST_MIGRATIONS_DIR};
}

/// Seed applied-article state directly so these focused tests can exercise
/// ranking and literal query handling independently of the workflow test.
void seed_article(kc::storage::Database& database) {
  database.execute(
      "INSERT INTO articles(article_id, title, slug, vault_path, created_at, "
      "updated_at) VALUES "
      "('art_01J00000000000000000000000', 'Markov Chains', "
      "'markov-chains', 'vault/Markov Chains.md', "
      "'2026-01-01T00:00:00Z', '2026-01-01T00:00:00Z'), "
      "('art_01J00000000000000000000001', 'Stochastic Processes', "
      "'stochastic-processes', 'vault/Stochastic Processes.md', "
      "'2026-01-01T00:00:00Z', '2026-01-01T00:00:00Z');");
  database.execute(
      "INSERT INTO article_fts(article_id, title, aliases, body) VALUES "
      "('art_01J00000000000000000000000', 'Markov Chains', "
      "'Markov chain', 'A process moves between states.'), "
      "('art_01J00000000000000000000001', 'Stochastic Processes', '', "
      "'This survey mentions Markov chains in a larger family.');");
}

}  // namespace

TEST_CASE("SQLite FTS search returns concept title and vault path") {
  kc::test::TemporaryDirectory temporary;
  const auto project_root = temporary.path() / "project";
  const auto initialized =
      kc::storage::initialize_project(init_options(project_root));
  kc::storage::Database database(initialized.state_path);
  seed_article(database);

  kc::search::SearchIndex index(project_root);
  const auto results = index.search("markov chains");

  REQUIRE(results.size() == 2U);
  CHECK(results.front().title == "Markov Chains");
  CHECK(results.front().vault_path == "vault/Markov Chains.md");
  CHECK_FALSE(results.front().vault_path.is_absolute());
  CHECK_FALSE(results.front().excerpt.empty());
}

TEST_CASE("search treats punctuation as literals and enforces limits") {
  kc::test::TemporaryDirectory temporary;
  const auto project_root = temporary.path() / "project";
  const auto initialized =
      kc::storage::initialize_project(init_options(project_root));
  kc::storage::Database database(initialized.state_path);
  seed_article(database);
  kc::search::SearchIndex index(project_root);

  // Characters that are FTS5 operators in a raw MATCH expression must not
  // cause a syntax error or broaden this user-controlled query.
  const auto literal = index.search("\"markov\" chains*", 1U);
  REQUIRE(literal.size() == 1U);
  CHECK(literal.front().title == "Markov Chains");

  // OR is a literal word here, not an operator that broadens the result.
  CHECK(index.search("markov OR chains").empty());

  CHECK_THROWS_AS(index.search("---"), kc::search::SearchError);
  CHECK_THROWS_AS(index.search("markov", 0U), kc::search::SearchError);
  CHECK_THROWS_AS(index.search("markov", 101U), kc::search::SearchError);
}
