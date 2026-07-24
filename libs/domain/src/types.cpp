#include "kc/domain/types.hpp"

#include <algorithm>
#include <regex>
#include <set>
#include <string>

namespace kc::domain {
namespace {

constexpr std::string_view ulid_pattern = "[0-9A-HJKMNP-TV-Z]{26}";

/// Check both the object-specific prefix and the 26-character ULID body.
bool has_valid_id(const std::string& value, const std::string_view prefix) {
  const std::regex pattern("^" + std::string(prefix) + std::string(ulid_pattern) + "$");
  return std::regex_match(value, pattern);
}

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
  require(has_valid_id(config.project_id.value, "prj_"), result, "/project_id", "must be a prj_ ULID");
  require(is_project_relative(config.paths.sources), result, "/paths/sources", "must be project-relative");
  require(is_project_relative(config.paths.vault), result, "/paths/vault", "must be project-relative");
  require(is_project_relative(config.paths.state), result, "/paths/state", "must be project-relative");
  require(is_project_relative(config.paths.cache), result, "/paths/cache", "must be project-relative");
  require(is_project_relative(config.vault.source_directory), result, "/vault/source_directory", "must be relative to the vault");
  require(!config.vault.generated_section_id.empty(), result, "/vault/generated_section_id", "must not be empty");
  require(!config.providers.llm.default_provider.empty(), result, "/providers/llm/default", "must not be empty");
  require(!config.providers.llm.ollama.base_url.empty(), result, "/providers/llm/ollama/base_url", "must not be empty");
  require(!config.providers.llm.ollama.model.empty(), result, "/providers/llm/ollama/model", "must not be empty");
  require(config.providers.llm.ollama.timeout_seconds > 0U, result, "/providers/llm/ollama/timeout_seconds", "must be positive");
  require(!config.providers.ocr.default_provider.empty(), result, "/providers/ocr/default", "must not be empty");
  require(!config.providers.ocr.language.empty(), result, "/providers/ocr/language", "must not be empty");
  return result;
}

ValidationResult validate(const ExtractedPage& page) {
  ValidationResult result;
  require(has_valid_id(page.page_id.value, "pg_"), result, "/page_id", "must be a pg_ ULID");
  require(has_valid_id(page.source_version_id.value, "ver_"), result, "/source_version_id", "must be a ver_ ULID");
  require(page.page_number >= 1U, result, "/page_number", "must be at least 1");
  if (page.image_path) {
    require(is_project_relative(*page.image_path), result, "/image_path", "must be project-relative");
  }
  require(!page.language.empty(), result, "/language", "must not be empty");
  return result;
}

ValidationResult validate(const Citation& citation) {
  ValidationResult result;
  require(has_valid_id(citation.page_id.value, "pg_"), result, "/page_id", "must be a pg_ ULID");
  require(citation.start_char < citation.end_char, result, "/end_char", "must be greater than start_char");
  require(!citation.quote.empty(), result, "/quote", "must not be empty");
  return result;
}

ValidationResult validate(const ArticleProposal& proposal) {
  // Structural parsing has already checked JSON shapes. These checks enforce
  // semantic rules such as safe slugs, unique aliases, and cited content.
  ValidationResult result;
  require(proposal.schema_version == 1U, result, "/schema_version", "must equal 1");
  require(!proposal.article.title.empty(), result, "/article/title", "must not be empty");
  require(std::regex_match(proposal.article.slug, std::regex("^[a-z0-9]+(?:-[a-z0-9]+)*$")),
          result, "/article/slug", "must be a lowercase kebab-case slug");
  if (proposal.article.article_id) {
    require(has_valid_id(proposal.article.article_id->value, "art_"), result, "/article/article_id", "must be an art_ ULID");
  }
  const std::set<std::string> aliases(proposal.article.aliases.begin(), proposal.article.aliases.end());
  require(aliases.size() == proposal.article.aliases.size(), result, "/article/aliases", "must contain unique values");
  require(!proposal.sections.empty(), result, "/sections", "must contain at least one section");

  for (std::size_t section_index = 0; section_index < proposal.sections.size(); ++section_index) {
    const auto& section = proposal.sections[section_index];
    const auto section_path = "/sections/" + std::to_string(section_index);
    for (std::size_t block_index = 0; block_index < section.blocks.size(); ++block_index) {
      const auto& block = section.blocks[block_index];
      const auto block_path = section_path + "/blocks/" + std::to_string(block_index);
      require(!block.text.empty(), result, block_path + "/text", "must not be empty");
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
