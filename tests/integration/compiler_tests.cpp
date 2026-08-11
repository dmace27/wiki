#include "kc/compiler/compiler.hpp"
#include "kc/extraction/extractor.hpp"
#include "kc/import/source_importer.hpp"
#include "kc/models/language_model.hpp"
#include "kc/storage/database.hpp"
#include "kc/storage/project_initializer.hpp"
#include "../test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

kc::storage::InitOptions init_options(const std::filesystem::path& root) {
  return {
      .project_root = root,
      .vault_path = "vault",
      .migration_directory = KC_TEST_MIGRATIONS_DIR};
}

void write_file(const std::filesystem::path& path,
                const std::string_view content) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << content;
}

struct ModelState {
  std::string response;
  std::vector<std::string> prompts;
};

/// A deterministic local model double that still passes through ModelRunner's
/// real strict parsing, citation validation, and model-run auditing.
class StaticLanguageModel final : public kc::models::LanguageModel {
 public:
  explicit StaticLanguageModel(std::shared_ptr<ModelState> state)
      : state_(std::move(state)) {}

  [[nodiscard]] std::string_view provider_name() const noexcept override {
    return "compiler-test";
  }

  [[nodiscard]] std::string_view model_name() const noexcept override {
    return "static-v1";
  }

  [[nodiscard]] kc::models::LanguageModelResponse generate(
      const kc::models::LanguageModelRequest& request) override {
    state_->prompts.push_back(request.user_prompt);
    return {.content = state_->response};
  }

 private:
  std::shared_ptr<ModelState> state_;
};

nlohmann::json create_proposal(const std::string& page_id,
                               const std::size_t end_char = 14U) {
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
                    {"text", "A Markov chain moves between states."},
                    {"citations",
                     nlohmann::json::array(
                         {{{"page_id", page_id},
                           {"start_char", 2},
                           {"end_char", end_char},
                           {"quote", "Markov chain"}}})}}})}}})},
      {"related_concepts",
       nlohmann::json::array(
           {{{"title", "Conditional Probability"},
             {"reason", "Transitions use conditional probabilities."},
             {"citations",
              nlohmann::json::array(
                  {{{"page_id", page_id},
                    {"start_char", 2},
                    {"end_char", end_char},
                    {"quote", "Markov chain"}}})}}})}};
}

kc::source_import::ImportResult import_and_extract(
    const std::filesystem::path& project_root,
    const std::filesystem::path& source_path,
    const std::string_view content) {
  write_file(source_path, content);
  kc::source_import::SourceImporter importer(project_root);
  auto imported = importer.import_file(source_path);
  kc::extraction::Extractor extractor(project_root);
  static_cast<void>(extractor.extract(imported.source.source_id));
  return imported;
}

}  // namespace

TEST_CASE("compiler evidence selection is relevant, latest-only, and deterministic") {
  kc::test::TemporaryDirectory temporary;
  const auto project_root = temporary.path() / "project";
  static_cast<void>(
      kc::storage::initialize_project(init_options(project_root)));

  const auto title_source = import_and_extract(
      project_root, temporary.path() / "Markov Chains Overview.md",
      "An overview of a discrete process.\n");
  const auto keyword_source = import_and_extract(
      project_root, temporary.path() / "Lecture 8.md",
      "A transition matrix describes movement between states.\n");
  static_cast<void>(import_and_extract(
      project_root, temporary.path() / "Bayes Notes.md",
      "Posterior distributions update prior beliefs.\n"));

  // Only the latest immutable version of a logical source may be selected.
  const auto changing_path = temporary.path() / "Changing Notes.md";
  static_cast<void>(import_and_extract(
      project_root, changing_path,
      "A Markov chain appeared only in the old revision.\n"));
  static_cast<void>(import_and_extract(
      project_root, changing_path,
      "The current revision discusses unrelated combinatorics.\n"));

  auto state = std::make_shared<ModelState>();
  kc::compiler::Compiler compiler(
      project_root, std::make_unique<StaticLanguageModel>(state));
  const auto pages = compiler.select_evidence({});

  REQUIRE(pages.size() == 2U);
  CHECK(pages[0].source_version_id == title_source.version.source_version_id);
  CHECK(pages[1].source_version_id == keyword_source.version.source_version_id);

  const auto filtered = compiler.select_evidence(
      {.source_ids = {keyword_source.source.source_id}});
  REQUIRE(filtered.size() == 1U);
  CHECK(filtered.front().source_version_id ==
        keyword_source.version.source_version_id);

  CHECK_THROWS_AS(
      compiler.select_evidence(
          {.source_ids = {{"src_01J99999999999999999999999"}}}),
      kc::compiler::CompilerError);
}

TEST_CASE("unreviewed OCR cannot reach the model") {
  kc::test::TemporaryDirectory temporary;
  const auto project_root = temporary.path() / "project";
  const auto initialized =
      kc::storage::initialize_project(init_options(project_root));
  const auto imported = import_and_extract(
      project_root, temporary.path() / "scanned-markov-notes.md",
      "A Markov chain has the Markov property.\n");

  kc::storage::Database database(initialized.state_path);
  const auto version_id = imported.version.source_version_id.value;
  const auto page_id = database.scalar_text(
      "SELECT page_id FROM source_pages WHERE source_version_id = '" +
      version_id + "'");
  database.execute(
      "UPDATE source_pages SET text_status = 'ocr_unreviewed' "
      "WHERE source_version_id = '" +
      version_id + "'");

  auto state = std::make_shared<ModelState>();
  kc::compiler::Compiler compiler(
      project_root, std::make_unique<StaticLanguageModel>(state));
  try {
    static_cast<void>(compiler.compile(
        {.source_ids = {imported.source.source_id}}));
    FAIL("expected unreviewed OCR to produce no eligible evidence");
  } catch (const kc::compiler::CompilerError& error) {
    CHECK(error.kind() == kc::compiler::CompilerErrorKind::no_evidence);
  }
  CHECK(state->prompts.empty());
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM model_runs") == 0);

  // The same extracted text becomes eligible only after explicit review.
  database.execute(
      "UPDATE source_pages SET text_status = 'reviewed' "
      "WHERE source_version_id = '" +
      version_id + "'");
  state->response = create_proposal(page_id).dump();
  const auto compiled = compiler.compile(
      {.source_ids = {imported.source.source_id}});
  CHECK(compiled.selected_page_count == 1U);
  CHECK(state->prompts.size() == 1U);
}

TEST_CASE("compile stores one pending proposal and normalized citations") {
  kc::test::TemporaryDirectory temporary;
  const auto project_root = temporary.path() / "project";
  const auto initialized =
      kc::storage::initialize_project(init_options(project_root));
  const auto imported = import_and_extract(
      project_root, temporary.path() / "probability.md",
      "A Markov \n chain has the Markov property.\n");

  kc::storage::Database database(initialized.state_path);
  const auto page_id = database.scalar_text(
      "SELECT page_id FROM source_pages WHERE source_version_id = '" +
      imported.version.source_version_id.value + "'");
  auto state = std::make_shared<ModelState>();
  state->response = create_proposal(page_id, 16U).dump();
  kc::compiler::Compiler compiler(
      project_root, std::make_unique<StaticLanguageModel>(state));

  const auto first = compiler.compile({});

  CHECK(first.operation ==
        kc::domain::ProposalOperation::create_article);
  CHECK_FALSE(first.article_id.has_value());
  CHECK(first.selected_page_count == 1U);
  CHECK(database.scalar_integer(
            "SELECT COUNT(*) FROM proposals WHERE status = 'pending'") == 1);
  CHECK(database.scalar_integer(
            "SELECT COUNT(*) FROM proposal_citations") == 2);
  CHECK(database.scalar_text(
            "SELECT quote FROM proposal_citations LIMIT 1") ==
        "Markov chain");
  const auto payload = nlohmann::json::parse(
      database.scalar_text("SELECT payload_json FROM proposals"));
  CHECK(payload["sections"][0]["blocks"][0]["citations"][0]["quote"] ==
        "Markov chain");
  CHECK(database.scalar_text("SELECT status FROM model_runs") ==
        "completed");
  CHECK(std::filesystem::is_empty(project_root / "vault"));
  REQUIRE(state->prompts.size() == 1U);
  CHECK(state->prompts.front().find(page_id) != std::string::npos);

  // A newer immutable proposal supersedes earlier review work atomically.
  const auto second = compiler.compile({});
  CHECK(second.proposal_id != first.proposal_id);
  CHECK(database.scalar_integer(
            "SELECT COUNT(*) FROM proposals WHERE status = 'pending'") == 1);
  CHECK(database.scalar_integer(
            "SELECT COUNT(*) FROM proposals WHERE status = 'superseded'") ==
        1);
}

TEST_CASE("invalid generated blocks remain failed model runs, not proposals") {
  kc::test::TemporaryDirectory temporary;
  const auto project_root = temporary.path() / "project";
  const auto initialized =
      kc::storage::initialize_project(init_options(project_root));
  const auto imported = import_and_extract(
      project_root, temporary.path() / "markov-notes.md",
      "A Markov chain has the Markov property.\n");

  kc::storage::Database database(initialized.state_path);
  const auto page_id = database.scalar_text(
      "SELECT page_id FROM source_pages WHERE source_version_id = '" +
      imported.version.source_version_id.value + "'");
  auto invalid = create_proposal(page_id);
  invalid["sections"][0]["blocks"][0]["citations"] =
      nlohmann::json::array();
  auto state = std::make_shared<ModelState>();
  state->response = invalid.dump();
  kc::compiler::Compiler compiler(
      project_root, std::make_unique<StaticLanguageModel>(state));

  try {
    static_cast<void>(compiler.compile({}));
    FAIL("expected compiler validation failure");
  } catch (const kc::compiler::CompilerError& error) {
    CHECK(error.kind() ==
          kc::compiler::CompilerErrorKind::validation_failed);
  }

  CHECK(database.scalar_integer("SELECT COUNT(*) FROM proposals") == 0);
  CHECK(database.scalar_integer(
            "SELECT COUNT(*) FROM model_runs WHERE status = 'invalid_response'") ==
        1);
  const auto diagnostic = nlohmann::json::parse(
      database.scalar_text("SELECT response_json FROM model_runs"));
  CHECK(diagnostic.at("redacted") == true);
  CHECK(std::filesystem::is_empty(project_root / "vault"));
}
