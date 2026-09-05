// SPDX-License-Identifier: MIT
// canonical/color_transform.h — canonical color-domain conversions (§13).
//
// A3 experiment paths: the Blender layer always extracts scene-linear
// values; these transforms produce the three candidate input encodings
// for the neural backend:
//   A. SceneLinear      — identity (raw scene-linear HDR)
//   B. StandardDisplay  — sRGB EOTF (Blender "Standard" view family)
//   C. AgXDisplay       — AgX tone map + sRGB encode (true display/sRGB
//                         representation of Blender's "AgX" view)
//
// Both the probe and the Blender bridge call these shared functions, so
// cross-validation between the two stays bit-exact.
//
// AgX implementation informed by three.js (MIT) / Google Filament
// (Apache-2.0) AgX tone mapping — see THIRD_PARTY_NOTICES.md.

#pragma once

#include "color.h"

namespace blender_dlss5::canonical {

// linear -> sRGB (IEC 61966-2-1 EOTF). Input is scene-linear; negative
// inputs are clamped to 0 before the curve (display-referred output).
float SrgbEotf(float x);

// Full AgX pipeline (linear sRGB in -> display-referred sRGB out, values
// in [0,1]). Exposure = 1.0, no look modifications (the "Base" look).
void AgxTransform(float* r, float* g, float* b);

// Converts a scene-linear frame into the target encoding. Returns false
// for unsupported targets (ExperimentalProxy — D path, not implemented).
// NaN/Inf input pixels are sanitized to finite values; the output is
// display-referred [0,1] for StandardDisplay/AgXDisplay and unclamped
// scene-linear for SceneLinear.
bool ConvertEncoding(const CanonicalColor& in, ColorEncoding target,
                     CanonicalColor* out);

}  // namespace blender_dlss5::canonical
