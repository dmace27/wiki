#include "kc/models/proposal_validator.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace kc::models {
namespace {

void add_issue(domain::ValidationResult& result, std::string path,
               std::string message) {
  result.issues.push_back({std::move(path), std::move(message)});
}

void append_issues(domain::ValidationResult& target,
                   domain::ValidationResult source,
                   const std::string_view prefix) {
  for (auto& issue : source.issues) {
    issue.path = std::string(prefix) + issue.path;
    target.issues.push_back(std::move(issue));
  }
}

/// Collapse ASCII whitespace and trim both ends for citation comparison.
std::string normalize_whitespace(const std::string_view text) {
  std::string normalized;
  normalized.reserve(text.size());
  bool pending_space = false;
  for (const auto character : text) {
    if (std::isspace(static_cast<unsigned char>(character))) {
      pending_space = !normalized.empty();
      continue;
    }
    if (pending_space) {
      normalized.push_back(' ');
      pending_space = false;
    }
    normalized.push_back(character);
  }
  return normalized;
}

using PageIndex =
    std::unordered_map<std::string, const domain::ExtractedPage*>;

void validate_citation(const domain::Citation& citation,
                       const std::string& path, const PageIndex& pages,
                       domain::ValidationResult& result) {
  const auto found = pages.find(citation.page_id.value);
  if (found == pages.end()) {
    add_issue(result, path + "/page_id",
              "must reference a page supplied to the model");
    return;
  }

  const auto& page_text = found->second->text;
  if (citation.start_char >= citation.end_char ||
      citation.end_char > page_text.size()) {
    add_issue(result, path,
              "citation offsets must be within the referenced page text");
    return;
  }

  const auto cited_text = page_text.substr(
      citation.start_char, citation.end_char - citation.start_char);
  if (normalize_whitespace(cited_text) !=
      normalize_whitespace(citation.quote)) {
    add_issue(result, path + "/quote",
              "must match the normalized referenced page substring");
  }
}

}  // namespace

domain::ValidationResult ProposalValidator::validate_request(
    const ProposalGenerationRequest& request) const {
  domain::ValidationResult result;
  if (request.concept_title != "Markov Chains") {
    add_issue(result, "/concept",
              "MVP prompt supports exactly 'Markov Chains'");
  }
  if (request.pages.empty()) {
    add_issue(result, "/pages", "must contain at least one evidence page");
  }

  if (request.operation == domain::ProposalOperation::create_article &&
      request.article_id) {
    add_issue(result, "/article_id",
              "must be absent for a create_article request");
  }
  if (request.operation == domain::ProposalOperation::update_article &&
      !request.article_id) {
    add_issue(result, "/article_id",
              "is required for an update_article request");
  }
  if (request.article_id &&
      !std::regex_match(
          request.article_id->value,
          std::regex("^art_[0-9A-HJKMNP-TV-Z]{26}$"))) {
    add_issue(result, "/article_id", "must be an art_ ULID");
  }

  std::unordered_set<std::string> page_ids;
  for (std::size_t index = 0; index < request.pages.size(); ++index) {
    const auto path = "/pages/" + std::to_string(index);
    append_issues(result, domain::validate(request.pages[index]), path);
    if (!page_ids.insert(request.pages[index].page_id.value).second) {
      add_issue(result, path + "/page_id",
                "must be unique within model evidence");
    }
  }
  return result;
}

domain::ValidationResult ProposalValidator::validate_response(
    const domain::ArticleProposal& proposal,
    const ProposalGenerationRequest& request) const {
  auto result = domain::validate(proposal);

  if (proposal.operation != request.operation) {
    add_issue(result, "/operation", "must match the requested operation");
  }
  if (proposal.article.title != "Markov Chains") {
    add_issue(result, "/article/title", "must equal 'Markov Chains'");
  }
  if (proposal.article.slug != "markov-chains") {
    add_issue(result, "/article/slug", "must equal 'markov-chains'");
  }

  if (request.operation == domain::ProposalOperation::create_article &&
      proposal.article.article_id) {
    add_issue(result, "/article/article_id",
              "must be absent when creating an article");
  }
  if (request.operation == domain::ProposalOperation::update_article &&
      (!proposal.article.article_id || !request.article_id ||
       *proposal.article.article_id != *request.article_id)) {
    add_issue(result, "/article/article_id",
              "must match the requested article ID");
  }

  PageIndex pages;
  for (const auto& page : request.pages) {
    pages.emplace(page.page_id.value, &page);
  }

  for (std::size_t section_index = 0;
       section_index < proposal.sections.size(); ++section_index) {
    const auto& section = proposal.sections[section_index];
    for (std::size_t block_index = 0;
         block_index < section.blocks.size(); ++block_index) {
      const auto& block = section.blocks[block_index];
      for (std::size_t citation_index = 0;
           citation_index < block.citations.size(); ++citation_index) {
        validate_citation(
            block.citations[citation_index],
            "/sections/" + std::to_string(section_index) + "/blocks/" +
                std::to_string(block_index) + "/citations/" +
                std::to_string(citation_index),
            pages, result);
      }
    }
  }

  for (std::size_t concept_index = 0;
       concept_index < proposal.related_concepts.size(); ++concept_index) {
    const auto& related = proposal.related_concepts[concept_index];
    for (std::size_t citation_index = 0;
         citation_index < related.citations.size(); ++citation_index) {
      validate_citation(
          related.citations[citation_index],
          "/related_concepts/" + std::to_string(concept_index) +
              "/citations/" + std::to_string(citation_index),
          pages, result);
    }
  }
  return result;
}

}  // namespace kc::models
