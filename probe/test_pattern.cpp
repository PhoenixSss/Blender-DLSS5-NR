// SPDX-License-Identifier: MIT
#include "test_pattern.h"

#include <algorithm>
#include <cmath>

namespace blender_dlss5::probe {

canonical::CanonicalColor BuildTestPattern(uint32_t width, uint32_t height) {
    using canonical::CanonicalColor;
    using canonical::ColorEncoding;

    CanonicalColor c = CanonicalColor::zeros(width, height, ColorEncoding::SceneLinear);
    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);

    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const float fx = static_cast<float>(x);
            const float fy = static_cast<float>(y);
            const float nx = fx / w;  // 0..1
            const float ny = fy / h;  // 0..1

            float r, g, b;
            const bool left = nx < 0.5f;

            // Left half: diagonal gradient + zone plate (fine concentric
            // sine detail, strong high-frequency content for the NR).
            // Right half: sky ramp with horizontal bars + a flat skin-tone
            // patch and a highlight block.
            if (left) {
                const float plate = 0.5f + 0.5f * std::sin(0.0008f * (fx * fx + fy * fy));
                r = 0.2f + 0.5f * nx * (1.0f - ny);
                g = 0.6f * plate * (1.0f - ny);
                b = 0.3f + 0.4f * ny * (1.0f - nx);
            } else {
                // Sky ramp.
                r = 0.30f + 0.45f * ny;
                g = 0.50f + 0.35f * ny;
                b = 0.65f + 0.35f * ny;
                // Horizontal high-frequency bars in the upper band.
                if (ny < 0.3f) {
                    const float bar = (std::sin(fy * 0.9f) > 0.0f) ? 0.25f : 0.0f;
                    r += bar;
                    g += bar;
                    b += bar;
                }
                // Flat skin-tone patch (uniform region to reveal NR
                // smoothing behavior).
                if (nx > 0.62f && nx < 0.78f && ny > 0.55f && ny < 0.75f) {
                    r = 0.80f;
                    g = 0.60f;
                    b = 0.50f;
                }
                // Near-white highlight block.
                if (nx > 0.82f && nx < 0.92f && ny > 0.78f && ny < 0.90f) {
                    r = 0.95f;
                    g = 0.95f;
                    b = 0.94f;
                }
            }

            const size_t p = (static_cast<size_t>(y) * width + x) * 4;
            c.rgba_f32[p + 0] = std::clamp(r, 0.0f, 1.0f);
            c.rgba_f32[p + 1] = std::clamp(g, 0.0f, 1.0f);
            c.rgba_f32[p + 2] = std::clamp(b, 0.0f, 1.0f);
            c.rgba_f32[p + 3] = 1.0f;
        }
    }
    return c;
}

canonical::CanonicalColor BuildTestPatternHdr(uint32_t width, uint32_t height) {
    canonical::CanonicalColor c = BuildTestPattern(width, height);

    // Replace the highlight block with 10x values and add a 100x spot.
    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const float nx = static_cast<float>(x) / w;
            const float ny = static_cast<float>(y) / h;
            const size_t p = (static_cast<size_t>(y) * width + x) * 4;
            // 10x highlight block (replaces the near-white block region)
            if (nx > 0.82f && nx < 0.92f && ny > 0.78f && ny < 0.90f) {
                c.rgba_f32[p + 0] = 9.5f;
                c.rgba_f32[p + 1] = 9.5f;
                c.rgba_f32[p + 2] = 9.4f;
            }
            // 100x bright spot (specular-like)
            if (nx > 0.95f && nx < 0.98f && ny > 0.02f && ny < 0.05f) {
                c.rgba_f32[p + 0] = 95.0f;
                c.rgba_f32[p + 1] = 95.0f;
                c.rgba_f32[p + 2] = 94.0f;
            }
        }
    }
    return c;
}

}  // namespace blender_dlss5::probe
