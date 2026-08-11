#pragma once

#include "kc/domain/types.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace kc::review {

/// Stable failure categories used by the CLI's documented exit-code mapping.
enum class ReviewErrorKind {
  invalid_project,
  source_not_found,
  page_not_found,
  proposal_not_found,
  invalid_state,
  invalid_input,
  state_error,
};

/// A classified failure while reading or changing review state.
class ReviewError : public std::runtime_error {
 public:
  ReviewError(ReviewErrorKind kind, const std::string& message)
      : std::runtime_error(message), kind_(kind) {}

  [[nodiscard]] ReviewErrorKind kind() const noexcept { return kind_; }

 private:
  ReviewErrorKind kind_;
};

/// One extraction page with the three values a reviewer must inspect together.
struct ExtractionReviewPage {
  domain::PageId page_id;
  std::uint32_t page_number{1};
  std::optional<std::filesystem::path> image_path;
  std::string text;
  domain::TextStatus text_status{domain::TextStatus::failed};
};

/// Reviewable extraction state for the latest immutable source version.
struct ExtractionReview {
  domain::SourceId source_id;
  domain::SourceVersionId source_version_id;
  std::string display_name;
  domain::SourceKind source_kind{domain::SourceKind::text};
  std::vector<ExtractionReviewPage> pages;
};

/// The durable result of one reviewer-authored text correction.
struct PageCorrectionResult {
  domain::SourceId source_id;
  domain::SourceVersionId source_version_id;
  domain::PageId page_id;
  std::uint32_t page_number{1};
  domain::Sha256 text_sha256;
};

/// Compact proposal metadata used by `kc proposal list`.
struct ProposalSummary {
  domain::ProposalId proposal_id;
  std::optional<domain::ArticleId> article_id;
  domain::ProposalOperation operation{
      domain::ProposalOperation::create_article};
  domain::ProposalStatus status{domain::ProposalStatus::pending};
  std::string title;
  domain::Timestamp created_at;
  std::optional<domain::Timestamp> reviewed_at;
  std::optional<std::string> review_reason;
};

/// A normalized citation joined to the source evidence needed to assess it.
struct CitationEvidence {
  std::string section_key;
  std::size_t block_index{0};
  domain::Citation citation;
  domain::SourceId source_id;
  std::string source_name;
  domain::SourceKind source_kind{domain::SourceKind::text};
  domain::SourceVersionId source_version_id;
  std::uint32_t page_number{1};
  std::optional<std::filesystem::path> image_path;
  std::string extracted_text;
  domain::TextStatus text_status{domain::TextStatus::failed};
};

/// Complete, read-only proposal inspection data.
struct ProposalReview {
  ProposalSummary summary;
  domain::ArticleProposal proposal;
  std::vector<CitationEvidence> citation_evidence;
};

/// Implements the minimal extraction and proposal review surface.
///
/// This service intentionally has no VaultWriter reference and exposes no
/// article-content mutation. Its only writes are corrected extraction text and
/// pending-to-rejected proposal review state.
class ReviewService {
 public:
  explicit ReviewService(std::filesystem::path project_root);

  /// Load the latest extracted version and return image, text, and status for
  /// every page in page-number order.
  [[nodiscard]] ExtractionReview review_extraction(
      const domain::SourceId& source_id) const;

  /// Replace one latest-version page's extracted text and mark it reviewed.
  /// A page already referenced by any proposal is immutable so stored
  /// citations cannot be invalidated after compilation.
  [[nodiscard]] PageCorrectionResult correct_page_text(
      const domain::SourceId& source_id, std::uint32_t page_number,
      std::string corrected_text) const;

  /// List proposals, optionally restricted to one exact persisted status.
  [[nodiscard]] std::vector<ProposalSummary> list_proposals(
      std::optional<domain::ProposalStatus> status = std::nullopt) const;

  /// Load immutable proposal sections and every normalized citation joined to
  /// its page image, extracted text, and extraction status.
  [[nodiscard]] ProposalReview review_proposal(
      const domain::ProposalId& proposal_id) const;

  /// Reject a pending proposal without changing an article or vault file.
  void reject_proposal(const domain::ProposalId& proposal_id,
                       std::optional<std::string> reason = std::nullopt) const;

 private:
  std::filesystem::path project_root_;
  domain::ProjectConfig config_;
};

}  // namespace kc::review
