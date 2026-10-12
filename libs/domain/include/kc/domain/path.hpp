#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace kc::domain {

/// Database and JSON paths use UTF-8 regardless of the host's native encoding.
[[nodiscard]] inline std::filesystem::path path_from_utf8(
    const std::string_view text) {
  return std::filesystem::path(std::u8string(
      reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

/// Use '/' separators for portable state and CLI output; Windows native paths
/// are converted from UTF-16 by std::filesystem rather than the local code page.
[[nodiscard]] inline std::string path_to_utf8(const std::filesystem::path& path) {
  const auto text = path.generic_u8string();
  return {reinterpret_cast<const char*>(text.data()), text.size()};
}

}  // namespace kc::domain
