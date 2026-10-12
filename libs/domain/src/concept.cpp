#include "kc/domain/concept.hpp"
#include "kc/domain/path.hpp"

#include <utf8proc.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <stdexcept>

namespace kc::domain {
namespace {

/// utf8proc_map uses explicit lengths so embedded NULs cannot truncate input.
std::string unicode_normalize(const std::string_view text, const bool fold) {
  utf8proc_uint8_t* output = nullptr;
  const auto options = static_cast<utf8proc_option_t>(
      UTF8PROC_STABLE | UTF8PROC_COMPOSE | (fold ? UTF8PROC_CASEFOLD : 0));
  const auto length = utf8proc_map(
      reinterpret_cast<const utf8proc_uint8_t*>(text.data()),
      static_cast<utf8proc_ssize_t>(text.size()), &output, options);
  const std::unique_ptr<utf8proc_uint8_t, decltype(&std::free)> owned(output,
                                                                  &std::free);
  if (length < 0) {
    throw std::invalid_argument("concept text must be valid UTF-8");
  }
  return {reinterpret_cast<const char*>(output),
          static_cast<std::size_t>(length)};
}

/// Decode one scalar while retaining the original bytes for output.
template <typename Visitor>
void visit_unicode(const std::string_view text, Visitor visitor) {
  for (std::size_t offset = 0; offset < text.size();) {
    utf8proc_int32_t scalar = 0;
    const auto length = utf8proc_iterate(
        reinterpret_cast<const utf8proc_uint8_t*>(text.data() + offset),
        static_cast<utf8proc_ssize_t>(text.size() - offset), &scalar);
    if (length <= 0) {
      throw std::invalid_argument("concept text must be valid UTF-8");
    }
    const auto bytes = static_cast<std::size_t>(length);
    visitor(scalar, text.substr(offset, bytes));
    offset += bytes;
  }
}

std::string display_title(const std::string_view title) {
  const auto normalized = unicode_normalize(title, false);
  std::string output;
  bool space = false;
  visit_unicode(normalized, [&](const auto scalar, const auto bytes) {
    const auto category = utf8proc_category(scalar);
    if (category == UTF8PROC_CATEGORY_ZS || scalar == '\t') {
      space = !output.empty();
      return;
    }
    if (category == UTF8PROC_CATEGORY_CC || category == UTF8PROC_CATEGORY_CF ||
        category == UTF8PROC_CATEGORY_ZL || category == UTF8PROC_CATEGORY_ZP) {
      throw std::invalid_argument(
          "concept title must not contain control, format, or line-break characters");
    }
    if (space) {
      output.push_back(' ');
      space = false;
    }
    output.append(bytes);
  });
  return output;
}

bool reserved_windows_name(const std::string_view title) {
  // Windows reserves device names even when followed by an extension. It also
  // recognizes superscript 1/2/3 as COM/LPT device numbers.
  auto stem = unicode_normalize(title.substr(0, title.find('.')), true);
  while (!stem.empty() && stem.back() == ' ') {
    stem.pop_back();
  }
  if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul" ||
      stem == "conin$" || stem == "conout$") {
    return true;
  }
  if (!stem.starts_with("com") && !stem.starts_with("lpt")) {
    return false;
  }
  const auto suffix = std::string_view(stem).substr(3);
  return (suffix.size() == 1U && suffix.front() >= '1' && suffix.front() <= '9') ||
         suffix == "¹" || suffix == "²" || suffix == "³";
}

}  // namespace

std::string fold_unicode_text(const std::string_view text) {
  return unicode_normalize(text, true);
}

std::string normalize_concept_title(const std::string_view title) {
  auto normalized = display_title(title);
  if (normalized.empty() || normalized.size() > 200U) {
    throw std::invalid_argument("concept title must be 1-200 UTF-8 bytes after normalization");
  }
  if (normalized.find_first_of("/\\<>:\"|?*") != std::string::npos ||
      normalized.back() == '.' || reserved_windows_name(normalized)) {
    throw std::invalid_argument(
        "concept title must be a portable filename: no reserved device names, "
        "trailing dots, or /\\<>:\"|?* characters");
  }
  return normalized;
}

std::string concept_identity_key(const std::string_view title) {
  return unicode_normalize(display_title(title), true);
}

std::string canonical_concept_slug(const std::string_view title) {
  const auto key = concept_identity_key(title);
  std::string slug;
  const auto separator = [&]() {
    if (!slug.empty() && slug.back() != '-') {
      slug.push_back('-');
    }
  };
  for (const auto character : key) {
    if ((character >= 'a' && character <= 'z') ||
        (character >= '0' && character <= '9')) {
      slug.push_back(character);
    } else if (character == '\'') {
      continue;
    } else {
      separator();
      switch (character) {
        case '+': slug += "plus"; break;
        case '#': slug += "sharp"; break;
        case '&': slug += "and"; break;
        case '@': slug += "at"; break;
        default: break;
      }
    }
  }
  while (!slug.empty() && slug.back() == '-') {
    slug.pop_back();
  }
  if (!slug.empty()) {
    return slug;
  }
  // Hash the identity key, so equivalent Unicode/case requests share a slug.
  std::uint64_t hash = 14695981039346656037ULL;
  for (const auto character : key) {
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

std::filesystem::path concept_filename(const std::string_view title) {
  // Validate the actual bytes as well as the normalized title: stored legacy
  // filenames with trailing spaces/dots cannot be made portable by trimming.
  static_cast<void>(normalize_concept_title(title));
  visit_unicode(title, [](const auto scalar, const auto) {
    if (utf8proc_category(scalar) == UTF8PROC_CATEGORY_CC) {
      throw std::invalid_argument("stored concept filename contains a control character");
    }
  });
  if (title.empty() || title.back() == ' ' || title.back() == '.') {
    throw std::invalid_argument("stored concept filename has trailing spaces or dots");
  }
  const auto filename = std::string(title) + ".md";
  return path_from_utf8(filename);
}

void validate_concept_filename(const std::filesystem::path& path) {
  const auto name = path_to_utf8(path.filename());
  if (name.empty() || name.size() > 255U || name.back() == ' ' ||
      name.back() == '.' || name.find_first_of("/\\<>:\"|?*") != std::string::npos ||
      reserved_windows_name(name)) {
    throw std::invalid_argument("stored article filename is not portable");
  }
  visit_unicode(name, [](const auto scalar, const auto) {
    const auto category = utf8proc_category(scalar);
    if (category == UTF8PROC_CATEGORY_CC || category == UTF8PROC_CATEGORY_CF ||
        category == UTF8PROC_CATEGORY_ZL || category == UTF8PROC_CATEGORY_ZP) {
      throw std::invalid_argument("stored article filename contains control/format characters");
    }
  });
}

std::string normalize_lexical_text(const std::string_view text) {
  const auto folded = unicode_normalize(text, true);
  std::string output;
  bool space = false;
  visit_unicode(folded, [&](const auto scalar, const auto bytes) {
    const auto category = utf8proc_category(scalar);
    if ((category >= UTF8PROC_CATEGORY_LU && category <= UTF8PROC_CATEGORY_NO)) {
      if (space && !output.empty()) {
        output.push_back(' ');
      }
      output.append(bytes);
      space = false;
      return;
    }
    std::string_view symbol;
    switch (scalar) {
      case '+': symbol = "plus"; break;
      case '#': symbol = "sharp"; break;
      case '&': symbol = "and"; break;
      case '@': symbol = "at"; break;
      default: break;
    }
    if (!symbol.empty()) {
      if (!output.empty()) {
        output.push_back(' ');
      }
      output.append(symbol);
    }
    space = !output.empty();
  });
  return output;
}

}  // namespace kc::domain
