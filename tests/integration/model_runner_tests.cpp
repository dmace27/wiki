#include "kc/domain/json.hpp"
#include "kc/models/model_runner.hpp"
#include "kc/storage/database.hpp"
#include "kc/storage/project_initializer.hpp"
#include "../test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>

namespace {

kc::storage::InitOptions init_options(const std::filesystem::path& root) {
  return {
      .project_root = root,
      .vault_path = "vault",
      .migration_directory = KC_TEST_MIGRATIONS_DIR};
}

/// Configure non-default values so the test proves ModelRunner reads kc.json
/// instead of silently using constants from the C++ adapter.
void configure_test_ollama(const std::filesystem::path& project_root) {
  const auto path = project_root / "kc.json";
  auto config = kc::test::read_json(path);
  config["providers"]["llm"]["ollama"]["base_url"] =
      "http://127.0.0.1:14567/";
  config["providers"]["llm"]["ollama"]["model"] = "fixture-gemma:1b";
  config["providers"]["llm"]["ollama"]["timeout_seconds"] = 17;
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << config.dump(2) << '\n';
}

kc::domain::ExtractedPage evidence_page() {
  const auto parsed = kc::domain::parse_extracted_page(
      kc::test::read_json(
          std::filesystem::path(KC_TEST_FIXTURES_DIR) /
          "extracted-page.valid.json"));
  REQUIRE(parsed.valid());
  return *parsed.value;
}

kc::models::ProposalGenerationRequest generation_request() {
  return {
      .concept_title = "Markov Chains",
      .operation = kc::domain::ProposalOperation::create_article,
      .pages = {evidence_page()}};
}

nlohmann::json valid_proposal() {
  return kc::test::read_json(
      std::filesystem::path(KC_TEST_FIXTURES_DIR) /
      "article-proposal.valid.json");
}

kc::models::HttpResponse ollama_envelope(
    const std::string& model_content) {
  return {
      .status_code = 200,
      .body = nlohmann::json{
          {"model", "fixture-gemma:1b"},
          {"message",
           {{"role", "assistant"}, {"content", model_content}}},
          {"done", true}}
                  .dump()};
}

class RecordingHttpClient final : public kc::models::HttpClient {
 public:
  [[nodiscard]] kc::models::HttpResponse post_json(
      const kc::models::HttpRequest& request) override {
    last_request = request;
    ++calls;
    return next_response;
  }

  kc::models::HttpRequest last_request;
  kc::models::HttpResponse next_response;
  std::size_t calls{0};
};

}  // namespace

TEST_CASE("Ollama settings, schema prompt, and completed runs are audited") {
  kc::test::TemporaryDirectory temporary;
  const auto project_root = temporary.path() / "project";
  const auto initialized =
      kc::storage::initialize_project(init_options(project_root));
  configure_test_ollama(project_root);

  auto http = std::make_shared<RecordingHttpClient>();
  http->next_response = ollama_envelope(valid_proposal().dump());
  kc::models::ModelRunner runner(project_root, http);

  const auto result = runner.generate_markov_chains(generation_request());

  CHECK(result.status == kc::domain::ModelRunStatus::completed);
  REQUIRE(result.proposal.has_value());
  CHECK(result.proposal->article.title == "Markov Chains");
  CHECK(http->calls == 1);
  CHECK(http->last_request.url == "http://127.0.0.1:14567/api/chat");
  CHECK(http->last_request.timeout_seconds == 17);

  const auto request_body =
      nlohmann::json::parse(http->last_request.json_body);
  CHECK(request_body.at("model") == "fixture-gemma:1b");
  CHECK(request_body.at("stream") == false);
  CHECK(request_body.at("options").at("temperature") == 0);
  CHECK(request_body.at("format").at("additionalProperties") == false);
  CHECK(request_body.at("format").contains("$defs"));
  REQUIRE(request_body.at("messages").size() == 2);
  CHECK(request_body.at("messages")[0].at("role") == "system");
  CHECK(request_body.at("messages")[1].at("content")
            .get<std::string>()
            .find("pg_01J00000000000000000000000") != std::string::npos);

  kc::storage::Database database(initialized.state_path);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM model_runs") == 1);
  CHECK(database.scalar_text("SELECT provider FROM model_runs") == "ollama");
  CHECK(database.scalar_text("SELECT model FROM model_runs") ==
        "fixture-gemma:1b");
  CHECK(database.scalar_text("SELECT prompt_version FROM model_runs") ==
        "markov-chains-v1");
  CHECK(database.scalar_text("SELECT status FROM model_runs") == "completed");
  CHECK(database.scalar_text("SELECT request_sha256 FROM model_runs").size() ==
        64);
  CHECK(nlohmann::json::parse(
            database.scalar_text("SELECT response_json FROM model_runs")) ==
        valid_proposal());
  CHECK(database.scalar_integer(
            "SELECT COUNT(*) FROM model_runs WHERE error_message IS NULL") ==
        1);

  // Part 1C records a validated model run only. Proposal creation and vault
  // writes are intentionally reserved for later compiler/writer stages.
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM proposals") == 0);
  CHECK(std::filesystem::is_empty(project_root / "vault"));
}

TEST_CASE("invalid JSON is redacted and cannot create a proposal") {
  kc::test::TemporaryDirectory temporary;
  const auto project_root = temporary.path() / "project";
  const auto initialized =
      kc::storage::initialize_project(init_options(project_root));
  auto http = std::make_shared<RecordingHttpClient>();
  constexpr std::string_view secret_response =
      "not-json Authorization: Bearer should-not-be-stored";
  http->next_response = ollama_envelope(std::string(secret_response));
  kc::models::ModelRunner runner(project_root, http);

  const auto result = runner.generate_markov_chains(generation_request());

  CHECK(result.status == kc::domain::ModelRunStatus::invalid_response);
  CHECK_FALSE(result.proposal.has_value());
  kc::storage::Database database(initialized.state_path);
  CHECK(database.scalar_text("SELECT status FROM model_runs") ==
        "invalid_response");
  const auto diagnostic =
      database.scalar_text("SELECT response_json FROM model_runs");
  CHECK(diagnostic.find(secret_response) == std::string::npos);
  const auto parsed_diagnostic = nlohmann::json::parse(diagnostic);
  CHECK(parsed_diagnostic.at("redacted") == true);
  CHECK(parsed_diagnostic.at("response_sha256").get<std::string>().size() == 64);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM proposals") == 0);
}

TEST_CASE("invalid citations become invalid_response with no proposal") {
  kc::test::TemporaryDirectory temporary;
  const auto project_root = temporary.path() / "project";
  const auto initialized =
      kc::storage::initialize_project(init_options(project_root));
  auto invalid = valid_proposal();
  invalid["sections"][0]["blocks"][0]["citations"][0]["page_id"] =
      "pg_01J11111111111111111111111";
  auto http = std::make_shared<RecordingHttpClient>();
  http->next_response = ollama_envelope(invalid.dump());
  kc::models::ModelRunner runner(project_root, http);

  const auto result = runner.generate_markov_chains(generation_request());

  CHECK(result.status == kc::domain::ModelRunStatus::invalid_response);
  CHECK_FALSE(result.proposal.has_value());
  REQUIRE_FALSE(result.issues.empty());
  kc::storage::Database database(initialized.state_path);
  CHECK(database.scalar_text("SELECT status FROM model_runs") ==
        "invalid_response");
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM proposals") == 0);
  CHECK(std::filesystem::is_empty(project_root / "vault"));
}

TEST_CASE("HTTP failures store only a fixed redacted diagnostic") {
  kc::test::TemporaryDirectory temporary;
  const auto project_root = temporary.path() / "project";
  const auto initialized =
      kc::storage::initialize_project(init_options(project_root));
  auto http = std::make_shared<RecordingHttpClient>();
  http->next_response = {
      .status_code = 500,
      .body = R"({"error":"Bearer highly-sensitive-token"})"};
  kc::models::ModelRunner runner(project_root, http);

  const auto result = runner.generate_markov_chains(generation_request());

  CHECK(result.status == kc::domain::ModelRunStatus::failed);
  CHECK_FALSE(result.proposal.has_value());
  kc::storage::Database database(initialized.state_path);
  CHECK(database.scalar_text("SELECT status FROM model_runs") == "failed");
  CHECK(database.scalar_text("SELECT error_message FROM model_runs") ==
        "local model request failed");
  CHECK(database.scalar_integer(
            "SELECT COUNT(*) FROM model_runs WHERE response_json IS NULL") ==
        1);
  const auto persisted = database.scalar_text(
      "SELECT provider || model || prompt_version || error_message "
      "FROM model_runs");
  CHECK(persisted.find("highly-sensitive-token") == std::string::npos);
  CHECK(database.scalar_integer("SELECT COUNT(*) FROM proposals") == 0);
}
