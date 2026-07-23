#include "kc/domain/json.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace kc::domain {
namespace {

using Json = nlohmann::json;

void require_object(const Json& document, const std::string_view path) {
  if (!document.is_object()) {
    throw std::invalid_argument(std::string(path) + " must be an object");
  }
}

void require_keys(const Json& document,
                  const std::initializer_list<std::string_view> required,
                  const std::initializer_list<std::string_view> optional,
                  const std::string_view path) {
  require_object(document, path);
  for (const auto key : required) {
    if (!document.contains(key)) {
      throw std::invalid_argument(std::string(path) + "/" + std::string(key) + " is required");
    }
  }
  for (const auto& [key, unused] : document.items()) {
    static_cast<void>(unused);
    const auto allowed = std::ranges::find(required, key) != required.end() ||
                         std::ranges::find(optional, key) != optional.end();
    if (!allowed) {
      throw std::invalid_argument(std::string(path) + "/" + key + " is not allowed");
    }
  }
}

template <typename Enum>
Enum parse_enum(const Json& document,
                const std::initializer_list<std::pair<std::string_view, Enum>> choices,
                const std::string_view path) {
  if (!document.is_string()) {
    throw std::invalid_argument(std::string(path) + " must be a string");
  }
  const auto value = document.get<std::string>();
  const auto found = std::ranges::find_if(choices, [&](const auto& choice) { return choice.first == value; });
  if (found == choices.end()) {
    throw std::invalid_argument(std::string(path) + " has an unsupported value");
  }
  return found->second;
}

std::filesystem::path parse_path(const Json& document, const std::string_view path) {
  if (!document.is_string()) {
    throw std::invalid_argument(std::string(path) + " must be a string");
  }
  return std::filesystem::path(document.get<std::string>());
}

Citation parse_citation_value(const Json& document, const std::string_view path) {
  require_keys(document, {"page_id", "start_char", "end_char", "quote"}, {}, path);
  Citation value;
  value.page_id = {document.at("page_id").get<std::string>()};
  value.start_char = document.at("start_char").get<std::size_t>();
  value.end_char = document.at("end_char").get<std::size_t>();
  value.quote = document.at("quote").get<std::string>();
  return value;
}

template <typename T, typename Parser>
ParseResult<T> parse_document(const Json& document, Parser&& parser) {
  ParseResult<T> result;
  try {
    T value = std::forward<Parser>(parser)(document);
    const auto validation = validate(value);
    if (!validation) {
      result.issues = validation.issues;
      return result;
    }
    result.value = std::move(value);
  } catch (const std::exception& error) {
    result.issues.push_back({"/", error.what()});
  }
  return result;
}

Json citation_json(const Citation& value) {
  return Json{{"page_id", value.page_id.value},
              {"start_char", value.start_char},
              {"end_char", value.end_char},
              {"quote", value.quote}};
}

}  // namespace

void from_json(const Json& document, ProjectConfig& value) {
  require_keys(document, {"schema_version", "project_id", "paths", "vault", "providers"}, {}, "");
  value.schema_version = document.at("schema_version").get<std::uint32_t>();
  value.project_id = {document.at("project_id").get<std::string>()};

  const auto& paths = document.at("paths");
  require_keys(paths, {"sources", "vault", "state", "cache"}, {}, "/paths");
  value.paths.sources = parse_path(paths.at("sources"), "/paths/sources");
  value.paths.vault = parse_path(paths.at("vault"), "/paths/vault");
  value.paths.state = parse_path(paths.at("state"), "/paths/state");
  value.paths.cache = parse_path(paths.at("cache"), "/paths/cache");

  const auto& vault = document.at("vault");
  require_keys(vault, {"source_directory", "generated_section_id"}, {}, "/vault");
  value.vault.source_directory = parse_path(vault.at("source_directory"), "/vault/source_directory");
  value.vault.generated_section_id = vault.at("generated_section_id").get<std::string>();

  const auto& providers = document.at("providers");
  require_keys(providers, {"llm", "ocr"}, {}, "/providers");
  const auto& llm = providers.at("llm");
  require_keys(llm, {"default", "ollama"}, {}, "/providers/llm");
  value.providers.llm.default_provider = llm.at("default").get<std::string>();
  const auto& ollama = llm.at("ollama");
  require_keys(ollama, {"base_url", "model", "timeout_seconds"}, {}, "/providers/llm/ollama");
  value.providers.llm.ollama.base_url = ollama.at("base_url").get<std::string>();
  value.providers.llm.ollama.model = ollama.at("model").get<std::string>();
  value.providers.llm.ollama.timeout_seconds = ollama.at("timeout_seconds").get<std::uint32_t>();

  const auto& ocr = providers.at("ocr");
  require_keys(ocr, {"default", "language"}, {}, "/providers/ocr");
  value.providers.ocr.default_provider = ocr.at("default").get<std::string>();
  value.providers.ocr.language = ocr.at("language").get<std::string>();
}

void to_json(Json& document, const ProjectConfig& value) {
  document = Json{
      {"schema_version", value.schema_version},
      {"project_id", value.project_id.value},
      {"paths",
       {{"sources", value.paths.sources.generic_string()},
        {"vault", value.paths.vault.generic_string()},
        {"state", value.paths.state.generic_string()},
        {"cache", value.paths.cache.generic_string()}}},
      {"vault",
       {{"source_directory", value.vault.source_directory.generic_string()},
        {"generated_section_id", value.vault.generated_section_id}}},
      {"providers",
       {{"llm",
         {{"default", value.providers.llm.default_provider},
          {"ollama",
           {{"base_url", value.providers.llm.ollama.base_url},
            {"model", value.providers.llm.ollama.model},
            {"timeout_seconds", value.providers.llm.ollama.timeout_seconds}}}}},
        {"ocr",
         {{"default", value.providers.ocr.default_provider},
          {"language", value.providers.ocr.language}}}}}};
}

void from_json(const Json& document, ExtractedPage& value) {
  require_keys(document,
               {"page_id", "source_version_id", "page_number", "text", "text_status"},
               {"image_path", "language"}, "");
  value.page_id = {document.at("page_id").get<std::string>()};
  value.source_version_id = {document.at("source_version_id").get<std::string>()};
  value.page_number = document.at("page_number").get<std::uint32_t>();
  if (document.contains("image_path")) {
    value.image_path = parse_path(document.at("image_path"), "/image_path");
  } else {
    value.image_path.reset();
  }
  value.text = document.at("text").get<std::string>();
  value.text_status = parse_enum<TextStatus>(
      document.at("text_status"),
      {{"native", TextStatus::native},
       {"ocr_unreviewed", TextStatus::ocr_unreviewed},
       {"reviewed", TextStatus::reviewed},
       {"failed", TextStatus::failed}},
      "/text_status");
  value.language = document.value("language", "en");
}

void to_json(Json& document, const ExtractedPage& value) {
  document = Json{{"page_id", value.page_id.value},
                  {"source_version_id", value.source_version_id.value},
                  {"page_number", value.page_number},
                  {"text", value.text},
                  {"text_status", to_string(value.text_status)},
                  {"language", value.language}};
  if (value.image_path) {
    document["image_path"] = value.image_path->generic_string();
  }
}

void from_json(const Json& document, Citation& value) { value = parse_citation_value(document, ""); }

void to_json(Json& document, const Citation& value) { document = citation_json(value); }

void from_json(const Json& document, ArticleProposal& value) {
  require_keys(document,
               {"schema_version", "operation", "article", "sections", "related_concepts"}, {}, "");
  value.schema_version = document.at("schema_version").get<std::uint32_t>();
  value.operation = parse_enum<ProposalOperation>(
      document.at("operation"),
      {{"create_article", ProposalOperation::create_article},
       {"update_article", ProposalOperation::update_article}},
      "/operation");

  const auto& article = document.at("article");
  require_keys(article, {"title", "slug"}, {"article_id", "aliases"}, "/article");
  if (article.contains("article_id")) {
    value.article.article_id = ArticleId{article.at("article_id").get<std::string>()};
  } else {
    value.article.article_id.reset();
  }
  value.article.title = article.at("title").get<std::string>();
  value.article.slug = article.at("slug").get<std::string>();
  value.article.aliases = article.value("aliases", std::vector<std::string>{});

  const auto& sections = document.at("sections");
  if (!sections.is_array()) {
    throw std::invalid_argument("/sections must be an array");
  }
  value.sections.clear();
  for (std::size_t section_index = 0; section_index < sections.size(); ++section_index) {
    const auto path = "/sections/" + std::to_string(section_index);
    const auto& section_json = sections.at(section_index);
    require_keys(section_json, {"key", "heading", "blocks"}, {}, path);
    ProposalSection section;
    section.key = parse_enum<SectionKey>(
        section_json.at("key"),
        {{"working_explanation", SectionKey::working_explanation},
         {"key_ideas", SectionKey::key_ideas},
         {"example", SectionKey::example},
         {"related_concepts", SectionKey::related_concepts}},
        path + "/key");
    section.heading = section_json.at("heading").get<std::string>();
    const auto& blocks = section_json.at("blocks");
    if (!blocks.is_array()) {
      throw std::invalid_argument(path + "/blocks must be an array");
    }
    for (std::size_t block_index = 0; block_index < blocks.size(); ++block_index) {
      const auto block_path = path + "/blocks/" + std::to_string(block_index);
      const auto& block_json = blocks.at(block_index);
      require_keys(block_json, {"kind", "text", "citations"}, {}, block_path);
      ProposalBlock block;
      block.kind = parse_enum<BlockKind>(
          block_json.at("kind"),
          {{"paragraph", BlockKind::paragraph}, {"bullet", BlockKind::bullet}},
          block_path + "/kind");
      block.text = block_json.at("text").get<std::string>();
      const auto& citations = block_json.at("citations");
      if (!citations.is_array()) {
        throw std::invalid_argument(block_path + "/citations must be an array");
      }
      for (std::size_t citation_index = 0; citation_index < citations.size(); ++citation_index) {
        block.citations.push_back(parse_citation_value(
            citations.at(citation_index), block_path + "/citations/" + std::to_string(citation_index)));
      }
      section.blocks.push_back(std::move(block));
    }
    value.sections.push_back(std::move(section));
  }

  const auto& concepts = document.at("related_concepts");
  if (!concepts.is_array()) {
    throw std::invalid_argument("/related_concepts must be an array");
  }
  value.related_concepts.clear();
  for (std::size_t concept_index = 0; concept_index < concepts.size(); ++concept_index) {
    const auto path = "/related_concepts/" + std::to_string(concept_index);
    const auto& concept_json = concepts.at(concept_index);
    require_keys(concept_json, {"title", "reason", "citations"}, {}, path);
    RelatedConcept related;
    related.title = concept_json.at("title").get<std::string>();
    related.reason = concept_json.at("reason").get<std::string>();
    const auto& citations = concept_json.at("citations");
    if (!citations.is_array()) {
      throw std::invalid_argument(path + "/citations must be an array");
    }
    for (std::size_t citation_index = 0; citation_index < citations.size(); ++citation_index) {
      related.citations.push_back(parse_citation_value(
          citations.at(citation_index), path + "/citations/" + std::to_string(citation_index)));
    }
    value.related_concepts.push_back(std::move(related));
  }
}

void to_json(Json& document, const ArticleProposal& value) {
  Json article{{"title", value.article.title}, {"slug", value.article.slug}, {"aliases", value.article.aliases}};
  if (value.article.article_id) {
    article["article_id"] = value.article.article_id->value;
  }

  Json sections = Json::array();
  for (const auto& section : value.sections) {
    Json blocks = Json::array();
    for (const auto& block : section.blocks) {
      Json citations = Json::array();
      for (const auto& citation : block.citations) {
        citations.push_back(citation_json(citation));
      }
      blocks.push_back({{"kind", to_string(block.kind)},
                        {"text", block.text},
                        {"citations", std::move(citations)}});
    }
    sections.push_back({{"key", to_string(section.key)},
                        {"heading", section.heading},
                        {"blocks", std::move(blocks)}});
  }

  Json concepts = Json::array();
  for (const auto& related : value.related_concepts) {
    Json citations = Json::array();
    for (const auto& citation : related.citations) {
      citations.push_back(citation_json(citation));
    }
    concepts.push_back({{"title", related.title},
                        {"reason", related.reason},
                        {"citations", std::move(citations)}});
  }

  document = Json{{"schema_version", value.schema_version},
                  {"operation", to_string(value.operation)},
                  {"article", std::move(article)},
                  {"sections", std::move(sections)},
                  {"related_concepts", std::move(concepts)}};
}

ParseResult<ProjectConfig> parse_project_config(const Json& document) {
  return parse_document<ProjectConfig>(document, [](const Json& value) { return value.get<ProjectConfig>(); });
}

ParseResult<ExtractedPage> parse_extracted_page(const Json& document) {
  return parse_document<ExtractedPage>(document, [](const Json& value) { return value.get<ExtractedPage>(); });
}

ParseResult<Citation> parse_citation(const Json& document) {
  return parse_document<Citation>(document, [](const Json& value) { return value.get<Citation>(); });
}

ParseResult<ArticleProposal> parse_article_proposal(const Json& document) {
  return parse_document<ArticleProposal>(document, [](const Json& value) { return value.get<ArticleProposal>(); });
}

}  // namespace kc::domain
