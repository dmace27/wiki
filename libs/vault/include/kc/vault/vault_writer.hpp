#pragma once

#include "kc/domain/types.hpp"

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>

namespace kc::vault {

/// Stable failure categories used by the CLI to honor its documented exits.
enum class VaultErrorKind {
  invalid_project,
  proposal_not_found,
  invalid_proposal_state,
  validation_failed,
  unsafe_write,
  state_error,
};

/// A classified approval or vault-write failure.
class VaultError : public std::runtime_error {
public:
  VaultError(VaultErrorKind kind, const std::string &message)
      : std::runtime_error(message), kind_(kind) {}

  [[nodiscard]] VaultErrorKind kind() const noexcept { return kind_; }

private:
  VaultErrorKind kind_;
};

/// Explicitly dangerous options are opt-in and default to preserving files.
struct ApplyOptions {
  /// Permit a create proposal to replace a colliding file that is not tracked
  /// as an article. The previous bytes are still backed up first.
  bool allow_overwrite_user_file{false};
};

/// Durable identifiers and paths produced by one successful application.
struct ApplyResult {
  domain::RunId apply_run_id;
  domain::ProposalId proposal_id;
  domain::ArticleId article_id;
  /// Project-relative path to the written Markdown article.
  std::filesystem::path vault_path;
  /// Project-relative recovery copy, absent when the article was newly created.
  std::optional<std::filesystem::path> backup_path;
  domain::Sha256 content_sha256;
};

/// Review and safely apply structured article proposals.
///
/// VaultWriter is the only MVP component with a vault-writing capability. It
/// requires an approved proposal, copies cited immutable sources, replaces
/// only its configured managed region, writes through a temporary sibling,
/// records recovery/audit state, and indexes only the committed new content.
class VaultWriter {
public:
  explicit VaultWriter(std::filesystem::path project_root);

  /// Move a pending proposal to approved without changing the vault.
  void approve(const domain::ProposalId &proposal_id);

  /// Apply an approved proposal and return its durable audit information.
  [[nodiscard]] ApplyResult apply(const domain::ProposalId &proposal_id,
                                  const ApplyOptions &options = {});

private:
  std::filesystem::path project_root_;
  domain::ProjectConfig config_;
};

} // namespace kc::vault
