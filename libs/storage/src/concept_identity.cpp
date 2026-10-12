#include "kc/storage/concept_identity.hpp"

#include "kc/domain/concept.hpp"
#include "kc/domain/path.hpp"
#include "kc/storage/database.hpp"

#include <sqlite3.h>

#include <memory>
#include <stdexcept>
#include <utility>

namespace kc::storage {
namespace {

struct IdentityRow {
  std::string id;
  std::string title;
  std::string slug;
  bool applied{false};
  std::string vault_path;
};

/// Keep this read-only scan independent of SQLite's ASCII-only NOCASE. The
/// MVP has a small concept catalog; a persisted Unicode key can be added later
/// without changing these comparison rules or merging existing user pages.
std::vector<IdentityRow> read_identities(sqlite3* connection,
                                       const bool include_pending) {
  const char* sql = include_pending
      ? "SELECT article_id, title, slug, 1, vault_path FROM articles UNION ALL "
        "SELECT proposal_id, json_extract(payload_json, '$.article.title'), "
        "json_extract(payload_json, '$.article.slug'), 0, NULL FROM proposals "
        "WHERE operation = 'create_article' AND article_id IS NULL "
        "AND status IN ('pending', 'approved') ORDER BY 4 DESC, 1"
      : "SELECT article_id, title, slug, 1, vault_path FROM articles ORDER BY article_id";
  sqlite3_stmt* raw = nullptr;
  if (sqlite3_prepare_v2(connection, sql, -1, &raw, nullptr) != SQLITE_OK) {
    throw StorageError("could not read concept identities: " +
                       std::string(sqlite3_errmsg(connection)));
  }
  const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(
      raw, &sqlite3_finalize);
  const auto text = [&](const int column) {
    const auto* value = sqlite3_column_text(raw, column);
    return value == nullptr ? std::string{} : std::string(
        reinterpret_cast<const char*>(value),
        static_cast<std::size_t>(sqlite3_column_bytes(raw, column)));
  };
  std::vector<IdentityRow> rows;
  int status = SQLITE_DONE;
  while ((status = sqlite3_step(raw)) == SQLITE_ROW) {
    rows.push_back({text(0), text(1), text(2), sqlite3_column_int(raw, 3) != 0, text(4)});
  }
  if (status != SQLITE_DONE) {
    throw StorageError("could not read concept identities: " +
                       std::string(sqlite3_errmsg(connection)));
  }
  return rows;
}

}  // namespace

ConceptTarget resolve_concept(sqlite3* connection, const std::string_view title,
                              const bool include_pending) {
  try {
    ConceptTarget target{.title = domain::normalize_concept_title(title),
                         .slug = domain::canonical_concept_slug(title)};
    const auto key = domain::concept_identity_key(title);
    const auto rows = read_identities(connection, include_pending);
    bool found = false;
    for (const auto& row : rows) {
      if (domain::concept_identity_key(row.title) != key) {
        continue;
      }
      if (row.applied && target.article_id) {
        throw ConceptIdentityError(
            "ambiguous Unicode concept identity: '" + row.title +
            "' identifies multiple stored articles; curate those pages before compiling");
      }
      if (!found) {
        // Applied identities precede pending ones. Preserve their original
        // display spelling and historical slug rather than renaming a page.
        if (row.applied) {
          static_cast<void>(domain::concept_filename(row.title));
          target.title = row.title;
          target.slug = row.slug;
          target.article_id = domain::ArticleId{row.id};
          target.vault_path = domain::path_from_utf8(row.vault_path);
          domain::validate_concept_filename(*target.vault_path);
        } else {
          target.title = domain::normalize_concept_title(row.title);
          // Unapplied legacy slugs can be regenerated safely. Applied slugs
          // and paths are immutable identity and always retained above.
          target.slug = domain::canonical_concept_slug(row.title);
        }
        found = true;
      }
      if (!row.applied) {
        target.pending_creates.push_back({row.id});
      }
    }
    for (const auto& row : rows) {
      if (row.slug == target.slug && domain::concept_identity_key(row.title) != key) {
        throw ConceptIdentityError(
            "concept slug collision: '" + target.title + "' and '" +
            row.title + "' both use '" + target.slug +
            "'; choose a more specific title or reject the conflicting pending proposal");
      }
    }
    return target;
  } catch (const std::invalid_argument& error) {
    throw ConceptIdentityError(error.what());
  }
}

void validate_concept_destination(
    sqlite3* connection, const std::filesystem::path& path,
    const std::optional<domain::ArticleId>& article_id) {
  const auto filename = path.generic_u8string();
  const auto key = domain::fold_unicode_text(std::string_view(
      reinterpret_cast<const char*>(filename.data()), filename.size()));
  for (const auto& row : read_identities(connection, false)) {
    if (domain::fold_unicode_text(row.vault_path) == key &&
        (!article_id || row.id != article_id->value)) {
      throw ConceptIdentityError("concept destination already belongs to '" + row.title +
                         "'; choose a distinct title instead of overwriting a tracked page");
    }
  }
}

domain::ArticleDescriptor resolve_proposal_identity(
    sqlite3* connection, const domain::ArticleProposal& proposal) {
  const auto target = resolve_concept(connection, proposal.article.title, false);
  auto article = proposal.article;
  if (proposal.operation == domain::ProposalOperation::create_article) {
    if (target.article_id || article.article_id ||
        article.title != target.title || article.slug != target.slug) {
      throw ConceptIdentityError(
          "create proposal conflicts with canonical concept identity; compile it again");
    }
  } else {
    if (!target.article_id || article.article_id != target.article_id ||
        article.slug != target.slug) {
      throw ConceptIdentityError("update proposal does not match its stored concept identity");
    }
    article.title = target.title;
    article.slug = target.slug;
  }
  return article;
}

}  // namespace kc::storage
