#pragma once

#include "kc/domain/types.hpp"
#include "kc/models/language_model.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace kc::compiler {

/// The user-controlled inputs to one proposal compilation.
struct CompileOptions {
  std::string concept_title{"Markov Chains"};
  std::vector<domain::SourceId> source_ids;
};

/// Durable identifiers returned after a pending proposal is committed.
struct CompileResult {
  domain::ProposalId proposal_id;
  domain::RunId model_run_id;
  domain::ProposalOperation operation{
      domain::ProposalOperation::create_article};
  std::optional<domain::ArticleId> article_id;
  std::size_t selected_page_count{0};
};

enum class CompilerErrorKind {
  invalid_project,
  unsupported_concept,
  source_not_found,
  no_evidence,
  model_failed,
  validation_failed,
  state_error
};

/// A classified compiler failure suitable for mapping to the CLI exit contract.
class CompilerError : public std::runtime_error {
 public:
  CompilerError(CompilerErrorKind kind, const std::string& message)
      : std::runtime_error(message), kind_(kind) {}

  [[nodiscard]] CompilerErrorKind kind() const noexcept { return kind_; }

 private:
  CompilerErrorKind kind_;
};

/// Select source evidence, generate a validated proposal, and persist it.
///
/// This service never writes to the vault. The production constructor uses the
/// model configured in `kc.json`; the injectable constructor keeps integration
/// tests local and deterministic without weakening the model validation path.
class Compiler {
 public:
  explicit Compiler(std::filesystem::path project_root);
  Compiler(std::filesystem::path project_root,
           std::unique_ptr<models::LanguageModel> language_model);
  ~Compiler();

  Compiler(const Compiler&) = delete;
  Compiler& operator=(const Compiler&) = delete;
  Compiler(Compiler&&) noexcept;
  Compiler& operator=(Compiler&&) noexcept;

  /// Return relevant evidence in the exact deterministic order sent to the
  /// model. Only active sources and their latest immutable versions are used.
  [[nodiscard]] std::vector<domain::ExtractedPage> select_evidence(
      const CompileOptions& options) const;

  /// Create one immutable pending proposal and its normalized citation rows.
  [[nodiscard]] CompileResult compile(const CompileOptions& options);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace kc::compiler
