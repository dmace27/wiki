#pragma once

#include "kc/domain/types.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <optional>
#include <string>

struct sqlite3;

namespace kc::updates {

/// The last successfully applied structured content and its managed bytes.
/// Apply-run identity detects intervening writes even if the content repeats.
struct ArticleBase {
  domain::RunId apply_run_id;
  domain::ArticleProposal proposal;
  std::string managed_sha256;
};

/// Read the last applied content and verify compiler-owned bytes on disk.
/// Legacy articles require their original full-file digest for the first update.
/// Missing history and manual managed edits fail closed with a recovery message.
[[nodiscard]] ArticleBase load_base(sqlite3* connection,
    const std::filesystem::path& root, const domain::ProjectConfig& config,
    const domain::ArticleId& article_id);

/// Extract exactly one ownership region, including its boundary whitespace.
[[nodiscard]] std::string managed_region(std::string_view content,
                                        std::string_view section_id);

/// Retain approved claims in order and merge new blocks/relationships. Identical
/// visible claims share their exact citations rather than becoming duplicates.
[[nodiscard]] domain::ArticleProposal accumulate(
    const domain::ArticleProposal& prior, const domain::ArticleProposal& additions);

/// Read a proposal's immutable update contract. Missing bases on old update
/// proposals are refused; users must recompile and review under these rules.
[[nodiscard]] std::optional<nlohmann::json> review_changes(
    sqlite3* connection, const domain::ProposalId& proposal_id,
    const domain::ArticleProposal& proposal);

/// Recheck both the durable article revision and current managed bytes during
/// approval/apply. Surrounding notes and frontmatter do not invalidate the base.
void validate_base(sqlite3* connection, const std::filesystem::path& root,
    const domain::ProjectConfig& config, const domain::ProposalId& proposal_id,
    const domain::ArticleProposal& proposal);

}  // namespace kc::updates
