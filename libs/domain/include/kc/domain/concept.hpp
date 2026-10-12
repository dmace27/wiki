#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace kc::domain {

/// NFC/full case folding of arbitrary UTF-8 text, without title validation.
[[nodiscard]] std::string fold_unicode_text(std::string_view text);

/// NFC display title with horizontal Unicode whitespace trimmed/collapsed.
/// Throws std::invalid_argument for malformed UTF-8 or nonportable filenames.
[[nodiscard]] std::string normalize_concept_title(std::string_view title);

/// Locale-independent NFC/full Unicode case folding; punctuation and accents
/// remain significant. Existing titles need not already be NFC to compare.
[[nodiscard]] std::string concept_identity_key(std::string_view title);

/// Stable ASCII slug. Punctuation collisions are rejected by the state layer.
[[nodiscard]] std::string canonical_concept_slug(std::string_view title);

/// UTF-8 title converted explicitly to the platform's native filename encoding.
[[nodiscard]] std::filesystem::path concept_filename(std::string_view title);

/// Check a recorded article filename without regenerating or renaming it.
void validate_concept_filename(const std::filesystem::path& path);

/// Unicode letters/numbers/marks and named programming symbols for lexical
/// matching. Invalid UTF-8 throws; punctuation-only input returns empty.
[[nodiscard]] std::string normalize_lexical_text(std::string_view text);

}  // namespace kc::domain
