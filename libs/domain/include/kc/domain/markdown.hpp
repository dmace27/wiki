#pragma once

#include "kc/domain/types.hpp"

#include <string>
#include <string_view>

namespace kc::domain {

/// Shared visible UTF-8 text/ownership-marker check. Issue paths are relative
/// to the text itself so callers can prepend their actual contract field.
[[nodiscard]] ValidationResult validate_plain_text(
    std::string_view text, bool single_line, bool allow_empty = false);

/// Validate text that the compiler will place into Markdown. Used by parsing,
/// generation, review, approval, and apply rather than deferred until writing.
[[nodiscard]] ValidationResult validate_markdown(const ArticleProposal& proposal);

/// Render untrusted UTF-8 as literal inline Markdown. Line breaks and horizontal
/// whitespace are folded to spaces; punctuation cannot introduce links, HTML,
/// embeds, footnotes, lists, or fenced blocks. Throws for unsafe Unicode text.
[[nodiscard]] std::string markdown_literal(std::string_view text);

/// Section IDs are inserted in an HTML comment attribute, never as model text.
[[nodiscard]] bool safe_managed_section_id(std::string_view id);

}  // namespace kc::domain
