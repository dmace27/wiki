#pragma once

#include "kc/domain/types.hpp"

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <limits>

namespace kc::domain {

enum class JsonContract { project_config, extracted_page, citation, article_proposal };

/// Portable curl timeout and signed SQLite citation storage boundaries. These
/// limits are also specified in the authoritative checked-in JSON schemas.
inline constexpr std::uint32_t max_timeout_seconds = 2147483647U;
inline constexpr std::uint64_t max_citation_end = 9223372036854775807ULL;
inline constexpr std::uint64_t max_citation_start = max_citation_end - 1U;

/// Schemas compiled from schemas/*.json; no source-tree access is needed at
/// runtime. A proposal schema embeds its citation reference for model servers.
[[nodiscard]] const nlohmann::json& contract_schema(JsonContract contract);

/// Check primitive types, bounds, shapes, required keys, and unknown properties
/// before conversion. Cross-field/domain/evidence checks run on typed values.
[[nodiscard]] ValidationResult validate_json_shape(
    JsonContract contract, const nlohmann::json& document);

}  // namespace kc::domain
