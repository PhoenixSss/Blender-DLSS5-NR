// SPDX-License-Identifier: MIT
// probe/png_io.h — stb-backed PNG load/save for the probe (convenience
// I/O only; the raw float32 bin file is the ground truth).

#pragma once

#include <string>

#include "canonical/color.h"

namespace blender_dlss5::probe {

// Loads a PNG (8/16-bit) into a float RGBA frame. Returns false and fills
// `error` on failure. No color management is applied — the PNG values are
// taken as-is (A1 documents this clearly).
bool LoadPng(const std::string& path, canonical::CanonicalColor* out,
             std::string* error);

// Writes a float RGBA frame as 8-bit PNG (values clamped to [0,1]).
bool SavePng(const std::string& path, const canonical::CanonicalColor& frame,
             std::string* error);

}  // namespace blender_dlss5::probe
