#pragma once

#include "kc/domain/types.hpp"

#include <cstddef>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace kc::search {

/// Stable failure categories used by the CLI's documented exit-code mapping.
enum class SearchErrorKind {
  invalid_project,
  invalid_query,
  state_error,
};

/// A classified failure while opening or querying the local article index.
class SearchError : public std::runtime_error {
 public:
  SearchError(SearchErrorKind kind, const std::string& message)
      : std::runtime_error(message), kind_(kind) {}

  [[nodiscard]] SearchErrorKind kind() const noexcept { return kind_; }

 private:
  SearchErrorKind kind_;
};

/// One generated article matched by title, alias, or body text.
struct SearchResult {
  domain::ArticleId article_id;
  std::string title;
  /// Project-relative Markdown path suitable for opening in the vault.
  std::filesystem::path vault_path;
  /// A short FTS5 excerpt with matching terms surrounded by square brackets.
  std::string excerpt;
  /// FTS5 BM25 score; smaller values are more relevant.
  double relevance{0.0};
};

/// Query the project-local SQLite FTS5 article index.
///
/// VaultWriter owns indexing and updates it only after a successful atomic
/// article write. SearchIndex is deliberately read-only and returns only
/// durable article identity plus project-relative vault locations.
class SearchIndex {
 public:
  explicit SearchIndex(std::filesystem::path project_root);

  /// Search all supplied terms with concept-first title and alias weighting.
  ///
  /// `limit` must be between 1 and 100. Punctuation is treated as a separator,
  /// and FTS syntax characters are never interpreted as query operators.
  [[nodiscard]] std::vector<SearchResult> search(
      std::string_view query, std::size_t limit = 20U) const;

 private:
  std::filesystem::path project_root_;
  domain::ProjectConfig config_;
};

}  // namespace kc::search
