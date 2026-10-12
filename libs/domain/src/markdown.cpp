#include "kc/domain/markdown.hpp"

#include "kc/domain/concept.hpp"

#include <utf8proc.h>

#include <algorithm>
#include <stdexcept>

namespace kc::domain {
namespace {

/// Fold prose without letting continuation lines become Markdown structures.
std::string inline_text(const std::string_view text) {
  std::string output;
  bool pending_space = false;
  for (std::size_t offset = 0; offset < text.size();) {
    utf8proc_int32_t scalar = 0;
    const auto length = utf8proc_iterate(
        reinterpret_cast<const utf8proc_uint8_t*>(text.data() + offset),
        static_cast<utf8proc_ssize_t>(text.size() - offset), &scalar);
    if (length <= 0) {
      throw std::invalid_argument("Markdown text must be valid UTF-8");
    }
    const auto category = utf8proc_category(scalar);
    const bool whitespace = scalar == '\n' || scalar == '\r' || scalar == '\t' ||
        category == UTF8PROC_CATEGORY_ZS || category == UTF8PROC_CATEGORY_ZL ||
        category == UTF8PROC_CATEGORY_ZP;
    if (whitespace) {
      pending_space = !output.empty();
    } else {
      if (category == UTF8PROC_CATEGORY_CC || category == UTF8PROC_CATEGORY_CF) {
        throw std::invalid_argument("Markdown text must not contain control or format characters");
      }
      if (pending_space) {
        output.push_back(' ');
        pending_space = false;
      }
      output.append(text.substr(offset, static_cast<std::size_t>(length)));
    }
    offset += static_cast<std::size_t>(length);
  }
  return output;
}

void check_text(ValidationResult& result, const std::string& path,
                const std::string_view text, const bool single_line,
                const bool allow_empty = false) {
  try {
    const auto flattened = inline_text(text);
    if (!allow_empty && flattened.empty()) {
      result.issues.push_back({path, "must contain visible text"});
    }
    if (single_line && text.find_first_of("\r\n\t") != std::string_view::npos) {
      result.issues.push_back({path, "must be a single line"});
    }
    if (single_line && (text.find("\u2028") != std::string_view::npos ||
                        text.find("\u2029") != std::string_view::npos)) {
      result.issues.push_back({path, "must be a single line"});
    }
    if (text.find("kc:managed:") != std::string_view::npos) {
      result.issues.push_back({path, "must not contain a reserved ownership marker"});
    }
  } catch (const std::invalid_argument& error) {
    result.issues.push_back({path, error.what()});
  }
}

void check_citations(ValidationResult& result, const std::string& path,
                     const std::vector<Citation>& citations) {
  for (std::size_t index = 0; index < citations.size(); ++index) {
    check_text(result, path + "/" + std::to_string(index) + "/quote",
               citations[index].quote, false);
  }
}

}  // namespace

ValidationResult validate_plain_text(const std::string_view text,
                                     const bool single_line,
                                     const bool allow_empty) {
  ValidationResult result;
  check_text(result, "", text, single_line, allow_empty);
  return result;
}

ValidationResult validate_markdown(const ArticleProposal& proposal) {
  ValidationResult result;
  check_text(result, "/article/title", proposal.article.title, true);
  for (std::size_t index = 0; index < proposal.article.aliases.size(); ++index) {
    check_text(result, "/article/aliases/" + std::to_string(index),
               proposal.article.aliases[index], true);
  }
  for (std::size_t index = 0; index < proposal.sections.size(); ++index) {
    const auto& section = proposal.sections[index];
    const auto path = "/sections/" + std::to_string(index);
    check_text(result, path + "/heading", section.heading, true);
    for (std::size_t block_index = 0; block_index < section.blocks.size(); ++block_index) {
      const auto& block = section.blocks[block_index];
      const auto block_path = path + "/blocks/" + std::to_string(block_index);
      check_text(result, block_path + "/text", block.text, false);
      check_citations(result, block_path + "/citations", block.citations);
    }
  }
  for (std::size_t index = 0; index < proposal.related_concepts.size(); ++index) {
    const auto& related = proposal.related_concepts[index];
    const auto path = "/related_concepts/" + std::to_string(index);
    check_text(result, path + "/title", related.title, true);
    check_text(result, path + "/reason", related.reason, false, true);
    // Wiki-link destinations cannot safely express these separators. Refuse
    // them before review instead of silently linking a different page/heading.
    if (related.title.find_first_of("[]#|^") != std::string::npos) {
      result.issues.push_back({path + "/title", "must be an unambiguous wiki-link destination (no []#|^)"});
    }
    try {
      static_cast<void>(concept_filename(related.title));
    } catch (const std::invalid_argument& error) {
      result.issues.push_back({path + "/title", error.what()});
    }
    check_citations(result, path + "/citations", related.citations);
  }
  return result;
}

std::string markdown_literal(const std::string_view text) {
  const auto flattened = inline_text(text);
  std::string output;
  for (const auto character : flattened) {
    // CommonMark permits backslash escaping every ASCII punctuation byte.
    // Escaping all of them also disables Obsidian's extended inline syntax.
    if ((character >= '!' && character <= '/') ||
        (character >= ':' && character <= '@') ||
        (character >= '[' && character <= '`') ||
        (character >= '{' && character <= '~')) {
      output.push_back('\\');
    }
    output.push_back(character);
  }
  return output;
}

bool safe_managed_section_id(const std::string_view id) {
  return !id.empty() && id.size() <= 128U &&
      std::ranges::all_of(id, [](const char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') ||
               character == '-' || character == '_' || character == '.';
      });
}

}  // namespace kc::domain
