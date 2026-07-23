#include "kc/domain/json.hpp"
#include "kc/domain/types.hpp"
#include "../test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>

namespace {

const std::filesystem::path fixtures = KC_TEST_FIXTURES_DIR;

}  // namespace

TEST_CASE("documented JSON fixtures parse into validated domain values") {
  const auto config = kc::domain::parse_project_config(
      kc::test::read_json(fixtures / "project-config.valid.json"));
  const auto page = kc::domain::parse_extracted_page(
      kc::test::read_json(fixtures / "extracted-page.valid.json"));
  const auto citation = kc::domain::parse_citation(
      kc::test::read_json(fixtures / "citation.valid.json"));
  const auto proposal = kc::domain::parse_article_proposal(
      kc::test::read_json(fixtures / "article-proposal.valid.json"));

  REQUIRE(config.valid());
  REQUIRE(page.valid());
  REQUIRE(citation.valid());
  REQUIRE(proposal.valid());
  CHECK(proposal.value->article.title == "Markov Chains");
  CHECK(proposal.value->sections.front().blocks.front().citations.size() == 1U);
}

TEST_CASE("domain JSON round trips without losing contract fields") {
  const auto original_document = kc::test::read_json(fixtures / "article-proposal.valid.json");
  const auto parsed = kc::domain::parse_article_proposal(original_document);
  REQUIRE(parsed.valid());

  const nlohmann::json serialized = *parsed.value;
  CHECK(serialized == original_document);
  CHECK(kc::domain::parse_article_proposal(serialized).valid());
}

TEST_CASE("strict parsing rejects arbitrary model operations and properties") {
  auto document = kc::test::read_json(fixtures / "article-proposal.valid.json");
  document["path"] = "vault/Markov Chains.md";
  CHECK_FALSE(kc::domain::parse_article_proposal(document).valid());

  document.erase("path");
  document["operation"] = "write_file";
  CHECK_FALSE(kc::domain::parse_article_proposal(document).valid());
}

TEST_CASE("every generated block requires a citation") {
  auto document = kc::test::read_json(fixtures / "article-proposal.valid.json");
  document["sections"][0]["blocks"][0]["citations"] = nlohmann::json::array();
  const auto parsed = kc::domain::parse_article_proposal(document);
  REQUIRE_FALSE(parsed.valid());
  REQUIRE_FALSE(parsed.issues.empty());
}

TEST_CASE("project paths cannot escape the project root") {
  auto document = kc::test::read_json(fixtures / "project-config.valid.json");
  document["paths"]["state"] = "../state.sqlite";
  CHECK_FALSE(kc::domain::parse_project_config(document).valid());
}
