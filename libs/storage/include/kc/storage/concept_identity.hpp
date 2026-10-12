#pragma once

#include "kc/domain/types.hpp"
#include "kc/storage/database.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;

namespace kc::storage {

/// A user-actionable identity conflict, distinct from a database I/O failure.
class ConceptIdentityError : public StorageError {
 public:
  using StorageError::StorageError;
};

/// Canonical display identity, retaining applied or first pending spelling.
struct ConceptTarget {
  std::string title;
  std::string slug;
  std::optional<domain::ArticleId> article_id;
  std::optional<std::filesystem::path> vault_path;
  std::vector<domain::ProposalId> pending_creates;
};

/// Resolve Unicode/whitespace-equivalent titles, rejecting distinct-title slug
/// collisions and ambiguous legacy identities. This reads existing schema-3
/// rows without rewriting IDs, filenames, or immutable proposal history.
/// Call under the caller's write transaction when publishing state.
[[nodiscard]] ConceptTarget resolve_concept(
    sqlite3* connection, std::string_view title, bool include_pending = true);

/// Reject destinations owned by a different article, including filenames that
/// alias under Unicode normalization/case folding on other operating systems.
void validate_concept_destination(
    sqlite3* connection, const std::filesystem::path& path,
    const std::optional<domain::ArticleId>& article_id);

/// Check a proposal against current article identity. Updates return the
/// stored display title and slug, including case-only legacy proposals.
/// Creates must use the normalized title and canonical slug and must not
/// already have an applied identity. Throws StorageError on conflicts.
[[nodiscard]] domain::ArticleDescriptor resolve_proposal_identity(
    sqlite3* connection, const domain::ArticleProposal& proposal);

}  // namespace kc::storage
