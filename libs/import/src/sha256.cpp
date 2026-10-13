#include "kc/import/sha256.hpp"
#include "kc/domain/sha256.hpp"

namespace kc::source_import {
// Keep the import API stable; recovery also needs the same byte fingerprint.
std::string sha256_file(const std::filesystem::path& path) {
  return domain::sha256_file(path);
}
std::string sha256_text(std::string_view text) {
  return domain::sha256_text(text);
}
}  // namespace kc::source_import
