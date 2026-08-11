#include "kc/compiler/compiler.hpp"
#include "kc/extraction/extractor.hpp"
#include "kc/import/source_importer.hpp"
#include "kc/models/language_model.hpp"
#include "kc/review/review_service.hpp"
#include "kc/search/search_index.hpp"
#include "kc/storage/database.hpp"
#include "kc/storage/project_initializer.hpp"
#include "kc/vault/vault_writer.hpp"
#include "../test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

kc::storage::InitOptions init_options(const std::filesystem::path& root) {
  return {.project_root = root,
          .vault_path = "vault",
          .migration_directory = KC_TEST_MIGRATIONS_DIR};
}

void write_file(const std::filesystem::path& path,
                const std::string_view content) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << content;
}

/// Synthetic, project-owned PDF fixture adapter. It exercises the complete PDF
/// state path without requiring Poppler in the automated test environment.
class OnePagePdfAdapter final : public kc::extraction::PdfAdapter {
 public:
  [[nodiscard]] std::string_view name() const noexcept override {
    return "synthetic-permitted-pdf";
  }

  [[nodiscard]] std::vector<kc::extraction::RenderedPage> render_pages(
      const std::filesystem::path&,
      const std::filesystem::path& output_directory) override {
    const auto image = output_directory / "page-1.png";
    write_file(image, "synthetic page image owned by this project");
    return {{1U, image}};
  }

  [[nodiscard]] std::optional<std::string> extract_native_text(
      const std::filesystem::path&, std::uint32_t,
      const std::filesystem::path&) override {
    return "Markov chaims move between states.";
  }
};

class UnexpectedOcrProvider final : public kc::extraction::OcrProvider {
 public:
  [[nodiscard]] std::string_view name() const noexcept override {
    return "unexpected-ocr";
  }

  [[nodiscard]] kc::extraction::OcrResult recognize(
      const std::filesystem::path&) override {
    return {.succeeded = false,
            .error_message = "native fixture text should avoid OCR"};
  }
};

/// Deterministic model output still traverses the production prompt, strict
/// JSON parser, evidence validation, compiler transaction, and audit records.
class StaticLanguageModel final : public kc::models::LanguageModel {
 public:
  explicit StaticLanguageModel(std::string response)
      : response_(std::move(response)) {}

  [[nodiscard]] std::string_view provider_name() const noexcept override {
    return "end-to-end-test";
  }

  [[nodiscard]] std::string_view model_name() const noexcept override {
    return "static-v1";
  }

  [[nodiscard]] kc::models::LanguageModelResponse generate(
      const kc::models::LanguageModelRequest&) override {
    return {.content = response_};
  }

 private:
  std::string response_;
};

nlohmann::json proposal_for(const std::string& page_id) {
  const auto citation = nlohmann::json{{"page_id", page_id},
                                      {"start_char", 0},
                                      {"end_char", 13},
                                      {"quote", "Markov chains"}};
  return {
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
                    {"text", "Markov chains move between states."},
                    {"citations", nlohmann::json::array({citation})}}})}}})},
      {"related_concepts",
       nlohmann::json::array(
           {{{"title", "Conditional Probability"},
             {"reason", "Transitions are conditional on the current state."},
             {"citations", nlohmann::json::array({citation})}}})}};
}

}  // namespace

TEST_CASE("import through apply produces a searchable source-linked article") {
  kc::test::TemporaryDirectory temporary;
  const auto project_root = temporary.path() / "project";
  const auto initialized =
      kc::storage::initialize_project(init_options(project_root));

  // Import an independently-created, permitted synthetic fixture and retain
  // it through the same immutable source-version path as a real PDF.
  const auto source_path = temporary.path() / "Probability Notes.pdf";
  write_file(source_path, "%PDF-1.4 synthetic project-owned fixture\n");
  kc::source_import::SourceImporter importer(project_root);
  const auto imported = importer.import_file(source_path);

  kc::extraction::Extractor extractor(
      project_root, std::make_unique<OnePagePdfAdapter>(),
      std::make_unique<UnexpectedOcrProvider>());
  const auto extracted = extractor.extract(imported.source.source_id);
  REQUIRE(extracted.pages.size() == 1U);
  CHECK(extracted.pages.front().image_path.has_value());

  // Exercise the review surface and correction boundary before compilation.
  kc::review::ReviewService reviewer(project_root);
  const auto before_review =
      reviewer.review_extraction(imported.source.source_id);
  REQUIRE(before_review.pages.size() == 1U);
  CHECK(before_review.pages.front().text.find("chaims") != std::string::npos);
  const auto correction = reviewer.correct_page_text(
      imported.source.source_id, 1U,
      "Markov chains move between states using transition probabilities.");
  const auto after_review =
      reviewer.review_extraction(imported.source.source_id);
  CHECK(after_review.pages.front().text_status ==
        kc::domain::TextStatus::reviewed);

  kc::compiler::Compiler compiler(
      project_root, std::make_unique<StaticLanguageModel>(
                        proposal_for(correction.page_id.value).dump()));
  const auto compiled = compiler.compile({});
  CHECK(compiled.selected_page_count == 1U);

  kc::storage::Database database(initialized.state_path);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM article_fts") == 0);
  kc::vault::VaultWriter writer(project_root);
  writer.approve(compiled.proposal_id);
  // Approval is review state only; indexing must wait for a successful apply.
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM article_fts") == 0);
  const auto applied = writer.apply(compiled.proposal_id);

  kc::search::SearchIndex search(project_root);
  const auto results = search.search("markov chains");
  REQUIRE(results.size() == 1U);
  CHECK(results.front().article_id == applied.article_id);
  CHECK(results.front().title == "Markov Chains");
  CHECK(results.front().vault_path == "vault/Markov Chains.md");

  const auto article_path = project_root / results.front().vault_path;
  REQUIRE(std::filesystem::is_regular_file(article_path));
  std::ifstream article(article_path, std::ios::binary);
  const std::string markdown{std::istreambuf_iterator<char>(article),
                             std::istreambuf_iterator<char>()};
  CHECK(markdown.find("#page=1") != std::string::npos);
  CHECK(markdown.find("Probability Notes.pdf, p. 1") != std::string::npos);
  CHECK(database.scalar_integer(
            "SELECT COUNT(*) FROM proposals WHERE status = 'applied'") == 1);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM apply_runs") == 1);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM article_fts") == 1);
}
