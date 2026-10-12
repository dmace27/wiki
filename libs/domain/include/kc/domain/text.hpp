#pragma once

#include <string_view>

namespace kc::domain {
/// Text offsets are UTF-8 byte offsets. Reject malformed sequences and NULs
/// before storage, hashing, review, or model use; preserve all other bytes.
[[nodiscard]] bool is_valid_source_text(std::string_view text);
}  // namespace kc::domain
