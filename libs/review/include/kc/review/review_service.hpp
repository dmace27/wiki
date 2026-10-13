#pragma once

#include "kc/domain/types.hpp"
#include <nlohmann/json.hpp>
#include "kc/vault/preview.hpp"
#include "kc/extraction/adapters.hpp"

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
  std::optional<domain::Sha256> image_sha256;
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
  std::optional<domain::PageId> previous_page_id;
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
  /// Immutable before/after claims, explicit removal list, mode, and base.
  std::optional<nlohmann::json> update_changes;
  vault::ArticlePreview preview;
};

/// Implements the minimal extraction and proposal review surface.
///
/// This service uses only the read-only preview capability of the vault layer.
/// It never mutates articles. Review actions record extraction acceptance or
/// correction revisions, rejection decisions, and unused approval revocation.
class ReviewService {
 public:
  explicit ReviewService(std::filesystem::path project_root);

  /// Load the latest extracted version and return image, text, and status for
  /// every page in page-number order.
  [[nodiscard]] ExtractionReview review_extraction(
      const domain::SourceId& source_id) const;

  /// Correct a current page and mark it reviewed. Referenced evidence receives
  /// a new page identity linked to the immutable predecessor; uncited evidence
  /// updates in place with a monotonic revision. Optional expectations bind
  /// acceptance to the exact text/page inspected by the caller.
  [[nodiscard]] PageCorrectionResult correct_page_text(
      const domain::SourceId& source_id, std::uint32_t page_number,
      std::string corrected_text,
      const std::optional<domain::PageId>& expected_page_id = std::nullopt,
      const std::optional<std::string>& expected_text_sha256 = std::nullopt) const;

  /// Record human acceptance of the inspected text without requiring a copy.
  [[nodiscard]] PageCorrectionResult accept_page_text(const domain::SourceId& source_id,
                                                     std::uint32_t page_number) const;
  /// Return an absolute, containment/hash-verified image for a selected page.
  [[nodiscard]] std::filesystem::path page_image(const domain::SourceId& source_id,
                                               std::uint32_t page_number) const;
  /// Launch the verified image through a direct argv call, with no shell.
  /// An injected runner keeps terminal opening verifiable without starting GUI apps.
  [[nodiscard]] std::filesystem::path open_page_image(const domain::SourceId& source_id,
      std::uint32_t page_number, extraction::CommandRunner& runner) const;
  /// Open the exact image from an inspected snapshot, pairing it with the same
  /// text even if a later extraction changes the current pointer.
  [[nodiscard]] std::filesystem::path open_page_image(const ExtractionReviewPage& snapshot,
      extraction::CommandRunner& runner) const;
  /// Move an unused approval back to pending; never writes the vault.
  void revoke_approval(const domain::ProposalId& proposal_id) const;

  /// List proposals, optionally restricted to one exact persisted status.
  [[nodiscard]] std::vector<ProposalSummary> list_proposals(
      std::optional<domain::ProposalStatus> status = std::nullopt) const;

  /// Load immutable proposal sections and every normalized citation joined to
  /// its page image, extracted text, and extraction status.
  [[nodiscard]] ProposalReview review_proposal(
      const domain::ProposalId& proposal_id,
      const vault::ApplyOptions& options = {}) const;

  /// Reject pending or unused approved work without changing a vault file.
  void reject_proposal(const domain::ProposalId& proposal_id,
                       std::optional<std::string> reason = std::nullopt) const;

 private:
  std::filesystem::path project_root_;
  domain::ProjectConfig config_;
};

}  // namespace kc::review
