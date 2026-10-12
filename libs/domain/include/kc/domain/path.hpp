#pragma once

#include "kc/domain/text.hpp"
#include "kc/domain/types.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <stdexcept>

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

/// Resolve existing ancestors before comparing path components. Missing leaves
/// are supported so initialization and staging use the same containment rule.
[[nodiscard]] inline bool is_within(const std::filesystem::path& root,
                                    const std::filesystem::path& candidate) {
  if (root.empty() || candidate.empty() ||
      !is_valid_source_text(path_to_utf8(root)) ||
      !is_valid_source_text(path_to_utf8(candidate))) {
    return false;
  }
  std::error_code error;
  // weakly_canonical leaves a dangling symlink in the nonexisting suffix.
  // SQLite can nevertheless follow that link and create its external target.
  // Reject dangling ancestors explicitly, including a missing state-file leaf.
  for (auto ancestor = candidate; !ancestor.empty();) {
    const auto status = std::filesystem::symlink_status(ancestor, error);
    if (std::filesystem::is_symlink(status) &&
        !std::filesystem::exists(ancestor, error)) {
      return false;
    }
    const auto parent = ancestor.parent_path();
    if (parent == ancestor) break;
    ancestor = parent;
  }
  const auto base = std::filesystem::weakly_canonical(root, error);
  if (error) return false;
  const auto resolved = std::filesystem::weakly_canonical(candidate, error);
  if (error) return false;
  const auto relative = resolved.lexically_relative(base);
  return !relative.empty() && !relative.is_absolute() &&
         std::ranges::none_of(relative, [](const auto& part) { return part == ".."; });
}

/// Reject escapes and overlapping ownership roots before opening state or
/// creating files. Cache and state may share a parent, as in the default
/// .knowledge-compiler layout.
inline void validate_project_paths(const std::filesystem::path& root,
                                   const ProjectConfig& config) {
  const auto sources = root / config.paths.sources;
  const auto vault = root / config.paths.vault;
  const auto cache = root / config.paths.cache;
  const auto state = root / config.paths.state;
  for (const auto& path : {root / "kc.json", sources, vault, cache, state,
                          vault / config.vault.source_directory}) {
    if (!is_within(root, path)) {
      throw std::invalid_argument("project path escapes its canonical root: " +
                                  path_to_utf8(path));
    }
  }
  if (!is_within(vault, vault / config.vault.source_directory)) {
    throw std::invalid_argument("vault source copies escape the vault root");
  }
  // Validate promised children too: initialization must never follow a
  // pre-existing pages/backups/staging symlink outside cache ownership.
  for (const auto* child : {"pages", "backups", "logs", "import-staging",
                            "extraction-staging"}) {
    const auto directory = cache / child;
    if (!is_within(cache, directory) || is_within(directory, state)) {
      throw std::invalid_argument("cache subdirectory escapes cache ownership or contains state");
    }
  }
  const auto overlaps = [](const auto& a, const auto& b) {
    return is_within(a, b) || is_within(b, a);
  };
  if (overlaps(sources, vault) || overlaps(sources, cache) ||
      overlaps(sources, state) || overlaps(vault, cache) ||
      overlaps(vault, state) || is_within(state, cache) ||
      overlaps(state, root / "kc.json") ||
      is_within(sources, root / "kc.json") ||
      is_within(vault, root / "kc.json") ||
      is_within(cache, root / "kc.json")) {
    throw std::invalid_argument("project paths have overlapping immutable, generated, or state ownership");
  }
}

}  // namespace kc::domain
