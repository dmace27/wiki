#include "kc/domain/id.hpp"
#include "kc/extraction/extractor.hpp"
#include "kc/import/sha256.hpp"
#include "kc/import/source_importer.hpp"
#include "kc/review/review_service.hpp"
#include "kc/storage/database.hpp"
#include "kc/storage/project_initializer.hpp"
#include "../test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace {

kc::storage::InitOptions init_options(const std::filesystem::path& root) {
  return {.project_root = root,
          .vault_path = "vault",
          .migration_directory = KC_TEST_MIGRATIONS_DIR};
}

void write_file(const std::filesystem::path& path,
                const std::string_view content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << content;
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

/// Quote a test-controlled string for direct SQL fixture construction.
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

struct ReviewFixture {
  kc::source_import::ImportResult imported;
  kc::domain::ExtractedPage page;
  kc::domain::ProposalId proposal_id;
};

ReviewFixture seed_review_fixture(const std::filesystem::path& root,
                                  kc::storage::Database& database) {
  const auto source_path = root / "incoming" / "Probability Notes.txt";
  write_file(source_path, "A Markov chain has the Markov property.\n");
  kc::source_import::SourceImporter importer(root);
  auto imported = importer.import_file(source_path);
  kc::extraction::Extractor extractor(root);
  const auto extracted = extractor.extract(imported.source.source_id);
  REQUIRE(extracted.pages.size() == 1U);
  auto page = extracted.pages.front();

  // A retained image path makes the review test exercise the same combined
  // image/text/status contract used for PDFs without requiring Poppler.
  const auto image_path = std::filesystem::path(".knowledge-compiler/pages") /
                          page.page_id.value / "page-0001.png";
  write_file(root / image_path, "review image fixture");
  database.execute("UPDATE source_pages SET image_path = " +
                   sql_string(image_path.generic_string()) +
                   " , image_sha256 = " + sql_string(kc::source_import::sha256_text("review image fixture")) +
                   " WHERE page_id = " + sql_string(page.page_id.value));
  page.image_path = image_path;

  const nlohmann::json payload = {
      {"schema_version", 1},
      {"operation", "create_article"},
      {"article",
       {{"title", "Markov Chains"},
        {"slug", "markov-chains"},
        {"aliases", nlohmann::json::array({"Markov chain"})}}},
      {"sections",
       nlohmann::json::array(
           {{{"key", "working_explanation"},
             {"heading", "My working explanation"},
             {"blocks",
              nlohmann::json::array(
                  {{{"kind", "paragraph"},
                    {"text", "A Markov chain moves between states."},
                    {"citations",
                     nlohmann::json::array(
                         {{{"page_id", page.page_id.value},
                           {"start_char", 2},
                           {"end_char", 14},
                           {"quote", "Markov chain"}}})}}})}}})},
      {"related_concepts",
       nlohmann::json::array(
           {{{"title", "Conditional Probability"},
             {"reason", "Transitions use conditional probabilities."},
             {"citations",
              nlohmann::json::array(
                  {{{"page_id", page.page_id.value},
                    {"start_char", 2},
                    {"end_char", 14},
                    {"quote", "Markov chain"}}})}}})}};

  const auto proposal_id = kc::domain::generate_proposal_id();
  database.execute(
      "INSERT INTO proposals(proposal_id, operation, payload_json, status, "
      "created_at) VALUES (" +
      sql_string(proposal_id.value) + ", 'create_article', " +
      sql_string(payload.dump()) + ", 'pending', '2026-08-11T12:00:00Z')");
  database.execute(
      "INSERT INTO proposal_citations(proposal_id, section_key, block_index, "
      "page_id, start_char, end_char, quote) VALUES (" +
      sql_string(proposal_id.value) + ", 'working_explanation', 0, " +
      sql_string(page.page_id.value) + ", 2, 14, 'Markov chain')");
  database.execute(
      "INSERT INTO proposal_citations(proposal_id, section_key, block_index, "
      "page_id, start_char, end_char, quote) VALUES (" +
      sql_string(proposal_id.value) + ", 'related_concepts', 0, " +
      sql_string(page.page_id.value) + ", 2, 14, 'Markov chain')");

  return {.imported = std::move(imported),
          .page = std::move(page),
          .proposal_id = proposal_id};
}

}  // namespace

TEST_CASE("extraction review keeps image text and status together and corrects safely") {
  kc::test::TemporaryDirectory temporary;
  const auto root = temporary.path() / "project";
  const auto initialized = kc::storage::initialize_project(init_options(root));
  kc::storage::Database database(initialized.state_path);

  const auto source_path = root / "incoming" / "Probability Notes.txt";
  write_file(source_path, "OCR says Markov chaim.\n");
  kc::source_import::SourceImporter importer(root);
  const auto imported = importer.import_file(source_path);
  kc::extraction::Extractor extractor(root);
  const auto extracted = extractor.extract(imported.source.source_id);
  REQUIRE(extracted.pages.size() == 1U);
  const auto image_path = std::filesystem::path(".knowledge-compiler/pages") /
                          extracted.pages.front().page_id.value / "page-0001.png";
  write_file(root / image_path, "review image fixture");
  database.execute("UPDATE source_pages SET image_path = " +
                   sql_string(image_path.generic_string()) +
                   " , image_sha256 = " + sql_string(kc::source_import::sha256_text("review image fixture")) +
                   " WHERE page_id = " +
                   sql_string(extracted.pages.front().page_id.value));

  kc::review::ReviewService reviewer(root);
  const auto before = reviewer.review_extraction(imported.source.source_id);
  REQUIRE(before.pages.size() == 1U);
  CHECK(before.pages.front().image_path == image_path);
  CHECK(before.pages.front().text == "OCR says Markov chaim.\n");
  CHECK(before.pages.front().text_status == kc::domain::TextStatus::native);

  const std::string corrected = "These notes explain a Markov chain.\n";
  const auto correction = reviewer.correct_page_text(
      imported.source.source_id, 1, corrected);
  CHECK(correction.page_id == extracted.pages.front().page_id);
  CHECK(correction.text_sha256 == kc::source_import::sha256_text(corrected));

  const auto after = reviewer.review_extraction(imported.source.source_id);
  CHECK(after.pages.front().image_path == image_path);
  CHECK(after.pages.front().text == corrected);
  CHECK(after.pages.front().text_status == kc::domain::TextStatus::reviewed);
  CHECK_THROWS_AS(reviewer.correct_page_text(imported.source.source_id, 1, " \n"),
                  kc::review::ReviewError);
}

TEST_CASE("proposal review exposes cited pages and review never edits articles") {
  kc::test::TemporaryDirectory temporary;
  const auto root = temporary.path() / "project";
  const auto initialized = kc::storage::initialize_project(init_options(root));
  kc::storage::Database database(initialized.state_path);
  const auto fixture = seed_review_fixture(root, database);

  const auto user_article = root / "vault" / "My Article.md";
  const std::string original_article = "# My article\n\nUser-owned bytes.\n";
  write_file(user_article, original_article);

  kc::review::ReviewService reviewer(root);
  const auto review = reviewer.review_proposal(fixture.proposal_id);
  CHECK(review.summary.status == kc::domain::ProposalStatus::pending);
  CHECK(review.proposal.sections.front().heading == "My working explanation");
  REQUIRE(review.citation_evidence.size() == 2U);
  CHECK(review.citation_evidence.front().page_number == 1U);
  CHECK(review.citation_evidence.front().image_path == fixture.page.image_path);
  CHECK(review.citation_evidence.front().extracted_text == fixture.page.text);
  CHECK(review.citation_evidence.front().citation.quote == "Markov chain");

  const auto pending =
      reviewer.list_proposals(kc::domain::ProposalStatus::pending);
  REQUIRE(pending.size() == 1U);
  CHECK(pending.front().proposal_id == fixture.proposal_id);

  reviewer.reject_proposal(fixture.proposal_id, "Needs a clearer example");
  const auto rejected =
      reviewer.list_proposals(kc::domain::ProposalStatus::rejected);
  REQUIRE(rejected.size() == 1U);
  CHECK(rejected.front().review_reason == "Needs a clearer example");
  CHECK(read_file(user_article) == original_article);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM articles") == 0);
  CHECK_THROWS_AS(
      reviewer.reject_proposal(fixture.proposal_id, "second decision"),
      kc::review::ReviewError);

  // Rejected evidence remains immutable, but correction gets a new current
  // identity so the extraction error can be fixed without losing history.
  const auto correction = reviewer.correct_page_text(
      fixture.imported.source.source_id, 1, "Changed later");
  CHECK(correction.page_id != fixture.page.page_id);
  CHECK(correction.previous_page_id == fixture.page.page_id);
  CHECK(reviewer.review_proposal(fixture.proposal_id).citation_evidence.front().extracted_text == fixture.page.text);
  CHECK(reviewer.review_extraction(fixture.imported.source.source_id).pages.front().text == "Changed later");
  CHECK(read_file(user_article) == original_article);
}

namespace {
/// Record direct argument vectors without starting the user's GUI viewer.
class ImageOpenRunner final : public kc::extraction::CommandRunner {
 public:
  std::string program;
  std::vector<std::string> arguments;
  int result{0};
  int run(std::string_view executable, const std::vector<std::string>& args) override {
    program = executable;
    arguments = args;
    return result;
  }
};
}  // namespace

TEST_CASE("OCR acceptance keeps exact inspected text and image opening uses verified absolute argv", "[review-preview]") {
  kc::test::TemporaryDirectory temporary;
  const auto root = temporary.path() / "project with spaces";
  const auto initialized = kc::storage::initialize_project(init_options(root));
  kc::storage::Database database(initialized.state_path);
  const auto fixture = seed_review_fixture(root,database);
  // Delete synthetic proposals so this case exercises an uncited OCR page.
  database.execute("DELETE FROM proposal_citations; DELETE FROM proposals; UPDATE source_pages SET text_status='ocr_unreviewed'");
  kc::review::ReviewService reviewer(root);
  const auto before = reviewer.review_extraction(fixture.imported.source.source_id);
  REQUIRE(before.pages.size() == 1U);
  const auto accepted = reviewer.accept_page_text(fixture.imported.source.source_id,1);
  CHECK(accepted.page_id == before.pages.front().page_id);
  CHECK(accepted.text_sha256 == kc::source_import::sha256_text(before.pages.front().text));
  const auto after = reviewer.review_extraction(fixture.imported.source.source_id);
  CHECK(after.pages.front().text == before.pages.front().text);
  CHECK(after.pages.front().text_status == kc::domain::TextStatus::reviewed);
  CHECK(reviewer.accept_page_text(fixture.imported.source.source_id,1).page_id == accepted.page_id);
  CHECK(database.scalar_integer("SELECT evidence_revision FROM source_pages") == 1);
  ImageOpenRunner opener;
  const auto image = reviewer.open_page_image(fixture.imported.source.source_id,1,opener);
  CHECK(image.is_absolute());
  CHECK(image == std::filesystem::absolute(root / *fixture.page.image_path));
  CHECK(opener.arguments.back() == image.string());
#if defined(__APPLE__)
  CHECK(opener.program == "/usr/bin/open");
  CHECK(opener.arguments.front() == "--");
#endif
  // Opening a captured inspection uses its matching image/text even if a
  // later extraction points the current page at a different retained image.
  const auto newer_image = root / ".knowledge-compiler/pages/newer.png";
  write_file(newer_image,"review image fixture");
  database.execute("UPDATE source_pages SET image_path='.knowledge-compiler/pages/newer.png'");
  CHECK(reviewer.open_page_image(after.pages.front(),opener) == image);
  CHECK(opener.arguments.back() == image.string());
  opener.result = 1;
  CHECK_THROWS_AS(reviewer.open_page_image(fixture.imported.source.source_id,1,opener),kc::review::ReviewError);
  CHECK_THROWS_AS(reviewer.page_image(fixture.imported.source.source_id,2),kc::review::ReviewError);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM apply_runs") == 0);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM articles") == 0);
}

TEST_CASE("cited OCR corrections preserve rejected and superseded evidence while selecting the new page", "[review-preview]") {
  const auto status = GENERATE("pending","approved","rejected","superseded");
  kc::test::TemporaryDirectory temporary;
  const auto root = temporary.path() / "project";
  const auto initialized = kc::storage::initialize_project(init_options(root));
  kc::storage::Database database(initialized.state_path);
  const auto fixture = seed_review_fixture(root,database);
  database.execute("UPDATE proposals SET status = " + sql_string(status));
  kc::review::ReviewService reviewer(root);
  const auto correction = reviewer.correct_page_text(fixture.imported.source.source_id,1,"Corrected Markov chain learning.\n");
  CHECK(correction.previous_page_id == fixture.page.page_id);
  CHECK(correction.page_id != fixture.page.page_id);
  CHECK(database.scalar_text("SELECT text FROM source_pages WHERE is_current=0") == fixture.page.text);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM source_pages WHERE is_current=1") == 1);
  CHECK(reviewer.review_extraction(fixture.imported.source.source_id).pages.front().page_id == correction.page_id);
  const auto history = reviewer.review_proposal(fixture.proposal_id);
  CHECK(history.citation_evidence.front().citation.page_id == fixture.page.page_id);
  CHECK(history.citation_evidence.front().extracted_text == fixture.page.text);
  CHECK(history.citation_evidence.front().image_path == fixture.page.image_path);
  CHECK(std::filesystem::exists(root / *fixture.page.image_path));
  CHECK_THROWS_AS(reviewer.correct_page_text(fixture.imported.source.source_id,1,"Unexpected stale acceptance",fixture.page.page_id),kc::review::ReviewError);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM articles") == 0);
}
