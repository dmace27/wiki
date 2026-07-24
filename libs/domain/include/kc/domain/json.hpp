#pragma once

#include "kc/domain/types.hpp"

#include <nlohmann/json_fwd.hpp>

#include <optional>
#include <string>
#include <vector>

namespace kc::domain {

template <typename T>
struct ParseResult {
  /// Contains the parsed object only when parsing and validation both succeed.
  std::optional<T> value;
  /// Human-readable problems, with JSON-pointer-like paths to bad fields.
  std::vector<ValidationIssue> issues;

  /// Returns true only when there is a value and there are no reported issues.
  [[nodiscard]] bool valid() const noexcept { return value.has_value() && issues.empty(); }
  /// Lets callers write `if (result)` as a short form of `if (result.valid())`.
  explicit operator bool() const noexcept { return valid(); }
};

/// Strictly parses and validates each supported JSON document.
///
/// These functions return issues instead of throwing for malformed input. They
/// also reject unknown properties so model output cannot smuggle in unsupported
/// operations such as arbitrary file writes.
[[nodiscard]] ParseResult<ProjectConfig> parse_project_config(const nlohmann::json& document);
[[nodiscard]] ParseResult<ExtractedPage> parse_extracted_page(const nlohmann::json& document);
[[nodiscard]] ParseResult<Citation> parse_citation(const nlohmann::json& document);
[[nodiscard]] ParseResult<ArticleProposal> parse_article_proposal(const nlohmann::json& document);

/// nlohmann/json conversion hooks.
///
/// `to_json` serializes a C++ value; `from_json` performs the structural part
/// of parsing. Application code should normally use the `parse_*` functions
/// above because they run semantic validation as well.
void to_json(nlohmann::json& document, const ProjectConfig& value);
void from_json(const nlohmann::json& document, ProjectConfig& value);
void to_json(nlohmann::json& document, const ExtractedPage& value);
void from_json(const nlohmann::json& document, ExtractedPage& value);
void to_json(nlohmann::json& document, const Citation& value);
void from_json(const nlohmann::json& document, Citation& value);
void to_json(nlohmann::json& document, const ArticleProposal& value);
void from_json(const nlohmann::json& document, ArticleProposal& value);

}  // namespace kc::domain
