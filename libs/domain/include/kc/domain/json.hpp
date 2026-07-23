#pragma once

#include "kc/domain/types.hpp"

#include <nlohmann/json_fwd.hpp>

#include <optional>
#include <string>
#include <vector>

namespace kc::domain {

template <typename T>
struct ParseResult {
  std::optional<T> value;
  std::vector<ValidationIssue> issues;

  [[nodiscard]] bool valid() const noexcept { return value.has_value() && issues.empty(); }
  explicit operator bool() const noexcept { return valid(); }
};

[[nodiscard]] ParseResult<ProjectConfig> parse_project_config(const nlohmann::json& document);
[[nodiscard]] ParseResult<ExtractedPage> parse_extracted_page(const nlohmann::json& document);
[[nodiscard]] ParseResult<Citation> parse_citation(const nlohmann::json& document);
[[nodiscard]] ParseResult<ArticleProposal> parse_article_proposal(const nlohmann::json& document);

void to_json(nlohmann::json& document, const ProjectConfig& value);
void from_json(const nlohmann::json& document, ProjectConfig& value);
void to_json(nlohmann::json& document, const ExtractedPage& value);
void from_json(const nlohmann::json& document, ExtractedPage& value);
void to_json(nlohmann::json& document, const Citation& value);
void from_json(const nlohmann::json& document, Citation& value);
void to_json(nlohmann::json& document, const ArticleProposal& value);
void from_json(const nlohmann::json& document, ArticleProposal& value);

}  // namespace kc::domain

