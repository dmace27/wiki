#include "kc/domain/json.hpp"
#include "kc/domain/markdown.hpp"
#include "kc/models/concept.hpp"
#include "kc/models/proposal_validator.hpp"
#include "../test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <filesystem>

namespace {

kc::domain::ArticleProposal fixture_proposal() {
  const auto parsed = kc::domain::parse_article_proposal(
      kc::test::read_json(
          std::filesystem::path(KC_TEST_FIXTURES_DIR) /
          "article-proposal.valid.json"));
  REQUIRE(parsed.valid());
  return *parsed.value;
}

kc::domain::ExtractedPage fixture_page() {
  const auto parsed = kc::domain::parse_extracted_page(
      kc::test::read_json(
          std::filesystem::path(KC_TEST_FIXTURES_DIR) /
          "extracted-page.valid.json"));
  REQUIRE(parsed.valid());
  return *parsed.value;
}

kc::models::ProposalGenerationRequest request_for(
    kc::domain::ExtractedPage page) {
  return {
      .concept_title = "Markov Chains",
      .operation = kc::domain::ProposalOperation::create_article,
      .pages = {std::move(page)}};
}

}  // namespace

TEST_CASE("proposal citations are checked against exact evidence spans") {
  kc::models::ProposalValidator validator;
  auto proposal = fixture_proposal();
  const auto request = request_for(fixture_page());

  CHECK(validator.validate_request(request));
  CHECK(validator.validate_response(proposal, request));

  proposal.sections[0].blocks[0].citations[0].quote = "different text";
  CHECK_FALSE(validator.validate_response(proposal, request));
}

TEST_CASE("citation comparison normalizes whitespace but enforces bounds") {
  kc::models::ProposalValidator validator;
  auto page = fixture_page();
  page.text = "A Markov \n chain has a property.";
  auto proposal = fixture_proposal();
  proposal.sections[0].blocks[0].citations[0].end_char = 16;
  proposal.sections[0].blocks[0].citations[0].quote = "Markov chain";
  proposal.related_concepts[0].citations[0] =
      proposal.sections[0].blocks[0].citations[0];
  const auto request = request_for(page);

  CHECK(validator.validate_response(proposal, request));

  proposal.sections[0].blocks[0].citations[0].end_char = 10'000;
  CHECK_FALSE(validator.validate_response(proposal, request));
}

TEST_CASE("requests accept arbitrary safe concept titles") {
  kc::models::ProposalValidator validator;
  auto request = request_for(fixture_page());
  request.concept_title = "Bayes' Rule";

  CHECK(validator.validate_request(request));

  request.concept_title = "../unsafe";
  CHECK_FALSE(validator.validate_request(request));
}

TEST_CASE("proposal title must match the requested concept") {
  kc::models::ProposalValidator validator;
  const auto request = request_for(fixture_page());
  auto proposal = fixture_proposal();
  proposal.article.title = "Unrelated Topic";

  CHECK_FALSE(validator.validate_response(proposal, request));
}

TEST_CASE("concept slugs are generic and deterministic") {
  CHECK(kc::models::canonical_concept_slug("Bayes' Rule") == "bayes-rule");
  CHECK(kc::models::canonical_concept_slug("C++") == "c-plus-plus");
  const auto non_latin = kc::models::canonical_concept_slug("量子力学");
  CHECK(non_latin.starts_with("concept-"));
  CHECK(non_latin.size() == 24U);
}

TEST_CASE("model requests accept only usable extraction review states") {
  kc::models::ProposalValidator validator;
  auto page = fixture_page();

  page.text_status = kc::domain::TextStatus::native;
  CHECK(validator.validate_request(request_for(page)));

  page.text_status = kc::domain::TextStatus::reviewed;
  CHECK(validator.validate_request(request_for(page)));

  page.text_status = kc::domain::TextStatus::ocr_unreviewed;
  CHECK_FALSE(validator.validate_request(request_for(page)));

  page.text_status = kc::domain::TextStatus::failed;
  CHECK_FALSE(validator.validate_request(request_for(page)));
}

TEST_CASE("citation offsets are UTF-8 byte offsets") {
  kc::models::ProposalValidator validator;
  auto page = fixture_page();
  page.text = "\u03c0AMarkov chain example";
  auto proposal = fixture_proposal();
  auto& block_citation = proposal.sections[0].blocks[0].citations[0];
  block_citation.start_char = 3;
  block_citation.end_char = 15;
  block_citation.quote = "Markov chain";
  proposal.related_concepts[0].citations[0] = block_citation;

  CHECK(validator.validate_response(proposal, request_for(page)));

  // A character-counted start of 2 lands on the preceding ASCII byte because
  // the pi character occupies two UTF-8 bytes.
  block_citation.start_char = 2;
  CHECK_FALSE(validator.validate_response(proposal, request_for(page)));
}

TEST_CASE("Markdown headings and relationship destinations fail shared validation before review") {
  const auto field = GENERATE("heading", "related", "alias");
  const auto text = GENERATE("", "   ", "line\nbreak", "line\rbreak", "line\u2028break");
  auto proposal = fixture_proposal();
  if (std::string_view(field) == "heading") {
    proposal.sections.front().heading = text;
  } else if (std::string_view(field) == "related") {
    proposal.related_concepts.front().title = text;
  } else {
    proposal.article.aliases = {text};
  }
  CHECK_FALSE(kc::domain::validate(proposal));
  kc::models::ProposalValidator validator;
  CHECK_FALSE(validator.validate_response(proposal, request_for(fixture_page())));
}

TEST_CASE("relationship targets cannot inject wiki link aliases headings or embeds") {
  const auto title = GENERATE("Other]] [[Fake", "Page|Fake", "Page#Section",
                              "Page^block", "../Outside", "CON", "<img>");
  auto proposal = fixture_proposal();
  proposal.related_concepts.front().title = title;
  CHECK_FALSE(kc::domain::validate(proposal));
}

TEST_CASE("reserved markers and unsafe Unicode are rejected in every proposal text position") {
  const auto field = GENERATE("title", "alias", "heading", "block", "related", "reason", "quote");
  const std::string text = GENERATE(std::string("<!-- kc:managed:end -->"),
                                    std::string("A\0B", 3), std::string("\xff", 1),
                                    std::string("A\u202eB"));
  auto proposal = fixture_proposal();
  const std::string_view position(field);
  if (position == "title") proposal.article.title = text;
  if (position == "alias") proposal.article.aliases = {text};
  if (position == "heading") proposal.sections.front().heading = text;
  if (position == "block") proposal.sections.front().blocks.front().text = text;
  if (position == "related") proposal.related_concepts.front().title = text;
  if (position == "reason") proposal.related_concepts.front().reason = text;
  if (position == "quote") proposal.sections.front().blocks.front().citations.front().quote = text;
  CHECK_FALSE(kc::domain::validate(proposal));
}

TEST_CASE("model markup remains literal and multiline prose remains one cited block") {
  auto proposal = fixture_proposal();
  proposal.sections.front().blocks.front().text =
      "[Fake source](https://evil.test) <img src=x> ![[embed]]\n"
      "[^fake]: injected\n    ```code```";
  CHECK(kc::domain::validate(proposal));
  const auto rendered = kc::domain::markdown_literal(proposal.sections.front().blocks.front().text);
  CHECK(rendered.find('\n') == std::string::npos);
  CHECK(rendered.find("[Fake source](") == std::string::npos);
  CHECK(rendered.find("![[") == std::string::npos);
  CHECK(rendered.find("[^fake]:") == std::string::npos);
  CHECK(rendered.find("\\<img") != std::string::npos);
  CHECK(rendered.find("```") == std::string::npos);
  CHECK(kc::domain::markdown_literal("α\n\n  β") == "α β");
}

TEST_CASE("configured ownership marker IDs cannot inject Markdown or comment attributes") {
  auto config = kc::domain::parse_project_config(kc::test::read_json(
      std::filesystem::path(KC_TEST_FIXTURES_DIR) / "project-config.valid.json"));
  REQUIRE(config);
  for (const auto& id : {"", "quote\"", "close-->", "line\nbreak", "with space", "日本語"}) {
    config.value->vault.generated_section_id = id;
    CHECK_FALSE(kc::domain::validate(*config.value));
  }
  for (const auto& id : {"knowledge-compiler", "section_2.v1"}) {
    config.value->vault.generated_section_id = id;
    CHECK(kc::domain::validate(*config.value));
  }
}
