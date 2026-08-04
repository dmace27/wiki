#include "kc/models/model_runner.hpp"

#include "kc/domain/id.hpp"
#include "kc/domain/json.hpp"
#include "kc/import/sha256.hpp"
#include "kc/storage/database.hpp"
#include "markov_chains_prompt.hpp"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <chrono>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace kc::models {
namespace {

class Statement {
 public:
  Statement(sqlite3* connection, const std::string_view sql)
      : connection_(connection) {
    const auto query = std::string(sql);
    if (sqlite3_prepare_v2(
            connection_, query.c_str(), -1, &statement_, nullptr) != SQLITE_OK) {
      throw ModelError(ModelErrorKind::state_error,
                       "failed to prepare model-run audit query");
    }
  }

  ~Statement() { sqlite3_finalize(statement_); }
  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;

  void bind_text(const int index, const std::string_view value) {
    if (sqlite3_bind_text(statement_, index, value.data(),
                          static_cast<int>(value.size()),
                          SQLITE_TRANSIENT) != SQLITE_OK) {
      throw ModelError(ModelErrorKind::state_error,
                       "failed to bind model-run audit query");
    }
  }

  void bind_null(const int index) {
    if (sqlite3_bind_null(statement_, index) != SQLITE_OK) {
      throw ModelError(ModelErrorKind::state_error,
                       "failed to bind model-run audit query");
    }
  }

  void execute() {
    if (sqlite3_step(statement_) != SQLITE_DONE) {
      throw ModelError(ModelErrorKind::state_error,
                       "failed to write model-run audit record");
    }
  }

 private:
  sqlite3* connection_;
  sqlite3_stmt* statement_{nullptr};
};

std::string utc_now() {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm utc{};
#if defined(_WIN32)
  gmtime_s(&utc, &time);
#else
  gmtime_r(&time, &utc);
#endif
  std::ostringstream output;
  output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
  return output.str();
}

domain::ProjectConfig read_config(const std::filesystem::path& root) {
  const auto path = root / "kc.json";
  std::ifstream input(path);
  if (!input) {
    throw ModelError(ModelErrorKind::invalid_project,
                     "no readable kc.json at project root");
  }

  try {
    nlohmann::json document;
    input >> document;
    const auto parsed = domain::parse_project_config(document);
    if (!parsed) {
      throw ModelError(ModelErrorKind::invalid_project,
                       "project configuration is invalid");
    }
    return *parsed.value;
  } catch (const nlohmann::json::exception&) {
    throw ModelError(ModelErrorKind::invalid_project,
                     "project configuration is not valid JSON");
  }
}

std::unique_ptr<LanguageModel> configured_ollama_model(
    const domain::ProjectConfig& config,
    std::shared_ptr<HttpClient> http_client) {
  if (config.providers.llm.default_provider != "ollama") {
    throw ModelError(ModelErrorKind::invalid_project,
                     "unsupported default language model provider");
  }
  try {
    return std::make_unique<OllamaLanguageModel>(
        config.providers.llm.ollama, std::move(http_client));
  } catch (const ModelAdapterError&) {
    throw ModelError(ModelErrorKind::invalid_project,
                     "configured Ollama settings are invalid");
  }
}

std::string_view status_string(const domain::ModelRunStatus status) {
  switch (status) {
    case domain::ModelRunStatus::completed:
      return "completed";
    case domain::ModelRunStatus::invalid_response:
      return "invalid_response";
    case domain::ModelRunStatus::failed:
      return "failed";
  }
  return "failed";
}

void record_model_run(
    const std::filesystem::path& state_path, const domain::RunId& run_id,
    const std::string_view provider, const std::string_view model,
    const std::string_view prompt_version,
    const std::string_view request_sha256,
    const domain::ModelRunStatus status, const std::string_view started_at,
    const std::optional<std::string>& response_json,
    const std::optional<std::string_view> error_message) {
  try {
    storage::Database database(state_path);
    Statement statement(
        database.native_handle(),
        "INSERT INTO model_runs("
        "run_id, provider, model, prompt_version, request_sha256, "
        "response_json, status, started_at, completed_at, error_message) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    statement.bind_text(1, run_id.value);
    statement.bind_text(2, provider);
    statement.bind_text(3, model);
    statement.bind_text(4, prompt_version);
    statement.bind_text(5, request_sha256);
    if (response_json) {
      statement.bind_text(6, *response_json);
    } else {
      statement.bind_null(6);
    }
    statement.bind_text(7, status_string(status));
    statement.bind_text(8, started_at);
    statement.bind_text(9, utc_now());
    if (error_message) {
      statement.bind_text(10, *error_message);
    } else {
      statement.bind_null(10);
    }
    statement.execute();
  } catch (const storage::StorageError&) {
    throw ModelError(ModelErrorKind::state_error,
                     "could not write model-run audit record");
  }
}

std::string request_hash(const LanguageModel& model,
                         const LanguageModelRequest& request) {
  // Persist only this digest, never source evidence or complete prompts.
  const nlohmann::json audit_input{
      {"provider", model.provider_name()},
      {"model", model.model_name()},
      {"prompt_version", detail::markov_prompt_version},
      {"system_prompt", request.system_prompt},
      {"user_prompt", request.user_prompt},
      {"response_schema", request.response_schema}};
  return source_import::sha256_text(audit_input.dump());
}

std::string redacted_invalid_response(
    const std::string_view response,
    const std::size_t validation_issue_count) {
  // Invalid output is untrusted and may echo evidence or prompt-injected
  // secrets. Keep only metadata sufficient to correlate repeated failures.
  return nlohmann::json{
      {"redacted", true},
      {"response_sha256", source_import::sha256_text(response)},
      {"response_bytes", response.size()},
      {"validation_issue_count", validation_issue_count}}
      .dump();
}

}  // namespace

ModelRunner::ModelRunner(std::filesystem::path project_root)
    : ModelRunner(std::move(project_root),
                  std::make_shared<CurlHttpClient>()) {}

ModelRunner::ModelRunner(std::filesystem::path project_root,
                         std::shared_ptr<HttpClient> http_client)
    : project_root_(
          std::filesystem::absolute(std::move(project_root)).lexically_normal()),
      config_(read_config(project_root_)),
      language_model_(configured_ollama_model(config_, std::move(http_client))) {
  if (!std::filesystem::is_regular_file(project_root_ / config_.paths.state)) {
    throw ModelError(ModelErrorKind::invalid_project,
                     "project state database does not exist; run 'kc init' first");
  }
}

ModelRunner::ModelRunner(std::filesystem::path project_root,
                         std::unique_ptr<LanguageModel> language_model)
    : project_root_(
          std::filesystem::absolute(std::move(project_root)).lexically_normal()),
      config_(read_config(project_root_)),
      language_model_(std::move(language_model)) {
  if (!std::filesystem::is_regular_file(project_root_ / config_.paths.state)) {
    throw ModelError(ModelErrorKind::invalid_project,
                     "project state database does not exist; run 'kc init' first");
  }
  if (!language_model_) {
    throw ModelError(ModelErrorKind::invalid_project,
                     "language model adapter must not be null");
  }
}

ModelRunResult ModelRunner::generate_markov_chains(
    const ProposalGenerationRequest& request) {
  const auto request_validation = validator_.validate_request(request);
  if (!request_validation) {
    const auto detail = request_validation.issues.empty()
                            ? "invalid model request"
                            : request_validation.issues.front().message;
    throw ModelError(ModelErrorKind::invalid_request, detail);
  }

  const auto prompt = detail::build_markov_chains_prompt(request);
  const auto hash = request_hash(*language_model_, prompt);
  const auto run_id = domain::generate_run_id();
  const auto started_at = utc_now();
  const auto state_path = project_root_ / config_.paths.state;

  LanguageModelResponse response;
  try {
    response = language_model_->generate(prompt);
  } catch (const std::exception&) {
    // Persist a fixed diagnostic rather than provider exception text. HTTP
    // errors can contain URLs, headers, model infrastructure, or response data.
    record_model_run(
        state_path, run_id, language_model_->provider_name(),
        language_model_->model_name(), detail::markov_prompt_version, hash,
        domain::ModelRunStatus::failed, started_at, std::nullopt,
        "local model request failed");
    return {
        .run_id = run_id,
        .status = domain::ModelRunStatus::failed,
        .issues = {{"/", "local model request failed"}}};
  }

  nlohmann::json response_document;
  try {
    response_document = nlohmann::json::parse(response.content);
  } catch (const nlohmann::json::exception&) {
    const std::vector<domain::ValidationIssue> issues{
        {"/", "model response was not valid JSON"}};
    record_model_run(
        state_path, run_id, language_model_->provider_name(),
        language_model_->model_name(), detail::markov_prompt_version, hash,
        domain::ModelRunStatus::invalid_response, started_at,
        redacted_invalid_response(response.content, issues.size()),
        "model response failed validation");
    return {
        .run_id = run_id,
        .status = domain::ModelRunStatus::invalid_response,
        .issues = issues};
  }

  const auto parsed = domain::parse_article_proposal(response_document);
  if (!parsed) {
    record_model_run(
        state_path, run_id, language_model_->provider_name(),
        language_model_->model_name(), detail::markov_prompt_version, hash,
        domain::ModelRunStatus::invalid_response, started_at,
        redacted_invalid_response(response.content, parsed.issues.size()),
        "model response failed validation");
    return {
        .run_id = run_id,
        .status = domain::ModelRunStatus::invalid_response,
        .issues = parsed.issues};
  }

  const auto semantic = validator_.validate_response(*parsed.value, request);
  if (!semantic) {
    record_model_run(
        state_path, run_id, language_model_->provider_name(),
        language_model_->model_name(), detail::markov_prompt_version, hash,
        domain::ModelRunStatus::invalid_response, started_at,
        redacted_invalid_response(response.content, semantic.issues.size()),
        "model response failed validation");
    return {
        .run_id = run_id,
        .status = domain::ModelRunStatus::invalid_response,
        .issues = semantic.issues};
  }

  const nlohmann::json validated_proposal = *parsed.value;
  record_model_run(
      state_path, run_id, language_model_->provider_name(),
      language_model_->model_name(), detail::markov_prompt_version, hash,
      domain::ModelRunStatus::completed, started_at,
      validated_proposal.dump(), std::nullopt);
  return {
      .run_id = run_id,
      .status = domain::ModelRunStatus::completed,
      .proposal = std::move(*parsed.value)};
}

}  // namespace kc::models
