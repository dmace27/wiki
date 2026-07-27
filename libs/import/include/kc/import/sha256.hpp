#pragma once

#include <filesystem>
#include <string>

namespace kc::source_import {

/// Return the lowercase hexadecimal SHA-256 digest of a file's exact bytes.
///
/// The file is read incrementally so importing large PDFs does not require
/// loading them into memory.
[[nodiscard]] std::string sha256_file(const std::filesystem::path& path);

}  // namespace kc::source_import
