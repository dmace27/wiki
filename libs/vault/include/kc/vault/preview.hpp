#pragma once
#include "kc/vault/vault_writer.hpp"
#include <nlohmann/json.hpp>
#include <vector>

namespace kc::vault {
/// Complete inspection result. Invalid proposals expose issues without
/// authorizing a write; absent before/after content is represented by null.
struct ArticlePreview {
  domain::ProposalId proposal_id;
  std::optional<domain::ArticleId> article_id;
  std::filesystem::path destination;
  bool valid{false};
  bool untracked_collision{false};
  bool allow_overwrite_user_file{false};
  std::optional<std::string> before;
  std::optional<std::string> after;
  std::string diff;
  std::string token;
  std::vector<domain::ValidationIssue> issues;
  std::optional<bool> matches_approval;
};

/// Read-only preview service. Rendering is pure and shared with apply; previews
/// do not copy sources, make backups, alter proposal state, or write the vault.
class PreviewService {
 public:
  explicit PreviewService(std::filesystem::path root);
  [[nodiscard]] ArticlePreview preview(const domain::ProposalId& proposal_id,
                                       const ApplyOptions& options = {}) const;
 private:
  std::filesystem::path root_;
  domain::ProjectConfig config_;
};
[[nodiscard]] nlohmann::json preview_json(const ArticlePreview& preview);
}  // namespace kc::vault
