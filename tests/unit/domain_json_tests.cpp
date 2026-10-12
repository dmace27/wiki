#include "kc/domain/json.hpp"
#include "kc/domain/types.hpp"
#include "../test_support.hpp"
#include "../contract_cases.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <limits>

namespace {

const std::filesystem::path fixtures = KC_TEST_FIXTURES_DIR;

}  // namespace

// The example files in tests/fixtures are executable documentation: each must
// parse into its matching strongly typed C++ value.
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

// Serialization must preserve every field required by the public contract.
TEST_CASE("domain JSON round trips without losing contract fields") {
  const auto original_document = kc::test::read_json(fixtures / "article-proposal.valid.json");
  const auto parsed = kc::domain::parse_article_proposal(original_document);
  REQUIRE(parsed.valid());

  const nlohmann::json serialized = *parsed.value;
  CHECK(serialized == original_document);
  CHECK(kc::domain::parse_article_proposal(serialized).valid());
}

// Strict parsing is a security boundary between model output and application
// behavior; unknown keys and arbitrary operations must be rejected.
TEST_CASE("strict parsing rejects arbitrary model operations and properties") {
  auto document = kc::test::read_json(fixtures / "article-proposal.valid.json");
  document["path"] = "vault/Markov Chains.md";
  CHECK_FALSE(kc::domain::parse_article_proposal(document).valid());

  document.erase("path");
  document["operation"] = "write_file";
  CHECK_FALSE(kc::domain::parse_article_proposal(document).valid());
}

// Generated prose without evidence violates the product's provenance promise.
TEST_CASE("every generated block requires a citation") {
  auto document = kc::test::read_json(fixtures / "article-proposal.valid.json");
  document["sections"][0]["blocks"][0]["citations"] = nlohmann::json::array();
  const auto parsed = kc::domain::parse_article_proposal(document);
  REQUIRE_FALSE(parsed.valid());
  REQUIRE_FALSE(parsed.issues.empty());
}

// Persisted paths must remain portable and confined to the project directory.
TEST_CASE("project paths cannot escape the project root") {
  auto document = kc::test::read_json(fixtures / "project-config.valid.json");
  document["paths"]["state"] = "../state.sqlite";
  CHECK_FALSE(kc::domain::parse_project_config(document).valid());
}

TEST_CASE("direct JSON conversion cannot bypass shared semantic contract checks") {
  for (const auto& fixture : kc::test::contract_cases()) {
    CAPTURE(fixture.at("name"));
    const auto contract = kc::test::case_contract(fixture);
    const auto document = kc::test::case_document(fixture);
    const bool valid = fixture.at("runtime_valid").get<bool>();
    const auto convert = [&] {
      switch (contract) {
        case kc::domain::JsonContract::project_config:
          static_cast<void>(document.get<kc::domain::ProjectConfig>()); break;
        case kc::domain::JsonContract::extracted_page:
          static_cast<void>(document.get<kc::domain::ExtractedPage>()); break;
        case kc::domain::JsonContract::citation:
          static_cast<void>(document.get<kc::domain::Citation>()); break;
        case kc::domain::JsonContract::article_proposal:
          static_cast<void>(document.get<kc::domain::ArticleProposal>()); break;
      }
    };
    if (valid) {
      CHECK_NOTHROW(convert());
    } else {
      CHECK_THROWS(convert());
    }
  }
}

TEST_CASE("numeric wire syntax is checked before narrowing and preserves integer boundaries") {
  const auto original = kc::test::read_json(fixtures / "project-config.valid.json");
  for (const auto& token : {"1.0", "1e0", "1E+0", "1.9", "-1", "true", "\"1\"",
                            "18446744073709551616", "1e100"}) {
    auto config = original;
    config["schema_version"] = nlohmann::json::parse(token);
    const auto parsed = kc::domain::parse_project_config(config);
    REQUIRE_FALSE(parsed);
    REQUIRE_FALSE(parsed.issues.empty());
    CHECK(parsed.issues.front().path == "/schema_version");
  }
  // A programmatically constructed signed positive integer and a wire-parsed
  // unsigned integer both represent the accepted integer primitive.
  auto config = original;
  config["schema_version"] = std::int64_t{1};
  CHECK(kc::domain::parse_project_config(config));
  config["schema_version"] = std::uint64_t{1};
  CHECK(kc::domain::parse_project_config(config));
  auto citation = kc::test::read_json(fixtures / "citation.valid.json");
  citation["start_char"] = nlohmann::json::parse("-0");
  CHECK(kc::domain::parse_citation(citation));
  citation["start_char"] = -0.0;
  CHECK_FALSE(kc::domain::parse_citation(citation));
}

TEST_CASE("typed proposal validation rejects invalid content enums and storage offsets") {
  const auto parsed = kc::domain::parse_article_proposal(
      kc::test::read_json(fixtures / "article-proposal.valid.json"));
  REQUIRE(parsed);
  auto proposal = *parsed.value;
  proposal.sections.front().blocks.clear();
  CHECK_FALSE(kc::domain::validate(proposal));
  proposal = *parsed.value;
  proposal.operation = static_cast<kc::domain::ProposalOperation>(99);
  CHECK_FALSE(kc::domain::validate(proposal));
  proposal = *parsed.value;
  proposal.sections.front().key = static_cast<kc::domain::SectionKey>(99);
  CHECK_FALSE(kc::domain::validate(proposal));
  proposal = *parsed.value;
  proposal.sections.front().blocks.front().kind = static_cast<kc::domain::BlockKind>(99);
  CHECK_FALSE(kc::domain::validate(proposal));
  proposal = *parsed.value;
  proposal.sections.front().blocks.front().citations.front().end_char =
      std::numeric_limits<std::size_t>::max();
  CHECK_FALSE(kc::domain::validate(proposal));
}
