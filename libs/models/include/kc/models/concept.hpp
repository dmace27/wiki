#pragma once

#include <array>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

namespace kc::models {

/// Produce the stable vault/database slug required for a requested concept.
///
/// Common programming symbols are named explicitly. Titles with no ASCII
/// letters or digits receive a deterministic hash slug so non-Latin concepts
/// remain supported without relying on locale-specific transliteration.
[[nodiscard]] inline std::string canonical_concept_slug(
    const std::string_view title) {
  std::string slug;
  const auto append_separator = [&slug]() {
    if (!slug.empty() && slug.back() != '-') {
      slug.push_back('-');
    }
  };
  const auto append_word = [&slug, &append_separator](
                               const std::string_view word) {
    append_separator();
    slug.append(word);
  };

  for (const char character : title) {
    const auto byte = static_cast<unsigned char>(character);
    if (std::isalnum(byte)) {
      slug.push_back(static_cast<char>(std::tolower(byte)));
      continue;
    }
    switch (character) {
      case '+':
        append_word("plus");
        break;
      case '#':
        append_word("sharp");
        break;
      case '&':
        append_word("and");
        break;
      case '@':
        append_word("at");
        break;
      case '\'':
        // Apostrophes do not create awkward slugs such as "bayes-s-rule".
        break;
      default:
        append_separator();
        break;
    }
  }
  while (!slug.empty() && slug.back() == '-') {
    slug.pop_back();
  }
  if (!slug.empty()) {
    return slug;
  }

  // FNV-1a gives opaque but stable slugs when transliteration is unavailable.
  std::uint64_t hash = 14695981039346656037ULL;
  for (const char character : title) {
    hash ^= static_cast<unsigned char>(character);
    hash *= 1099511628211ULL;
  }
  constexpr std::array<char, 16> hex{
      '0', '1', '2', '3', '4', '5', '6', '7',
      '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  std::string fallback = "concept-";
  for (int shift = 60; shift >= 0; shift -= 4) {
    fallback.push_back(hex[(hash >> shift) & 0xFU]);
  }
  return fallback;
}

}  // namespace kc::models
