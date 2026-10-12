#pragma once

#include "kc/domain/types.hpp"

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <vector>

namespace kc::source_import {

enum class ImportErrorKind {
  invalid_project,
  invalid_input,
  unavailable_input,
  state_error,
};

/// A user-facing import failure with enough classification for CLI exit codes.
class ImportError : public std::runtime_error {
 public:
  ImportError(ImportErrorKind kind, const std::string& message)
      : std::runtime_error(message), kind_(kind) {}

  [[nodiscard]] ImportErrorKind kind() const noexcept { return kind_; }

 private:
  ImportErrorKind kind_;
};

struct ImportResult {
  domain::Source source;
  domain::SourceVersion version;
  /// True only when this call registered a new logical source.
  bool source_created{false};
  /// False only when an explicit revision repeats the current bytes.
  bool version_created{false};
  /// True when retained bytes were reused, including across logical sources.
  bool deduplicated{false};
  /// True when a new revision restores bytes from an earlier revision.
  bool restored{false};
};

struct ImportOptions {
  /// Omit to create a new logical source; supply to revise an existing source.
  std::optional<domain::SourceId> source_id;
};

struct ImportFailure {
  ImportErrorKind kind;
  std::string message;
};

/// Exactly one of result/failure is populated for each requested input.
struct ImportFileResult {
  std::filesystem::path input_path;
  std::optional<ImportResult> result;
  std::optional<ImportFailure> failure;
};

/// Registers supported source files and retains immutable project-local copies.
///
/// New imports always receive distinct portable source IDs. Revisions require
/// an explicit source ID and must retain its source kind. Global byte reuse
/// never merges logical sources. Repeating current bytes is idempotent; older
/// bytes create a new latest revision without mutating historical evidence.
class SourceImporter {
 public:
  /// Open an initialized project rooted at `project_root`.
  explicit SourceImporter(std::filesystem::path project_root);

  /// Import one Markdown, text, or PDF file.
  [[nodiscard]] ImportResult import_file(
      const std::filesystem::path& input_path, const ImportOptions& options = {});

  /// Import new sources in argument order, continuing after per-file failures.
  /// Each success commits independently and remains visible in the result.
  [[nodiscard]] std::vector<ImportFileResult> import_files(
      const std::vector<std::filesystem::path>& input_paths);

 private:
  std::filesystem::path project_root_;
  domain::ProjectConfig config_;
};

}  // namespace kc::source_import
