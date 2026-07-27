#pragma once

#include "kc/domain/types.hpp"

#include <filesystem>
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
  /// False for a content hash already present in project state.
  bool version_created{false};
};

/// Registers supported source files and retains immutable project-local copies.
///
/// Logical identity is the original filename and source kind. Importing changed
/// bytes under that identity appends a SourceVersion; importing a hash already
/// known to the project returns the existing version without adding rows.
class SourceImporter {
 public:
  /// Open an initialized project rooted at `project_root`.
  explicit SourceImporter(std::filesystem::path project_root);

  /// Import one Markdown, text, or PDF file.
  [[nodiscard]] ImportResult import_file(const std::filesystem::path& input_path);

  /// Import files in argument order. Each completed file is durable even if a
  /// later file fails.
  [[nodiscard]] std::vector<ImportResult> import_files(
      const std::vector<std::filesystem::path>& input_paths);

 private:
  std::filesystem::path project_root_;
  domain::ProjectConfig config_;
};

}  // namespace kc::source_import
