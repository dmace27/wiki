#include "kc/domain/concept.hpp"
#include "kc/domain/path.hpp"
#include "kc/models/proposal_validator.hpp"
#include "kc/domain/json.hpp"
#include "../test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <string>
#include <vector>

TEST_CASE("concept identity normalizes Unicode case and horizontal whitespace") {
  using kc::domain::concept_identity_key;
  using kc::domain::normalize_concept_title;
  CHECK(normalize_concept_title(" \tCellular\u00a0  Respiration\u2003") ==
        "Cellular Respiration");
  CHECK(normalize_concept_title("Cafe\u0301") == "Café");
  CHECK(concept_identity_key(" STRASSE ") == concept_identity_key("Straße"));
  CHECK(concept_identity_key("БИОЛОГИЯ") == concept_identity_key("Биология"));
  CHECK(concept_identity_key("Σ") == concept_identity_key("ς"));
  CHECK(concept_identity_key("Cafe\u0301") == concept_identity_key("CAFÉ"));
  // Identity keeps punctuation and accents. Lexical/FTS matching is a separate
  // retrieval operation and must never silently merge these concepts.
  CHECK(concept_identity_key("A-B") != concept_identity_key("A B"));
  CHECK(concept_identity_key("Resume") != concept_identity_key("Résumé"));
  CHECK(kc::domain::canonical_concept_slug("Σ") ==
        kc::domain::canonical_concept_slug("ς"));
  CHECK(kc::domain::canonical_concept_slug("量子力学").starts_with("concept-"));
}

TEST_CASE("concept filenames obey portable component rules on every host") {
  const std::vector<std::string> invalid{
      "", " \t ", ".", "..", "Topic.", "Topic. ", "CON", "con.txt",
      "CON .txt", "PRN", "AUX", "NUL", "COM1", "COM9.data", "LPT1", "LPT9.txt",
      "COM¹", "LPT².txt", "COM³", "CONIN$", "CONOUT$",
      "a/b", "a\\b", "a<b", "a>b", "a:b", "a\"b", "a|b", "a?b", "a*b",
      "a\nb", "a\rb", "a\u2028b", "a\u2029b", "a\u200bb",
      std::string("a\0b", 3), std::string("\xc0\xaf", 2),
      std::string("\xed\xa0\x80", 3), std::string(201, 'x')};
  for (const auto& title : invalid) {
    INFO("title bytes: " << title.size());
    CHECK_THROWS_AS(kc::domain::normalize_concept_title(title), std::invalid_argument);
  }
  const std::vector<std::string> valid{
      "C++", "C#", "Bayes' Rule", "Biology", "Résumé", "量子力学",
      "COM0", "COM10", "LPT10", "Connection", "!!!", std::string(200, 'x')};
  for (const auto& title : valid) {
    CHECK(kc::domain::normalize_concept_title(title) == title);
    const auto filename = kc::domain::concept_filename(title);
    CHECK(filename.filename() == filename);
    const auto utf8 = filename.u8string();
    CHECK(std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size()) == title + ".md");
  }
}

TEST_CASE("Unicode lexical matching preserves letters and never invents empty words") {
  CHECK(kc::domain::normalize_lexical_text("CAFÉ / Cafe\u0301") == "café café");
  CHECK(kc::domain::normalize_lexical_text("БИОЛОГИЯ: клетки") == "биология клетки");
  CHECK(kc::domain::normalize_lexical_text("量子力学") == "量子力学");
  CHECK(kc::domain::normalize_lexical_text("C++ and C#") == "c plus plus and c sharp");
  CHECK(kc::domain::normalize_lexical_text("--- 🧠 ...").empty());
}

TEST_CASE("proposal parsing and model requests share portable title validation") {
  const auto fixture = kc::test::read_json(
      std::filesystem::path(KC_TEST_FIXTURES_DIR) / "article-proposal.valid.json");
  kc::models::ProposalValidator validator;
  for (const auto& title : {"CON", "Topic.", "Topic?", "Line\nBreak"}) {
    auto payload = fixture;
    payload["article"]["title"] = title;
    CHECK_FALSE(kc::domain::parse_article_proposal(payload));
    CHECK_FALSE(validator.validate_request({.concept_title = title}));
  }
}

TEST_CASE("recorded article filenames are checked without changing their spelling") {
  for (const auto& filename : {"CON.md", "Topic.md.", "Topic.md ", "Topic?.md", "Topic\tName.md"}) {
    CHECK_THROWS_AS(kc::domain::validate_concept_filename(
                        kc::domain::path_from_utf8(filename)), std::invalid_argument);
  }
  CHECK_NOTHROW(kc::domain::validate_concept_filename(
      kc::domain::path_from_utf8("vault/Cafe\u0301.md")));
  CHECK_NOTHROW(kc::domain::validate_concept_filename(
      kc::domain::path_from_utf8("vault/" + std::string(200, 'x') + ".md")));
}
