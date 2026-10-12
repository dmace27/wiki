#include "kc/compiler/compiler.hpp"
#include "kc/domain/concept.hpp"
#include "kc/search/search_index.hpp"
#include "kc/extraction/extractor.hpp"
#include "kc/import/source_importer.hpp"
#include "kc/models/language_model.hpp"
#include "kc/review/review_service.hpp"
#include "kc/storage/database.hpp"
#include "kc/storage/project_initializer.hpp"
#include "kc/vault/vault_writer.hpp"
#include "../test_support.hpp"
#include "../contract_cases.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>

#include <filesystem>
#include <functional>
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
  std::function<void()> during_generation;
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
    if (state_->during_generation) {
      state_->during_generation();
    }
    return {.content = state_->response};
  }

 private:
  std::shared_ptr<ModelState> state_;
};

nlohmann::json create_proposal(const std::string& page_id,
                               const std::size_t end_char = 14U,
                               const std::string_view title = "Markov Chains",
                               const std::string_view slug = "markov-chains") {
  return {
      {"schema_version", 1},
      {"operation", "create_article"},
      {"article",
       {{"title", title},
        {"slug", slug},
        {"aliases", nlohmann::json::array()}}},
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
    const std::string_view content,
    const kc::source_import::ImportOptions& options = {}) {
  write_file(source_path, content);
  kc::source_import::SourceImporter importer(project_root);
  auto imported = importer.import_file(source_path, options);
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
  const auto term_source = import_and_extract(
      project_root, temporary.path() / "Lecture 8.md",
      "A Markov chain describes movement between states.\n");
  static_cast<void>(import_and_extract(
      project_root, temporary.path() / "Bayes Notes.md",
      "Posterior distributions update prior beliefs.\n"));

  // Only the latest immutable version of a logical source may be selected.
  const auto changing_path = temporary.path() / "Changing Notes.md";
  const auto changing = import_and_extract(
      project_root, changing_path,
      "A Markov chain appeared only in the old revision.\n");
  static_cast<void>(import_and_extract(
      project_root, changing_path,
      "The current revision discusses unrelated combinatorics.\n",
      {.source_id = changing.source.source_id}));

  auto state = std::make_shared<ModelState>();
  kc::compiler::Compiler compiler(
      project_root, std::make_unique<StaticLanguageModel>(state));
  const auto pages =
      compiler.select_evidence({.concept_title = "Markov Chains"});

  REQUIRE(pages.size() == 2U);
  CHECK(pages[0].source_version_id == title_source.version.source_version_id);
  CHECK(pages[1].source_version_id == term_source.version.source_version_id);

  const auto filtered = compiler.select_evidence(
      {.concept_title = "Markov Chains",
       .source_ids = {term_source.source.source_id}});
  REQUIRE(filtered.size() == 1U);
  CHECK(filtered.front().source_version_id ==
        term_source.version.source_version_id);

  CHECK_THROWS_AS(
      compiler.select_evidence(
          {.concept_title = "Markov Chains",
           .source_ids = {{"src_01J99999999999999999999999"}}}),
      kc::compiler::CompilerError);
}

TEST_CASE("equal-basename sources remain selectable through revisions and historical citations") {
  kc::test::TemporaryDirectory temporary;
  const auto root = temporary.path() / "project";
  const auto initialized = kc::storage::initialize_project(init_options(root));
  const auto semester_a = temporary.path() / "semester-a" / "Notes.md";
  const auto semester_b = temporary.path() / "semester-b" / "Notes.md";
  const auto semester_c = temporary.path() / "semester-c" / "Notes.md";
  std::filesystem::create_directories(semester_a.parent_path());
  std::filesystem::create_directories(semester_b.parent_path());
  std::filesystem::create_directories(semester_c.parent_path());
  const auto a = import_and_extract(root, semester_a,
                                    "A Markov chain describes semester A.\n");
  const auto b = import_and_extract(root, semester_b,
                                    "A Markov chain describes semester B.\n");
  const auto c = import_and_extract(root, semester_c,
                                    "A Markov chain describes semester A.\n");
  CHECK(a.source.source_id != b.source.source_id);
  CHECK(a.source.source_id != c.source.source_id);
  CHECK(c.deduplicated);
  CHECK(c.version.stored_path == a.version.stored_path);

  auto state = std::make_shared<ModelState>();
  kc::compiler::Compiler compiler(root, std::make_unique<StaticLanguageModel>(state));
  const auto all = compiler.select_evidence({.concept_title = "Markov Chains"});
  REQUIRE(all.size() == 3U);
  for (const auto& source : {a, b, c}) {
    const auto selected = compiler.select_evidence(
        {.concept_title = "Markov Chains", .source_ids = {source.source.source_id}});
    REQUIRE(selected.size() == 1U);
    CHECK(selected.front().source_version_id == source.version.source_version_id);
  }

  // Generate a real validated proposal before changing the source. Its exact
  // historical page remains reviewable and applicable after A -> B -> A.
  const auto old_page = compiler.select_evidence(
      {.concept_title = "Markov Chains", .source_ids = {a.source.source_id}}).front();
  state->response = create_proposal(old_page.page_id.value).dump();
  const auto proposal = compiler.compile(
      {.concept_title = "Markov Chains", .source_ids = {a.source.source_id}});
  const kc::source_import::ImportOptions revision{.source_id = a.source.source_id};
  const auto changed = import_and_extract(root, semester_a,
      "A Markov chain describes semester B.\n", revision);
  CHECK(changed.source.source_id == a.source.source_id);
  CHECK(changed.version_created);
  CHECK(changed.deduplicated);
  CHECK(changed.version.stored_path == b.version.stored_path);
  const auto restored = import_and_extract(root, semester_a,
      "A Markov chain describes semester A.\n", revision);
  CHECK(restored.restored);
  CHECK(restored.version.source_version_id != a.version.source_version_id);
  const auto current = compiler.select_evidence(
      {.concept_title = "Markov Chains", .source_ids = {a.source.source_id}});
  REQUIRE(current.size() == 1U);
  CHECK(current.front().source_version_id == restored.version.source_version_id);
  CHECK(current.front().page_id != old_page.page_id);
  kc::review::ReviewService review(root);
  CHECK(review.review_extraction(a.source.source_id).source_version_id ==
        restored.version.source_version_id);
  const auto historical = review.review_proposal(proposal.proposal_id);
  REQUIRE_FALSE(historical.citation_evidence.empty());
  CHECK(historical.citation_evidence.front().source_version_id == a.version.source_version_id);
  CHECK(historical.citation_evidence.front().extracted_text == old_page.text);
  kc::vault::VaultWriter writer(root);
  writer.approve(proposal.proposal_id);
  const auto applied = writer.apply(proposal.proposal_id);
  kc::storage::Database database(initialized.state_path);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM article_citations") == 2);
  CHECK(database.scalar_text(
      "SELECT p.source_version_id FROM article_citations c "
      "JOIN source_pages p ON p.page_id = c.page_id LIMIT 1") == a.version.source_version_id.value);
  CHECK(std::filesystem::is_regular_file(root / changed.version.stored_path));
  CHECK(std::filesystem::is_regular_file(root / applied.vault_path));
}

TEST_CASE("evidence selection supports arbitrary and curated concepts") {
  kc::test::TemporaryDirectory temporary;
  const auto project_root = temporary.path() / "project";
  static_cast<void>(
      kc::storage::initialize_project(init_options(project_root)));

  const auto biology = import_and_extract(
      project_root, temporary.path() / "Plant Biology.md",
      "Photosynthesis captures light energy in chemical bonds.\n");
  const auto implicit = import_and_extract(
      project_root, temporary.path() / "Lecture Notes.md",
      "Mitochondria convert nutrients into usable cellular energy.\n");
  const auto programming = import_and_extract(
      project_root, temporary.path() / "Systems Programming.md",
      "C++ uses RAII to tie resources to object lifetimes.\n");

  auto state = std::make_shared<ModelState>();
  kc::compiler::Compiler compiler(
      project_root, std::make_unique<StaticLanguageModel>(state));

  const auto automatic =
      compiler.select_evidence({.concept_title = "Photosynthesis"});
  REQUIRE(automatic.size() == 1U);
  CHECK(automatic.front().source_version_id ==
        biology.version.source_version_id);

  const auto symbolic =
      compiler.select_evidence({.concept_title = "C++"});
  REQUIRE(symbolic.size() == 1U);
  CHECK(symbolic.front().source_version_id ==
        programming.version.source_version_id);

  // The requested concept is implicit in this page, so an explicit source
  // filter deliberately treats the source as human-curated evidence.
  const auto curated = compiler.select_evidence(
      {.concept_title = "Cellular Respiration",
       .source_ids = {implicit.source.source_id}});
  REQUIRE(curated.size() == 1U);
  CHECK(curated.front().source_version_id ==
        implicit.version.source_version_id);
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
        {.concept_title = "Markov Chains",
         .source_ids = {imported.source.source_id}}));
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
      {.concept_title = "Markov Chains",
       .source_ids = {imported.source.source_id}});
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

  const auto first =
      compiler.compile({.concept_title = "Markov Chains"});

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
  const auto second =
      compiler.compile({.concept_title = "Markov Chains"});
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
    static_cast<void>(
        compiler.compile({.concept_title = "Markov Chains"}));
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

TEST_CASE("pending create proposals are superseded per concept") {
  kc::test::TemporaryDirectory temporary;
  const auto project_root = temporary.path() / "project";
  const auto initialized =
      kc::storage::initialize_project(init_options(project_root));
  const auto imported = import_and_extract(
      project_root, temporary.path() / "mixed-notes.md",
      "A Markov chain and Photosynthesis are unrelated examples.\n");

  kc::storage::Database database(initialized.state_path);
  const auto page_id = database.scalar_text(
      "SELECT page_id FROM source_pages WHERE source_version_id = '" +
      imported.version.source_version_id.value + "'");
  auto state = std::make_shared<ModelState>();
  kc::compiler::Compiler compiler(
      project_root, std::make_unique<StaticLanguageModel>(state));

  state->response = create_proposal(page_id).dump();
  static_cast<void>(
      compiler.compile({.concept_title = "Markov Chains"}));
  state->response =
      create_proposal(page_id, 14U, "Photosynthesis", "photosynthesis")
          .dump();
  static_cast<void>(
      compiler.compile({.concept_title = "Photosynthesis"}));

  CHECK(database.scalar_integer(
            "SELECT COUNT(*) FROM proposals WHERE status = 'pending'") == 2);
  CHECK(database.scalar_integer(
            "SELECT COUNT(*) FROM proposals WHERE status = 'superseded'") ==
        0);
}

TEST_CASE("canonical concept requests preserve identity through compile apply and search") {
  struct Variant {
    std::string title;
    std::string request;
  };
  const auto variant = GENERATE(
      Variant{"Photosynthesis", " \tphotosynthesis  "},
      Variant{"Café", " CAFE\u0301 "},
      Variant{"Straße", "STRASSE"},
      Variant{"Биология", "БИОЛОГИЯ"},
      Variant{"量子力学", "量子力学"},
      Variant{"!!!", " !!! "});
  INFO(variant.title);
  kc::test::TemporaryDirectory temporary;
  const auto project_root = temporary.path() / "project";
  const auto initialized = kc::storage::initialize_project(init_options(project_root));
  const auto imported = import_and_extract(
      project_root, temporary.path() / "notes.md",
      "A Markov chain supports a fixture about " + variant.title + ".\n");
  kc::storage::Database database(initialized.state_path);
  const auto page_id = database.scalar_text("SELECT page_id FROM source_pages");
  auto state = std::make_shared<ModelState>();
  const auto slug = kc::domain::canonical_concept_slug(variant.title);
  state->response = create_proposal(page_id, 14U, variant.title, slug).dump();
  kc::compiler::Compiler compiler(project_root, std::make_unique<StaticLanguageModel>(state));
  const auto first = compiler.compile({.concept_title = "  " + variant.title + "\t",
                                      .source_ids = {imported.source.source_id}});
  kc::vault::VaultWriter writer(project_root);
  writer.approve(first.proposal_id);
  const auto created = writer.apply(first.proposal_id);
  const auto original_path = created.vault_path;
  const auto original_title = database.scalar_text("SELECT title FROM articles");
  const auto original_slug = database.scalar_text("SELECT slug FROM articles");
  REQUIRE(original_title == variant.title);

  auto update = create_proposal(page_id, 14U, variant.title, slug);
  update["operation"] = "update_article";
  update["article"]["article_id"] = created.article_id.value;
  state->response = update.dump();
  const auto second = compiler.compile({.concept_title = variant.request,
                                       .source_ids = {imported.source.source_id}});
  CHECK(second.operation == kc::domain::ProposalOperation::update_article);
  CHECK(second.article_id == created.article_id);
  kc::review::ReviewService review(project_root);
  CHECK(review.review_proposal(second.proposal_id).proposal.article.title == original_title);
  writer.approve(second.proposal_id);
  const auto updated = writer.apply(second.proposal_id);
  CHECK(updated.article_id == created.article_id);
  CHECK(updated.vault_path == original_path);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM articles") == 1);
  CHECK(database.scalar_text("SELECT title FROM articles") == original_title);
  CHECK(database.scalar_text("SELECT slug FROM articles") == original_slug);
  CHECK(database.scalar_text("SELECT title FROM article_fts") == original_title);
  kc::search::SearchIndex search(project_root);
  const auto results = search.search(variant.request);
  REQUIRE(results.size() == 1U);
  CHECK(results.front().article_id == created.article_id);
  CHECK(results.front().title == original_title);
  CHECK(results.front().vault_path == original_path);
}

TEST_CASE("pending creates share canonical Unicode identity without superseding other concepts") {
  kc::test::TemporaryDirectory temporary;
  const auto project_root = temporary.path() / "project";
  const auto initialized = kc::storage::initialize_project(init_options(project_root));
  const auto imported = import_and_extract(project_root, temporary.path() / "notes.md",
      "A Markov chain supports a fixture about Café and Biology.\n");
  kc::storage::Database database(initialized.state_path);
  const auto page_id = database.scalar_text("SELECT page_id FROM source_pages");
  auto state = std::make_shared<ModelState>();
  kc::compiler::Compiler compiler(project_root, std::make_unique<StaticLanguageModel>(state));
  state->response = create_proposal(page_id, 14U, "Café", "caf").dump();
  const auto first = compiler.compile({.concept_title = "Cafe\u0301"});
  kc::vault::VaultWriter writer(project_root);
  writer.approve(first.proposal_id);
  state->response = create_proposal(page_id, 14U, "Biology", "biology").dump();
  const auto unrelated = compiler.compile({.concept_title = "Biology"});
  state->response = create_proposal(page_id, 14U, "Café", "caf").dump();
  const auto second = compiler.compile({.concept_title = "\tCAFÉ "});
  CHECK(second.operation == kc::domain::ProposalOperation::create_article);
  kc::review::ReviewService review(project_root);
  CHECK(review.review_proposal(first.proposal_id).summary.status == kc::domain::ProposalStatus::superseded);
  CHECK(review.review_proposal(unrelated.proposal_id).summary.status == kc::domain::ProposalStatus::pending);
  CHECK(review.review_proposal(second.proposal_id).proposal.article.title == "Café");
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM proposals WHERE status = 'pending'") == 2);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM proposals WHERE status = 'superseded'") == 1);
  static_cast<void>(imported);
}

TEST_CASE("distinct-title slug collisions fail before model generation") {
  const bool apply_first = GENERATE(false, true);
  kc::test::TemporaryDirectory temporary;
  const auto project_root = temporary.path() / "project";
  const auto initialized = kc::storage::initialize_project(init_options(project_root));
  const auto imported = import_and_extract(project_root, temporary.path() / "notes.md",
      "A Markov chain supports a fixture about A-B and A B.\n");
  kc::storage::Database database(initialized.state_path);
  auto state = std::make_shared<ModelState>();
  state->response = create_proposal(database.scalar_text("SELECT page_id FROM source_pages"),
                                    14U, "A-B", "a-b").dump();
  kc::compiler::Compiler compiler(project_root, std::make_unique<StaticLanguageModel>(state));
  const auto first = compiler.compile({.concept_title = "A-B"});
  if (apply_first) {
    kc::vault::VaultWriter writer(project_root);
    writer.approve(first.proposal_id);
    static_cast<void>(writer.apply(first.proposal_id));
  }
  try {
    static_cast<void>(compiler.compile({.concept_title = "A B"}));
    FAIL("expected a pre-generation collision diagnostic");
  } catch (const kc::compiler::CompilerError& error) {
    CHECK(error.kind() == kc::compiler::CompilerErrorKind::invalid_concept);
    CHECK(std::string(error.what()).find("slug collision") != std::string::npos);
    CHECK(std::string(error.what()).find("A-B") != std::string::npos);
  }
  CHECK(state->prompts.size() == 1U);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM model_runs") == 1);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM proposals") == 1);
  // A more specific identity is accepted without touching the first proposal.
  state->response = create_proposal(database.scalar_text("SELECT page_id FROM source_pages"),
                                    14U, "A B Theory", "a-b-theory").dump();
  const auto distinct = compiler.compile({.concept_title = "A B Theory",
                                        .source_ids = {imported.source.source_id}});
  CHECK(distinct.proposal_id != first.proposal_id);
}

TEST_CASE("portable title errors are rejected before evidence and model use") {
  kc::test::TemporaryDirectory temporary;
  const auto project_root = temporary.path() / "project";
  const auto initialized = kc::storage::initialize_project(init_options(project_root));
  auto state = std::make_shared<ModelState>();
  kc::compiler::Compiler compiler(project_root, std::make_unique<StaticLanguageModel>(state));
  for (const auto& title : {"CON", "nul.txt", "COM9", "LPT²", "Topic.", "Topic?", "a\nb"}) {
    try {
      static_cast<void>(compiler.compile({.concept_title = title}));
      FAIL("expected portable concept validation");
    } catch (const kc::compiler::CompilerError& error) {
      CHECK(error.kind() == kc::compiler::CompilerErrorKind::invalid_concept);
    }
  }
  kc::storage::Database database(initialized.state_path);
  CHECK(state->prompts.empty());
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM model_runs") == 0);
}

TEST_CASE("non-English automatic evidence matching is Unicode aware and bounded") {
  kc::test::TemporaryDirectory temporary;
  const auto project_root = temporary.path() / "project";
  static_cast<void>(kc::storage::initialize_project(init_options(project_root)));
  const auto accented = import_and_extract(project_root, temporary.path() / "notes-a.md",
                                           "Cafe\u0301 and its history.\n");
  const auto cyrillic = import_and_extract(project_root, temporary.path() / "notes-b.md",
                                          "БИОЛОГИЯ изучает жизнь.\n");
  const auto cjk = import_and_extract(project_root, temporary.path() / "notes-c.md",
                                     "这里讨论量子力学现象\n");
  static_cast<void>(import_and_extract(project_root, temporary.path() / "---.md", "---\n"));
  auto state = std::make_shared<ModelState>();
  kc::compiler::Compiler compiler(project_root, std::make_unique<StaticLanguageModel>(state));
  const auto french = compiler.select_evidence({.concept_title = "CAFÉ"});
  REQUIRE(french.size() == 1U);
  CHECK(french.front().source_version_id == accented.version.source_version_id);
  const auto russian = compiler.select_evidence({.concept_title = "биология"});
  REQUIRE(russian.size() == 1U);
  CHECK(russian.front().source_version_id == cyrillic.version.source_version_id);
  // There is no dictionary-based segmentation or transliteration. A title
  // embedded in an unbroken CJK sentence needs explicit source curation.
  CHECK(compiler.select_evidence({.concept_title = "量子力学"}).empty());
  const auto curated = compiler.select_evidence({.concept_title = "量子力学",
                                                .source_ids = {cjk.source.source_id}});
  REQUIRE(curated.size() == 1U);
  CHECK(curated.front().source_version_id == cjk.version.source_version_id);
  CHECK(compiler.select_evidence({.concept_title = "!!!"}).empty());
  try {
    static_cast<void>(compiler.compile({.concept_title = "量子力学"}));
    FAIL("expected a curation diagnostic");
  } catch (const kc::compiler::CompilerError& error) {
    CHECK(error.kind() == kc::compiler::CompilerErrorKind::no_evidence);
    CHECK(std::string(error.what()).find("--source") != std::string::npos);
  }
  CHECK(state->prompts.empty());
}

TEST_CASE("identity collisions introduced during generation cannot publish a proposal") {
  kc::test::TemporaryDirectory temporary;
  const auto project_root = temporary.path() / "project";
  const auto initialized = kc::storage::initialize_project(init_options(project_root));
  static_cast<void>(import_and_extract(project_root, temporary.path() / "notes.md",
                                       "A Markov chain supports A B.\n"));
  kc::storage::Database database(initialized.state_path);
  auto state = std::make_shared<ModelState>();
  state->response = create_proposal(database.scalar_text("SELECT page_id FROM source_pages"),
                                    14U, "A B", "a-b").dump();
  state->during_generation = [&database] {
    database.execute("INSERT INTO articles(article_id,title,slug,vault_path,created_at,updated_at) "
                     "VALUES ('art_01J00000000000000000000000','A-B','a-b','vault/A-B.md','t','t')");
  };
  kc::compiler::Compiler compiler(project_root, std::make_unique<StaticLanguageModel>(state));
  CHECK_THROWS_AS(compiler.compile({.concept_title = "A B"}), kc::compiler::CompilerError);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM proposals") == 0);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM articles") == 1);
}

TEST_CASE("Unicode legacy articles retain their original slug and destination") {
  kc::test::TemporaryDirectory temporary;
  const auto project_root = temporary.path() / "project";
  const auto initialized = kc::storage::initialize_project(init_options(project_root));
  const auto imported = import_and_extract(project_root, temporary.path() / "notes.md",
                                           "A Markov chain supports Биология.\n");
  kc::storage::Database database(initialized.state_path);
  // A pre-task-3 opaque slug and custom vault path must not be regenerated.
  database.execute("INSERT INTO articles(article_id,title,slug,vault_path,created_at,updated_at) "
                   "VALUES ('art_01J00000000000000000000000','Биология','concept-legacy',"
                   "'vault/Original.md','t','t')");
  // Use the project's actual ownership marker rather than a fixture assumption.
  auto config = kc::test::read_json(project_root / "kc.json");
  write_file(project_root / "vault" / "Original.md",
      "---\nkc_schema: 1\narticle_id: art_01J00000000000000000000000\n"
      "title: Биология\naliases: []\n---\n# Биология\n\n<!-- kc:managed:start id=\"" +
      config["vault"]["generated_section_id"].get<std::string>() +
      "\" -->\nold\n<!-- kc:managed:end -->\n");
  auto state = std::make_shared<ModelState>();
  auto payload = create_proposal(database.scalar_text("SELECT page_id FROM source_pages"),
                                  14U, "Биология", "concept-legacy");
  payload["operation"] = "update_article";
  payload["article"]["article_id"] = "art_01J00000000000000000000000";
  state->response = payload.dump();
  kc::compiler::Compiler compiler(project_root, std::make_unique<StaticLanguageModel>(state));
  const auto compiled = compiler.compile({.concept_title = "БИОЛОГИЯ"});
  kc::vault::VaultWriter writer(project_root);
  writer.approve(compiled.proposal_id);
  CHECK(writer.apply(compiled.proposal_id).vault_path == "vault/Original.md");
  CHECK(database.scalar_text("SELECT slug FROM articles") == "concept-legacy");
  static_cast<void>(imported);
}

TEST_CASE("ambiguous legacy Unicode identities require curation before generation or search") {
  kc::test::TemporaryDirectory temporary;
  const auto project_root = temporary.path() / "project";
  const auto initialized = kc::storage::initialize_project(init_options(project_root));
  kc::storage::Database database(initialized.state_path);
  // SQLite NOCASE permitted these two articles under the previous rules.
  database.execute("INSERT INTO articles(article_id,title,slug,vault_path,created_at,updated_at) VALUES "
                   "('art_01J00000000000000000000000','Straße','strasse','vault/Straße.md','t','t'),"
                   "('art_01J00000000000000000000001','STRASSE','strasse-old','vault/STRASSE.md','t','t')");
  auto state = std::make_shared<ModelState>();
  kc::compiler::Compiler compiler(project_root, std::make_unique<StaticLanguageModel>(state));
  try {
    static_cast<void>(compiler.compile({.concept_title = "strasse"}));
    FAIL("expected an ambiguous identity error");
  } catch (const kc::compiler::CompilerError& error) {
    CHECK(std::string(error.what()).find("ambiguous Unicode") != std::string::npos);
  }
  CHECK(state->prompts.empty());
  kc::search::SearchIndex search(project_root);
  CHECK_THROWS_AS(search.search("strasse"), kc::search::SearchError);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM articles") == 2);
}

TEST_CASE("biology history and programming articles create and update with consistent YAML and aliases") {
  kc::test::TemporaryDirectory temporary;
  const auto root = temporary.path() / "project";
  const auto initialized = kc::storage::initialize_project(init_options(root));
  kc::storage::Database database(initialized.state_path);
  auto state = std::make_shared<ModelState>();
  kc::compiler::Compiler compiler(root, std::make_unique<StaticLanguageModel>(state));
  kc::vault::VaultWriter writer(root);
  kc::search::SearchIndex search(root);
  struct Topic {
    std::string title;
    std::string explanation;
    std::string related;
  };
  const std::vector<Topic> topics{
      {"Photosynthesis", "Photosynthesis converts light to chemical energy.", "Chlorophyll"},
      {"Silk Road", "The Silk Road connected trading communities.", "Trade"},
      {"Recursion", "Recursion invokes a function from within itself.", "Call Stack"}};
  for (const auto& topic : topics) {
    CAPTURE(topic.title);
    const auto imported = import_and_extract(root, temporary.path() / (topic.title + ".md"),
                                             topic.explanation);
    const kc::compiler::CompileOptions options{.concept_title = topic.title,
                                              .source_ids = {imported.source.source_id}};
    const auto page = compiler.select_evidence(options).front();
    auto payload = create_proposal(page.page_id.value, page.text.size(), topic.title,
                                   kc::domain::canonical_concept_slug(topic.title));
    const nlohmann::json citation{{"page_id", page.page_id.value}, {"start_char", 0},
                                  {"end_char", page.text.size()}, {"quote", page.text}};
    payload["sections"][0]["blocks"][0]["text"] = topic.explanation;
    payload["sections"][0]["blocks"][0]["citations"] = nlohmann::json::array({citation});
    payload["related_concepts"][0]["title"] = topic.related;
    payload["related_concepts"][0]["reason"] = "Connected in these notes.";
    payload["related_concepts"][0]["citations"] = nlohmann::json::array({citation});
    payload["article"]["aliases"] = nlohmann::json::array({"InitialAlias", "光合成"});
    state->response = payload.dump();
    const auto proposed = compiler.compile(options);
    writer.approve(proposed.proposal_id);
    const auto created = writer.apply(proposed.proposal_id);
    const auto path = root / created.vault_path;
    std::ifstream input(path);
    std::string content{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    const auto frontmatter_end = content.find("\n---\n", 4);
    REQUIRE(frontmatter_end != std::string::npos);
    auto metadata = YAML::Load(content.substr(4, frontmatter_end - 4));
    CHECK(metadata["title"].Scalar() == topic.title);
    CHECK(metadata["aliases"].IsSequence());
    CHECK(metadata["aliases"][1].Scalar() == "光合成");
    CHECK_FALSE(metadata["tags"]);

    // User ownership begins after creation. Model suggestions in the update
    // must not silently change either Obsidian aliases or CLI search aliases.
    const auto alias_start = content.find("aliases:");
    content.replace(alias_start, frontmatter_end - alias_start,
                    "aliases: [\"UserAlias\", \"新しい別名\"]\ncustom: keep");
    write_file(path, content);
    payload["operation"] = "update_article";
    payload["article"]["article_id"] = created.article_id.value;
    payload["article"]["aliases"] = nlohmann::json::array({"ModelSuggestion"});
    payload["sections"][0]["blocks"][0]["text"] = topic.explanation + " Updated explanation.";
    state->response = payload.dump();
    const auto update = compiler.compile(options);
    writer.approve(update.proposal_id);
    const auto applied = writer.apply(update.proposal_id);
    CHECK(applied.article_id == created.article_id);
    std::ifstream updated_input(path);
    const std::string updated{std::istreambuf_iterator<char>(updated_input),
                              std::istreambuf_iterator<char>()};
    CHECK(updated.substr(0, updated.find("\n---\n", 4)) ==
          content.substr(0, content.find("\n---\n", 4)));
    metadata = YAML::Load(updated.substr(4, updated.find("\n---\n", 4) - 4));
    CHECK(metadata["aliases"][0].Scalar() == "UserAlias");
    CHECK(metadata["custom"].Scalar() == "keep");
    CHECK(database.scalar_text("SELECT aliases FROM article_fts WHERE article_id = '" +
                               created.article_id.value + "'") == "UserAlias\n新しい別名");
    CHECK_FALSE(search.search("UserAlias").empty());
    CHECK(search.search("ModelSuggestion").empty());
    CHECK(search.search("InitialAlias").empty());
  }
}

TEST_CASE("unrenderable generated proposals never reach pending review") {
  const auto field = GENERATE("heading", "related", "marker");
  kc::test::TemporaryDirectory temporary;
  const auto root = temporary.path() / "project";
  const auto initialized = kc::storage::initialize_project(init_options(root));
  static_cast<void>(import_and_extract(root, temporary.path() / "notes.md",
                                       "A Markov chain has the Markov property."));
  kc::storage::Database database(initialized.state_path);
  auto payload = create_proposal(database.scalar_text("SELECT page_id FROM source_pages"));
  if (std::string_view(field) == "heading") payload["sections"][0]["heading"] = "Heading\nSources";
  if (std::string_view(field) == "related") payload["related_concepts"][0]["title"] = "Target|Misleading";
  if (std::string_view(field) == "marker") payload["article"]["aliases"] = {"<!-- kc:managed:end -->"};
  auto state = std::make_shared<ModelState>();
  state->response = payload.dump();
  kc::compiler::Compiler compiler(root, std::make_unique<StaticLanguageModel>(state));
  CHECK_THROWS_AS(compiler.compile({.concept_title = "Markov Chains"}), kc::compiler::CompilerError);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM proposals") == 0);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM model_runs WHERE status = 'invalid_response'") == 1);
  CHECK(std::filesystem::is_empty(root / "vault"));
}

TEST_CASE("common invalid contract fixtures cannot be persisted by the compiler") {
  kc::test::TemporaryDirectory temporary;
  const auto root = temporary.path() / "project";
  const auto initialized = kc::storage::initialize_project(init_options(root));
  static_cast<void>(import_and_extract(root, temporary.path() / "notes.md",
                                       "A Markov chain has the Markov property."));
  auto state = std::make_shared<ModelState>();
  kc::compiler::Compiler compiler(root, std::make_unique<StaticLanguageModel>(state));
  const auto page = compiler.select_evidence({.concept_title = "Markov Chains"}).front();
  auto base = kc::test::read_json(std::filesystem::path(KC_TEST_FIXTURES_DIR) / "article-proposal.valid.json");
  base["sections"][0]["blocks"][0]["citations"][0]["page_id"] = page.page_id.value;
  base["related_concepts"][0]["citations"][0]["page_id"] = page.page_id.value;
  std::size_t invalid_count = 0;
  for (const auto& fixture : kc::test::contract_cases()) {
    if (kc::test::case_contract(fixture) != kc::domain::JsonContract::article_proposal ||
        fixture.at("runtime_valid").get<bool>()) {
      continue;
    }
    CAPTURE(fixture.at("name"));
    state->response = base.patch(fixture.at("patches")).dump();
    try {
      static_cast<void>(compiler.compile({.concept_title = "Markov Chains"}));
      FAIL("invalid output must not enter pending review");
    } catch (const kc::compiler::CompilerError& error) {
      CHECK(error.kind() == kc::compiler::CompilerErrorKind::validation_failed);
    }
    ++invalid_count;
  }
  kc::storage::Database database(initialized.state_path);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM proposals") == 0);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM model_runs WHERE status = 'invalid_response'") ==
        static_cast<std::int64_t>(invalid_count));
  CHECK(std::filesystem::is_empty(root / "vault"));
}

TEST_CASE("repeated section keys preserve distinct cited blocks through apply") {
  kc::test::TemporaryDirectory temporary;
  const auto root = temporary.path() / "project";
  const auto initialized = kc::storage::initialize_project(init_options(root));
  static_cast<void>(import_and_extract(root, temporary.path() / "notes.md",
                                       "A Markov chain has the Markov property."));
  kc::storage::Database database(initialized.state_path);
  auto state = std::make_shared<ModelState>();
  auto payload = create_proposal(database.scalar_text("SELECT page_id FROM source_pages"));
  auto second = payload["sections"][0];
  second["heading"] = "Another working example";
  second["blocks"][0]["text"] = "A second supported example.";
  payload["sections"].push_back(second);
  state->response = payload.dump();
  kc::compiler::Compiler compiler(root, std::make_unique<StaticLanguageModel>(state));
  const auto proposal = compiler.compile({.concept_title = "Markov Chains"});
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM proposal_citations WHERE section_key = 'working_explanation'") == 2);
  CHECK(database.scalar_integer("SELECT MAX(block_index) FROM proposal_citations WHERE section_key = 'working_explanation'") == 1);
  kc::review::ReviewService review(root);
  CHECK(review.review_proposal(proposal.proposal_id).proposal.sections.size() == 2U);
  kc::vault::VaultWriter writer(root);
  writer.approve(proposal.proposal_id);
  const auto applied = writer.apply(proposal.proposal_id);
  std::ifstream input(root / applied.vault_path);
  const std::string content{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  CHECK(content.find("## Another working example") != std::string::npos);
  CHECK(content.find("A second supported example") != std::string::npos);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM article_citations WHERE section_key = 'working_explanation'") == 2);
}
