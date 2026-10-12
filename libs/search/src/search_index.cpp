#include "kc/search/search_index.hpp"

#include "kc/domain/json.hpp"
#include "kc/domain/concept.hpp"
#include "kc/domain/path.hpp"
#include "kc/storage/database.hpp"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <utility>

namespace kc::search {
namespace {

/// RAII wrapper for the search layer's single parameterized SQLite query.
class Statement {
 public:
  Statement(sqlite3* connection, const std::string_view sql)
      : connection_(connection) {
    const auto query = std::string(sql);
    if (sqlite3_prepare_v2(connection_, query.c_str(), -1, &statement_,
                           nullptr) != SQLITE_OK) {
      throw SearchError(SearchErrorKind::state_error,
                        "failed to prepare article search: " +
                            std::string(sqlite3_errmsg(connection_)));
    }
  }

  ~Statement() { sqlite3_finalize(statement_); }
  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;

  void bind_text(const int index, const std::string_view value) {
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        sqlite3_bind_text(statement_, index, value.data(),
                          static_cast<int>(value.size()),
                          SQLITE_TRANSIENT) != SQLITE_OK) {
      throw SearchError(SearchErrorKind::state_error,
                        "failed to bind article search: " +
                            std::string(sqlite3_errmsg(connection_)));
    }
  }

  void bind_integer(const int index, const std::int64_t value) {
    if (sqlite3_bind_int64(statement_, index, value) != SQLITE_OK) {
      throw SearchError(SearchErrorKind::state_error,
                        "failed to bind article search: " +
                            std::string(sqlite3_errmsg(connection_)));
    }
  }

  [[nodiscard]] int step() {
    const auto status = sqlite3_step(statement_);
    if (status != SQLITE_ROW && status != SQLITE_DONE) {
      throw SearchError(SearchErrorKind::state_error,
                        "article search failed: " +
                            std::string(sqlite3_errmsg(connection_)));
    }
    return status;
  }

  [[nodiscard]] std::string text(const int column) const {
    const auto* value = sqlite3_column_text(statement_, column);
    return value == nullptr
               ? std::string{}
               : std::string(reinterpret_cast<const char*>(value),
                             static_cast<std::size_t>(sqlite3_column_bytes(statement_, column)));
  }

  [[nodiscard]] double real(const int column) const {
    return sqlite3_column_double(statement_, column);
  }

 private:
  sqlite3* connection_;
  sqlite3_stmt* statement_{nullptr};
};

bool is_safe_relative(const std::filesystem::path& path) {
  if (path.empty() || path.is_absolute()) {
    return false;
  }
  return std::ranges::none_of(path, [](const auto& component) {
    return component == "..";
  });
}

/// Resolve containment again for services reused after directories change.
std::filesystem::path checked_state_path(const std::filesystem::path& root,
                                         const domain::ProjectConfig& config) {
  try {
    domain::validate_project_paths(root, config);
  } catch (const std::invalid_argument& error) {
    throw SearchError(SearchErrorKind::invalid_project, error.what());
  }
  return root / config.paths.state;
}

domain::ProjectConfig read_config(const std::filesystem::path& root) {
  if (!domain::is_within(root, root / "kc.json")) {
    throw SearchError(SearchErrorKind::invalid_project, "configuration escapes project root");
  }
  std::ifstream input(root / "kc.json");
  if (!input) {
    throw SearchError(SearchErrorKind::invalid_project,
                      "no readable kc.json at project root");
  }

  try {
    nlohmann::json document;
    input >> document;
    const auto parsed = domain::parse_project_config(document);
    if (!parsed) {
      const auto detail = parsed.issues.empty()
                              ? "unknown validation error"
                              : parsed.issues.front().message;
      throw SearchError(SearchErrorKind::invalid_project,
                        "invalid project configuration: " + detail);
    }
    try {
      domain::validate_project_paths(root, *parsed.value);
    } catch (const std::invalid_argument& error) {
      throw SearchError(SearchErrorKind::invalid_project, error.what());
    }
    return *parsed.value;
  } catch (const nlohmann::json::exception&) {
    throw SearchError(SearchErrorKind::invalid_project,
                      "project configuration is not valid JSON");
  }
}

/// Convert user text to a literal FTS5 expression instead of accepting its
/// operator language. Each Unicode byte sequence stays intact, while ASCII
/// punctuation becomes a separator and every term is quoted independently.
std::string literal_fts_query(const std::string_view query) {
  std::vector<std::string> terms;
  std::string term;
  for (const auto character : domain::fold_unicode_text(query)) {
    const auto byte = static_cast<unsigned char>(character);
    if ((byte >= 'a' && byte <= 'z') ||
        (byte >= '0' && byte <= '9') || byte >= 0x80U) {
      term.push_back(character);
    } else if (!term.empty()) {
      terms.push_back(std::move(term));
      term.clear();
    }
  }
  if (!term.empty()) {
    terms.push_back(std::move(term));
  }
  std::string expression;
  for (const auto& value : terms) {
    if (!expression.empty()) {
      expression += " AND ";
    }
    expression.push_back('"');
    expression += value;
    expression.push_back('"');
  }
  return expression;
}

}  // namespace

SearchIndex::SearchIndex(std::filesystem::path project_root)
    : project_root_(std::filesystem::absolute(std::move(project_root))
                        .lexically_normal()),
      config_(read_config(project_root_)) {
  if (!std::filesystem::is_regular_file(checked_state_path(project_root_, config_))) {
    throw SearchError(
        SearchErrorKind::invalid_project,
        "project state database does not exist; run 'kc init' first");
  }
}

std::vector<SearchResult> SearchIndex::search(
    const std::string_view query, const std::size_t limit) const {
  if (query.size() > 4096U) {
    throw SearchError(SearchErrorKind::invalid_query,
                      "search query must be at most 4096 bytes");
  }
  if (limit == 0U || limit > 100U) {
    throw SearchError(SearchErrorKind::invalid_query,
                      "search result limit must be between 1 and 100");
  }
  std::string expression;
  try {
    expression = literal_fts_query(query);
  } catch (const std::invalid_argument& error) {
    throw SearchError(SearchErrorKind::invalid_query, error.what());
  }

  try {
    storage::Database database(checked_state_path(project_root_, config_));
    Statement statement(
        database.native_handle(),
        "SELECT a.article_id, a.title, a.vault_path, "
        "snippet(article_fts, 3, '[', ']', ' ... ', 24), "
        "bm25(article_fts, 0.0, 10.0, 5.0, 1.0) AS relevance "
        "FROM article_fts JOIN articles a "
        "ON a.article_id = article_fts.article_id "
        "WHERE article_fts MATCH ?1 AND ?2 "
        "ORDER BY relevance, lower(a.title), a.article_id LIMIT ?3");
    // A harmless placeholder keeps MATCH syntactically valid for concepts
    // made entirely of punctuation; exact identity lookup handles them below.
    statement.bind_text(1, expression.empty() ? "\"kc_empty_query\"" : expression);
    statement.bind_integer(2, expression.empty() ? 0 : 1);
    statement.bind_integer(3, static_cast<std::int64_t>(limit));

    std::vector<SearchResult> results;
    while (statement.step() == SQLITE_ROW) {
      SearchResult result{.article_id = {statement.text(0)},
                          .title = statement.text(1),
                          .vault_path = domain::path_from_utf8(statement.text(2)),
                          .excerpt = statement.text(3),
                          .relevance = statement.real(4)};
      if (!is_safe_relative(result.vault_path)) {
        throw SearchError(SearchErrorKind::state_error,
                          "article search returned an unsafe vault path");
      }
      results.push_back(std::move(result));
    }
    // FTS5 has its own (older Unicode) token rules. Exact concept lookup uses
    // the same NFC/full case-fold key as compile/apply, so e.g. Straße/STRASSE
    // and composed/decomposed titles always return the canonical page first.
    std::string identity_key;
    try {
      identity_key = domain::concept_identity_key(query);
    } catch (const std::invalid_argument&) {
      if (expression.empty()) {
        throw SearchError(SearchErrorKind::invalid_query,
                          "search query must contain a word, number, or exact concept title");
      }
      return results;  // Multi-line free-text queries still use literal FTS.
    }
    Statement identities(database.native_handle(),
        "SELECT a.article_id, a.title, a.vault_path, substr(f.body, 1, 160) "
        "FROM articles a LEFT JOIN article_fts f ON f.article_id = a.article_id "
        "ORDER BY a.article_id");
    std::optional<SearchResult> exact;
    while (identities.step() == SQLITE_ROW) {
      if (domain::concept_identity_key(identities.text(1)) != identity_key) {
        continue;
      }
      if (exact) {
        throw SearchError(SearchErrorKind::state_error,
                          "ambiguous Unicode concept identity; curate the duplicate articles");
      }
      exact = SearchResult{.article_id = {identities.text(0)},
                           .title = identities.text(1),
                           .vault_path = domain::path_from_utf8(identities.text(2)),
                           .excerpt = identities.text(3),
                           .relevance = 0.0};
      if (!is_safe_relative(exact->vault_path)) {
        throw SearchError(SearchErrorKind::state_error,
                          "article search returned an unsafe vault path");
      }
    }
    if (expression.empty() && !exact) {
      throw SearchError(SearchErrorKind::invalid_query,
                        "search query must contain a word, number, or exact concept title");
    }
    if (exact) {
      const auto existing = std::ranges::find_if(results, [&](const auto& result) {
        return result.article_id == exact->article_id;
      });
      if (existing != results.end()) {
        *exact = *existing;  // Keep the FTS excerpt and score when available.
        results.erase(existing);
      }
      results.insert(results.begin(), std::move(*exact));
      if (results.size() > limit) {
        results.resize(limit);
      }
    }
    return results;
  } catch (const SearchError&) {
    throw;
  } catch (const std::invalid_argument& error) {
    throw SearchError(SearchErrorKind::state_error, error.what());
  } catch (const storage::StorageError& error) {
    throw SearchError(SearchErrorKind::state_error,
                      "could not query article index: " +
                          std::string(error.what()));
  }
}

}  // namespace kc::search
