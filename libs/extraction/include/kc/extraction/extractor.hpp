#pragma once

#include "kc/domain/types.hpp"
#include "kc/extraction/adapters.hpp"

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <vector>

namespace kc::extraction {

enum class ExtractionErrorKind {
  invalid_project,
  source_not_found,
  invalid_state,
  adapter_error,
  state_error,
};

class ExtractionError : public std::runtime_error {
 public:
  ExtractionError(ExtractionErrorKind kind, const std::string& message)
      : std::runtime_error(message), kind_(kind) {}

  [[nodiscard]] ExtractionErrorKind kind() const noexcept { return kind_; }

 private:
  ExtractionErrorKind kind_;
};

struct ExtractionResult {
  domain::SourceId source_id;
  domain::SourceVersionId source_version_id;
  domain::RunId run_id;
  std::vector<domain::ExtractedPage> pages;
  /// True when existing page rows were returned without re-running adapters.
  bool reused{false};
  /// Count of pages explicitly persisted with `text_status = failed`.
  std::size_t failed_pages{0};
};

/// Converts the latest immutable version of a source into page-level state.
///
/// The default constructor selects Poppler and the configured local Tesseract
/// provider. The injectable constructor keeps the pipeline independent of
/// concrete rendering/OCR implementations.
class Extractor {
 public:
  explicit Extractor(std::filesystem::path project_root);
  Extractor(std::filesystem::path project_root,
            std::unique_ptr<PdfAdapter> pdf_adapter,
            std::unique_ptr<OcrProvider> ocr_provider);

  /// Extract the newest version for `source_id`.
  ///
  /// Existing pages are returned unchanged unless `force` is true. Forced
  /// extraction is refused after any proposal cites an existing page, because
  /// changing its text or status would invalidate immutable citation evidence.
  /// OCR failures still produce durable page rows with a failed status.
  [[nodiscard]] ExtractionResult extract(
      const domain::SourceId& source_id, bool force = false);

 private:
  std::filesystem::path project_root_;
  domain::ProjectConfig config_;
  std::unique_ptr<PdfAdapter> pdf_adapter_;
  std::unique_ptr<OcrProvider> ocr_provider_;
};

}  // namespace kc::extraction
