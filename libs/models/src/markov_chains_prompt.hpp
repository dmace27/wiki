#pragma once

#include "kc/models/language_model.hpp"
#include "kc/models/proposal_validator.hpp"

#include <string_view>

namespace kc::models::detail {

/// Increment this whenever prompt wording or its schema contract changes.
inline constexpr std::string_view markov_prompt_version =
    "markov-chains-v1";

/// Build the deterministic prompt and fully self-contained response schema.
[[nodiscard]] LanguageModelRequest build_markov_chains_prompt(
    const ProposalGenerationRequest& request);

}  // namespace kc::models::detail
