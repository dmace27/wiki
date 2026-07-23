#pragma once

#include "kc/domain/types.hpp"

#include <string_view>

namespace kc::domain {

[[nodiscard]] std::string generate_prefixed_ulid(std::string_view prefix);
[[nodiscard]] ProjectId generate_project_id();
[[nodiscard]] SourceId generate_source_id();
[[nodiscard]] SourceVersionId generate_source_version_id();
[[nodiscard]] PageId generate_page_id();
[[nodiscard]] ArticleId generate_article_id();
[[nodiscard]] ProposalId generate_proposal_id();
[[nodiscard]] RunId generate_run_id();

}  // namespace kc::domain

