#include "kc/domain/text.hpp"

#include <utf8proc.h>

namespace kc::domain {
bool is_valid_source_text(const std::string_view text) {
  std::size_t offset = 0;
  while (offset < text.size()) {
    utf8proc_int32_t scalar = 0;
    const auto length = utf8proc_iterate(
        reinterpret_cast<const utf8proc_uint8_t*>(text.data() + offset),
        static_cast<utf8proc_ssize_t>(text.size() - offset), &scalar);
    if (length <= 0 || scalar == 0) return false;
    offset += static_cast<std::size_t>(length);
  }
  return true;
}
}  // namespace kc::domain
