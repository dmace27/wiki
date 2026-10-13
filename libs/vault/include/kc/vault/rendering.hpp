#pragma once
#include "kc/vault/vault_writer.hpp"
#include <unordered_map>
#include <unordered_set>

namespace kc::vault {
/// Metadata required to create a readable, immutable source footnote.
struct SourceReference {
  domain::PageId page_id;
  domain::SourceVersionId source_version_id;
  domain::SourceId source_id;
  std::uint32_t page_number{1};
  domain::SourceKind source_kind{domain::SourceKind::text};
  std::string display_name;
  std::string original_filename;
  std::filesystem::path stored_path;
  domain::Sha256 source_sha256;
  std::filesystem::path vault_source_path;
};

struct CitationState {
  std::unordered_map<std::string, SourceReference> pages;
  std::unordered_set<std::string> normalized_rows;
};


struct RenderedArticle {
  std::string content;
  std::vector<std::string> aliases;
};

/// Pure rendering: callers supply all approved content/source metadata and
/// optional existing bytes. No filesystem or database access is performed.
[[nodiscard]] RenderedArticle render_article(const domain::ArticleProposal& proposal,
    const domain::ArticleId& article_id, const domain::VaultConfig& config,
    const CitationState& citations, const std::optional<std::string>& existing);

/// Complete line-oriented unified diff; large middles use a correct replacement
/// hunk instead of allocating an unbounded quadratic comparison matrix.
[[nodiscard]] std::string rendered_diff(std::string_view before, std::string_view after,
                                      std::string_view destination);
}  // namespace kc::vault
