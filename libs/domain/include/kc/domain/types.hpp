#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kc::domain {

template <typename Tag>
struct Id {
  std::string value;

  auto operator<=>(const Id&) const = default;
};

struct ProjectIdTag;
struct SourceIdTag;
struct SourceVersionIdTag;
struct PageIdTag;
struct ArticleIdTag;
struct ProposalIdTag;
struct RunIdTag;

using ProjectId = Id<ProjectIdTag>;
using SourceId = Id<SourceIdTag>;
using SourceVersionId = Id<SourceVersionIdTag>;
using PageId = Id<PageIdTag>;
using ArticleId = Id<ArticleIdTag>;
using ProposalId = Id<ProposalIdTag>;
using RunId = Id<RunIdTag>;

using Timestamp = std::string;
using Sha256 = std::string;

enum class SourceKind { pdf, markdown, text };
enum class TextStatus { native, ocr_unreviewed, reviewed, failed };
enum class ExtractionStatus { running, completed, failed };
enum class ModelRunStatus { completed, invalid_response, failed };
enum class ProposalOperation { create_article, update_article };
enum class ProposalStatus { pending, approved, rejected, applied, superseded };
enum class SectionKey { working_explanation, key_ideas, example, related_concepts };
enum class BlockKind { paragraph, bullet };

struct ProjectPaths {
  std::filesystem::path sources{"sources"};
  std::filesystem::path vault{"vault"};
  std::filesystem::path state{".knowledge-compiler/state.sqlite"};
  std::filesystem::path cache{".knowledge-compiler"};

  auto operator<=>(const ProjectPaths&) const = default;
};

struct VaultConfig {
  std::filesystem::path source_directory{"_sources"};
  std::string generated_section_id{"knowledge-compiler"};

  auto operator<=>(const VaultConfig&) const = default;
};

struct OllamaConfig {
  std::string base_url{"http://127.0.0.1:11434"};
  std::string model{"gemma3:12b"};
  std::uint32_t timeout_seconds{300};

  auto operator<=>(const OllamaConfig&) const = default;
};

struct LlmConfig {
  std::string default_provider{"ollama"};
  OllamaConfig ollama;

  auto operator<=>(const LlmConfig&) const = default;
};

struct OcrConfig {
  std::string default_provider{"tesseract"};
  std::string language{"eng"};

  auto operator<=>(const OcrConfig&) const = default;
};

struct ProviderConfig {
  LlmConfig llm;
  OcrConfig ocr;

  auto operator<=>(const ProviderConfig&) const = default;
};

struct ProjectConfig {
  std::uint32_t schema_version{1};
  ProjectId project_id;
  ProjectPaths paths;
  VaultConfig vault;
  ProviderConfig providers;

  auto operator<=>(const ProjectConfig&) const = default;
};

struct Source {
  SourceId source_id;
  std::string display_name;
  SourceKind source_kind{SourceKind::text};
  Timestamp created_at;
  std::optional<Timestamp> archived_at;
};

struct SourceVersion {
  SourceVersionId source_version_id;
  SourceId source_id;
  Sha256 sha256;
  std::string original_filename;
  std::filesystem::path stored_path;
  std::string media_type;
  std::uint64_t byte_size{0};
  Timestamp imported_at;
};

struct ExtractedPage {
  PageId page_id;
  SourceVersionId source_version_id;
  std::uint32_t page_number{1};
  std::optional<std::filesystem::path> image_path;
  std::string text;
  TextStatus text_status{TextStatus::failed};
  std::string language{"en"};
};

struct Citation {
  PageId page_id;
  std::size_t start_char{0};
  std::size_t end_char{0};
  std::string quote;
};

struct ArticleDescriptor {
  std::optional<ArticleId> article_id;
  std::string title;
  std::string slug;
  std::vector<std::string> aliases;
};

struct ProposalBlock {
  BlockKind kind{BlockKind::paragraph};
  std::string text;
  std::vector<Citation> citations;
};

struct ProposalSection {
  SectionKey key{SectionKey::working_explanation};
  std::string heading;
  std::vector<ProposalBlock> blocks;
};

struct RelatedConcept {
  std::string title;
  std::string reason;
  std::vector<Citation> citations;
};

struct ArticleProposal {
  std::uint32_t schema_version{1};
  ProposalOperation operation{ProposalOperation::create_article};
  ArticleDescriptor article;
  std::vector<ProposalSection> sections;
  std::vector<RelatedConcept> related_concepts;
};

struct Article {
  ArticleId article_id;
  std::string title;
  std::string slug;
  std::vector<std::string> aliases;
  std::filesystem::path vault_path;
  std::optional<Sha256> content_sha256;
  Timestamp created_at;
  Timestamp updated_at;
};

struct ValidationIssue {
  std::string path;
  std::string message;
};

struct ValidationResult {
  std::vector<ValidationIssue> issues;

  [[nodiscard]] bool valid() const noexcept { return issues.empty(); }
  explicit operator bool() const noexcept { return valid(); }
};

[[nodiscard]] ValidationResult validate(const ProjectConfig& config);
[[nodiscard]] ValidationResult validate(const ExtractedPage& page);
[[nodiscard]] ValidationResult validate(const Citation& citation);
[[nodiscard]] ValidationResult validate(const ArticleProposal& proposal);

[[nodiscard]] std::string_view to_string(SourceKind value);
[[nodiscard]] std::string_view to_string(TextStatus value);
[[nodiscard]] std::string_view to_string(ProposalOperation value);
[[nodiscard]] std::string_view to_string(ProposalStatus value);
[[nodiscard]] std::string_view to_string(SectionKey value);
[[nodiscard]] std::string_view to_string(BlockKind value);

}  // namespace kc::domain

