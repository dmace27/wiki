#include "kc/domain/json.hpp"
#include "kc/models/proposal_validator.hpp"
#include "../test_support.hpp"

#include <catch2/catch_test_macros.hpp>

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

TEST_CASE("the MVP request rejects concepts other than Markov Chains") {
  kc::models::ProposalValidator validator;
  auto request = request_for(fixture_page());
  request.concept_title = "Bayes' Rule";

  CHECK_FALSE(validator.validate_request(request));
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
