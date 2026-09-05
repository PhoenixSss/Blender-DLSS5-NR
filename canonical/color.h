// SPDX-License-Identifier: MIT
// canonical/color.h — backend-independent color frame representation.
//
// This header must stay free of any NGX / D3D12 / Feature-18 specific types,
// names or conventions (requirements §7, §13, §24). Backends translate this
// canonical form into their own resource format and color semantics.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace blender_dlss5::canonical {

// §13: the encoding describes the *semantics* of the RGBA float values.
// The resource format chosen by a backend (e.g. RGBA16F) is a separate
// concern and must not be conflated with this metadata.
enum class ColorEncoding : uint8_t {
    SceneLinear = 0,
    StandardDisplay = 1,
    AgXDisplay = 2,
    ExperimentalProxy = 3,
};

inline const char* to_string(ColorEncoding e) {
    switch (e) {
        case ColorEncoding::SceneLinear: return "SceneLinear";
        case ColorEncoding::StandardDisplay: return "StandardDisplay";
        case ColorEncoding::AgXDisplay: return "AgXDisplay";
        case ColorEncoding::ExperimentalProxy: return "ExperimentalProxy";
    }
    return "Unknown";
}

inline bool from_string(const char* s, ColorEncoding* out) {
    if (!s || !out) return false;
    const std::string v(s);
    for (int i = 0; i <= 3; ++i) {
        const auto e = static_cast<ColorEncoding>(i);
        if (v == to_string(e)) { *out = e; return true; }
    }
    return false;
}

// Owning RGBA float32 frame, top-left origin, row-major, 4 channels/pixel.
// (§13 suggests a non-owning view; an owning vector keeps probe/backend
//  buffer lifetime trivial. data() provides the const float* view.)
struct CanonicalColor {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<float> rgba_f32;  // size == 4 * width * height
    ColorEncoding encoding = ColorEncoding::SceneLinear;
    bool premultiplied_alpha = false;

    const float* data() const { return rgba_f32.data(); }
    float* data() { return rgba_f32.data(); }

    // Reasonable sanity bound; matches the reference implementation's cap.
    static constexpr uint32_t kMaxDimension = 16384;

    bool valid() const {
        return width > 0 && height > 0 &&
               width <= kMaxDimension && height <= kMaxDimension &&
               rgba_f32.size() == static_cast<size_t>(width) * height * 4;
    }

    static CanonicalColor zeros(uint32_t w, uint32_t h, ColorEncoding e) {
        CanonicalColor c;
        c.width = w;
        c.height = h;
        c.rgba_f32.assign(static_cast<size_t>(w) * h * 4, 0.0f);
        c.encoding = e;
        return c;
    }
};

}  // namespace blender_dlss5::canonical
