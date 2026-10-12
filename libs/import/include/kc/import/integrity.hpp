#pragma once

#include "kc/domain/path.hpp"
#include "kc/domain/text.hpp"
#include "kc/import/sha256.hpp"

#include <stdexcept>

namespace kc::source_import {
/// Validate physical evidence against its owning directory and recorded hash.
/// Legacy images without a digest require re-extraction, never silent trust.
inline void verify_retained_file(const std::filesystem::path& owner,
                                 const std::filesystem::path& file,
                                 const std::string_view digest) {
  if (!domain::is_within(owner, file)) {
    throw std::invalid_argument("retained evidence path escapes its storage root");
  }
  if (!std::filesystem::is_regular_file(file)) {
    throw std::invalid_argument("retained evidence file is missing or unreadable");
  }
  if (digest.empty() || sha256_file(file) != digest) {
    throw std::invalid_argument("retained evidence does not match its recorded SHA-256; restore it or re-extract uncited pages");
  }
}
}  // namespace kc::source_import
