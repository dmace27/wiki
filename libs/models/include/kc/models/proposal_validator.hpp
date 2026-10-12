#pragma once

#include "kc/domain/types.hpp"

#include <string>
#include <vector>

namespace kc::models {

/// Evidence and target identity supplied to the generic concept model prompt.
struct ProposalGenerationRequest {
  std::string concept_title;
  domain::ProposalOperation operation{
      domain::ProposalOperation::create_article};
  std::optional<domain::ArticleId> article_id;
  // Compiler-resolved slug preserves historical identity during updates.
  std::optional<std::string> concept_slug;
  std::vector<domain::ExtractedPage> pages;
};

/// Semantic checks that require both model output and its evidence pages.
///
/// Domain parsing validates JSON shape and local field invariants. This class
/// completes the trust boundary by checking citation existence, offsets, and
/// normalized quotes against the exact text sent to the model.
class ProposalValidator {
 public:
  [[nodiscard]] domain::ValidationResult validate_request(
      const ProposalGenerationRequest& request) const;

  [[nodiscard]] domain::ValidationResult validate_response(
      const domain::ArticleProposal& proposal,
      const ProposalGenerationRequest& request) const;
};

}  // namespace kc::models
