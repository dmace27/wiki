#pragma once

#include "kc/domain/types.hpp"

#include <string_view>

namespace kc::domain {

/// Exact prefix plus a canonical 26-byte Crockford ULID. The leading digit is
/// 0-7 so the 130-bit encoding cannot overflow its 128-bit value.
[[nodiscard]] bool is_valid_id(std::string_view value, std::string_view prefix);

/// Creates a ULID and prepends `prefix` to make the ID's purpose visible.
///
/// A ULID is a 26-character identifier containing a creation timestamp plus
/// random data. For example, passing `"src_"` returns an ID suitable for a
/// source, such as `src_01J...`.
[[nodiscard]] std::string generate_prefixed_ulid(std::string_view prefix);

/// The following helpers create strongly typed IDs for each domain object.
/// Using separate C++ types prevents accidentally passing, for example, a
/// page ID to code that expects a source ID.
[[nodiscard]] ProjectId generate_project_id();
[[nodiscard]] SourceId generate_source_id();
[[nodiscard]] SourceVersionId generate_source_version_id();
[[nodiscard]] PageId generate_page_id();
[[nodiscard]] ArticleId generate_article_id();
[[nodiscard]] ProposalId generate_proposal_id();
[[nodiscard]] RunId generate_run_id();

}  // namespace kc::domain
