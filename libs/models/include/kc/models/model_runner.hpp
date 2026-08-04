#pragma once

#include "kc/domain/types.hpp"
#include "kc/models/language_model.hpp"
#include "kc/models/ollama_language_model.hpp"
#include "kc/models/proposal_validator.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

namespace kc::models {

enum class ModelErrorKind { invalid_project, invalid_request, state_error };

class ModelError : public std::runtime_error {
 public:
  ModelError(ModelErrorKind kind, const std::string& message)
      : std::runtime_error(message), kind_(kind) {}

  [[nodiscard]] ModelErrorKind kind() const noexcept { return kind_; }

 private:
  ModelErrorKind kind_;
};

/// Durable result of one local model attempt.
struct ModelRunResult {
  domain::RunId run_id;
  domain::ModelRunStatus status{domain::ModelRunStatus::failed};
  std::optional<domain::ArticleProposal> proposal;
  std::vector<domain::ValidationIssue> issues;
};

/// Loads model configuration, executes one provider request, validates it, and
/// records a redacted audit row.
///
/// This class intentionally stops before proposal creation. Part 2A owns the
/// transaction that turns a completed model result into a pending proposal.
class ModelRunner {
 public:
  /// Use the provider configured in `kc.json` and production libcurl HTTP.
  explicit ModelRunner(std::filesystem::path project_root);

  /// Use configured Ollama settings with an injectable HTTP implementation.
  ModelRunner(std::filesystem::path project_root,
              std::shared_ptr<HttpClient> http_client);

  /// Inject any LanguageModel while retaining normal validation and auditing.
  ModelRunner(std::filesystem::path project_root,
              std::unique_ptr<LanguageModel> language_model);

  [[nodiscard]] ModelRunResult generate_markov_chains(
      const ProposalGenerationRequest& request);

 private:
  std::filesystem::path project_root_;
  domain::ProjectConfig config_;
  std::unique_ptr<LanguageModel> language_model_;
  ProposalValidator validator_;
};

}  // namespace kc::models
