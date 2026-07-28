#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace kc::source_import {

/// Return the lowercase hexadecimal SHA-256 digest of a file's exact bytes.
///
/// The file is read incrementally so importing large PDFs does not require
/// loading them into memory.
[[nodiscard]] std::string sha256_file(const std::filesystem::path& path);

/// Return the lowercase hexadecimal SHA-256 digest of in-memory text bytes.
[[nodiscard]] std::string sha256_text(std::string_view text);

}  // namespace kc::source_import
