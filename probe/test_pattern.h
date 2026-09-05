// SPDX-License-Identifier: MIT
// probe/test_pattern.h — deterministic synthetic test image (no RNG).

#pragma once

#include "canonical/color.h"

namespace blender_dlss5::probe {

// Builds a deterministic 1920x1080-style test pattern with gradients, a
// zone plate (high-frequency detail), flat patches and highlights — all
// values in [0,1], alpha = 1. No randomness: identical inputs every run,
// which keeps the determinism acceptance test meaningful.
canonical::CanonicalColor BuildTestPattern(uint32_t width, uint32_t height);

}  // namespace blender_dlss5::probe
