#include "kc/domain/types.hpp"
#include "kc/domain/concept.hpp"
#include "kc/domain/markdown.hpp"
#include "kc/domain/id.hpp"
#include "kc/domain/schema.hpp"

#include <algorithm>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>

namespace kc::domain {
namespace {

/// Return true only for non-empty relative paths that cannot climb to a parent.
bool is_project_relative(const std::filesystem::path& path) {
  if (path.empty() || path.is_absolute()) {
    return false;
  }
  return std::ranges::none_of(path, [](const auto& part) { return part == ".."; });
}

/// Merge nested validation errors while prefixing their paths with the parent.
void append(ValidationResult& target, ValidationResult source, const std::string& prefix) {
  for (auto& issue : source.issues) {
    issue.path = prefix + issue.path;
    target.issues.push_back(std::move(issue));
  }
}

/// Add one issue when an invariant is false.
void require(bool condition, ValidationResult& result, std::string path, std::string message) {
  if (!condition) {
    result.issues.push_back({std::move(path), std::move(message)});
  }
}

}  // namespace

ValidationResult validate(const ProjectConfig& config) {
  // Accumulate all independent configuration errors in a single pass so users
  // do not have to fix and rerun once per field.
  ValidationResult result;
  require(config.schema_version == 1U, result, "/schema_version", "must equal 1");
  require(is_valid_id(config.project_id.value, "prj_"), result, "/project_id", "must be a prj_ ULID");
  require(is_project_relative(config.paths.sources), result, "/paths/sources", "must be project-relative");
  require(is_project_relative(config.paths.vault), result, "/paths/vault", "must be project-relative");
  require(is_project_relative(config.paths.state), result, "/paths/state", "must be project-relative");
  require(is_project_relative(config.paths.cache), result, "/paths/cache", "must be project-relative");
  require(is_project_relative(config.vault.source_directory), result, "/vault/source_directory", "must be relative to the vault");
  require(safe_managed_section_id(config.vault.generated_section_id), result, "/vault/generated_section_id", "must be 1-128 ASCII letters, digits, dots, underscores, or hyphens");
  require(!config.providers.llm.default_provider.empty(), result, "/providers/llm/default", "must not be empty");
  require(!config.providers.llm.ollama.base_url.empty(), result, "/providers/llm/ollama/base_url", "must not be empty"); require(!config.providers.llm.ollama.model.empty(), result, "/providers/llm/ollama/model", "must not be empty");
  require(config.providers.llm.ollama.timeout_seconds > 0U, result, "/providers/llm/ollama/timeout_seconds", "must be positive");
  require(config.providers.llm.ollama.timeout_seconds <= max_timeout_seconds,
          result, "/providers/llm/ollama/timeout_seconds", "exceeds the portable curl timeout bound");
  require(!config.providers.ocr.default_provider.empty(), result, "/providers/ocr/default", "must not be empty");
  require(!config.providers.ocr.language.empty(), result, "/providers/ocr/language", "must not be empty");
  return result;
}

ValidationResult validate(const ExtractedPage& page) {
  ValidationResult result;
  require(is_valid_id(page.page_id.value, "pg_"), result, "/page_id", "must be a pg_ ULID");
  require(is_valid_id(page.source_version_id.value, "ver_"), result, "/source_version_id", "must be a ver_ ULID");
  require(!to_string(page.text_status).empty(), result, "/text_status", "must be a supported text status");
  require(page.page_number >= 1U, result, "/page_number", "must be at least 1");
  if (page.image_path) {
    require(is_project_relative(*page.image_path), result, "/image_path", "must be project-relative");
  }
  require(!page.language.empty(), result, "/language", "must not be empty");
  return result;
}

ValidationResult validate(const Citation& citation) {
  ValidationResult result;
  require(is_valid_id(citation.page_id.value, "pg_"), result, "/page_id", "must be a pg_ ULID");
  require(citation.start_char <= max_citation_start, result, "/start_char", "exceeds signed SQLite offset storage");
  require(citation.end_char <= max_citation_end, result, "/end_char", "exceeds signed SQLite offset storage");
  require(citation.start_char < citation.end_char, result, "/end_char", "must be greater than start_char");
  append(result, validate_plain_text(citation.quote, false), "/quote");
  return result;
}

ValidationResult validate(const ArticleProposal& proposal) {
  // The shared semantics also protect typed adapters that did not originate
  // in JSON. Structural schema checks run before JSON values reach this layer.
  auto result = validate_markdown(proposal);
  require(proposal.schema_version == 1U, result, "/schema_version", "must equal 1");
  require(!to_string(proposal.operation).empty(), result, "/operation", "must be a supported operation");
  require(proposal.operation != ProposalOperation::create_article || !proposal.article.article_id,
          result, "/article/article_id", "must be absent for create_article");
  require(proposal.operation != ProposalOperation::update_article || proposal.article.article_id.has_value(),
          result, "/article/article_id", "is required for update_article");
  try {
    static_cast<void>(normalize_concept_title(proposal.article.title));
    static_cast<void>(concept_filename(proposal.article.title));
  } catch (const std::invalid_argument& error) {
    require(false, result, "/article/title", error.what());
  }
  require(std::regex_match(proposal.article.slug, std::regex("^[a-z0-9]+(?:-[a-z0-9]+)*$")),
          result, "/article/slug", "must be a lowercase kebab-case slug");
  if (proposal.article.article_id) {
    require(is_valid_id(proposal.article.article_id->value, "art_"), result, "/article/article_id", "must be an art_ ULID");
  }
  const std::set<std::string> aliases(proposal.article.aliases.begin(), proposal.article.aliases.end());
  require(aliases.size() == proposal.article.aliases.size(), result, "/article/aliases", "must contain unique values");
  require(!proposal.sections.empty(), result, "/sections", "must contain at least one section");

  for (std::size_t section_index = 0; section_index < proposal.sections.size(); ++section_index) {
    const auto& section = proposal.sections[section_index];
    const auto section_path = "/sections/" + std::to_string(section_index);
    require(!to_string(section.key).empty(), result, section_path + "/key", "must be a supported section key");
    // Repeated keys intentionally preserve separate, ordered sections. Every
    // occurrence must contribute supported prose rather than an empty heading.
    require(!section.blocks.empty(), result, section_path + "/blocks", "must contain at least one cited block");
    for (std::size_t block_index = 0; block_index < section.blocks.size(); ++block_index) {
      const auto& block = section.blocks[block_index];
      const auto block_path = section_path + "/blocks/" + std::to_string(block_index);
      require(!to_string(block.kind).empty(), result, block_path + "/kind", "must be a supported block kind");
      require(!block.citations.empty(), result, block_path + "/citations", "must contain at least one citation");
      for (std::size_t citation_index = 0; citation_index < block.citations.size(); ++citation_index) {
        append(result, validate(block.citations[citation_index]),
               block_path + "/citations/" + std::to_string(citation_index));
      }
    }
  }

  for (std::size_t concept_index = 0; concept_index < proposal.related_concepts.size(); ++concept_index) {
    const auto& related = proposal.related_concepts[concept_index];
    const auto concept_path = "/related_concepts/" + std::to_string(concept_index);
    require(!related.citations.empty(), result, concept_path + "/citations", "must contain at least one citation");
    for (std::size_t citation_index = 0; citation_index < related.citations.size(); ++citation_index) {
      append(result, validate(related.citations[citation_index]),
             concept_path + "/citations/" + std::to_string(citation_index));
    }
  }
  return result;
}

std::string_view to_string(const SourceKind value) {
  // Returning string_view avoids allocating because every result is a literal.
  switch (value) {
    case SourceKind::pdf: return "pdf";
    case SourceKind::markdown: return "markdown";
    case SourceKind::text: return "text";
  }
  return "";
}

std::string_view to_string(const TextStatus value) {
  switch (value) {
    case TextStatus::native: return "native";
    case TextStatus::ocr_unreviewed: return "ocr_unreviewed";
    case TextStatus::reviewed: return "reviewed";
    case TextStatus::failed: return "failed";
  }
  return "";
}

std::string_view to_string(const ProposalOperation value) {
  switch (value) {
    case ProposalOperation::create_article: return "create_article";
    case ProposalOperation::update_article: return "update_article";
  }
  return "";
}

std::string_view to_string(const ProposalStatus value) {
  switch (value) {
    case ProposalStatus::pending: return "pending";
    case ProposalStatus::approved: return "approved";
    case ProposalStatus::rejected: return "rejected";
    case ProposalStatus::applied: return "applied";
    case ProposalStatus::superseded: return "superseded";
  }
  return "";
}

std::string_view to_string(const SectionKey value) {
  switch (value) {
    case SectionKey::working_explanation: return "working_explanation";
    case SectionKey::key_ideas: return "key_ideas";
    case SectionKey::example: return "example";
    case SectionKey::related_concepts: return "related_concepts";
  }
  return "";
}

std::string_view to_string(const BlockKind value) {
  switch (value) {
    case BlockKind::paragraph: return "paragraph";
    case BlockKind::bullet: return "bullet";
  }
  return "";
}

}  // namespace kc::domain
