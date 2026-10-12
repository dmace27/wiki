#pragma once

#include "kc/domain/concept.hpp"

namespace kc::models {

// Preserve the model-layer API while sharing the identity rules with every
// stage, including services that must not depend on the model library.
using domain::canonical_concept_slug;

}  // namespace kc::models
