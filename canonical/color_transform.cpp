// SPDX-License-Identifier: MIT
#include "color_transform.h"

#include <algorithm>
#include <cmath>

namespace blender_dlss5::canonical {

namespace {

// --- AgX constants (three.js / Filament AgX, rec.2020 primaries) ----------
// The GLSL sources use mat3(vec3(...), vec3(...), vec3(...)) column-major
// constructors. The C++ arrays below store the EFFECTIVE matrices in
// row-major order (row i = (col0[i], col1[i], col2[i])); each row sums to
// ~1.0, which keeps achromatic inputs neutral (AgX's gray-preserving
// property) — verified by unit tests.

// linear sRGB -> linear Rec.2020
constexpr double kSrgbToRec2020[3][3] = {
    {0.6274, 0.3293, 0.0433},
    {0.0691, 0.9195, 0.0113},
    {0.0164, 0.0880, 0.8956},
};

// linear Rec.2020 -> linear sRGB
constexpr double kRec2020ToSrgb[3][3] = {
    {1.6605, -0.5876, -0.0728},
    {-0.1246, 1.1329, -0.0083},
    {-0.0182, -0.1006, 1.1187},
};

// AgX inset (working space) matrix
constexpr double kAgxInset[3][3] = {
    {0.856627153315983, 0.0951212405381588, 0.0482516061458583},
    {0.137318972929847, 0.761241990602591, 0.101439036467562},
    {0.11189821299995, 0.0767994186031903, 0.811302368396859},
};

// AgX outset matrix
constexpr double kAgxOutset[3][3] = {
    {1.1271005818144368, -0.11060664309660323, -0.016493938717834573},
    {-0.1413297634984383, 1.157823702216272, -0.016493938717834257},
    {-0.14132976349843826, -0.11060664309660294, 1.2519364065950405},
};

// log2 domain endpoints (LOG2_MIN=-10, LOG2_MAX=+6.5, MIDDLE_GRAY=0.18)
constexpr double kAgxMinEv = -12.47393;
constexpr double kAgxMaxEv = 4.026069;

void MatMul3x3(const double m[3][3], double& x, double& y, double& z) {
    const double ix = x, iy = y, iz = z;
    x = m[0][0] * ix + m[0][1] * iy + m[0][2] * iz;
    y = m[1][0] * ix + m[1][1] * iy + m[1][2] * iz;
    z = m[2][0] * ix + m[2][1] * iy + m[2][2] * iz;
}

// The default AgX contrast sigmoid (polynomial fit of the AgX Base curve).
double AgxDefaultContrastApprox(double x) {
    const double x2 = x * x;
    const double x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x +
           0.4298 * x2 + 0.1191 * x - 0.00232;
}

}  // namespace

float SrgbEotf(float x) {
    x = std::max(x, 0.0f);
    if (x <= 0.0031308f) return 12.92f * x;
    return 1.055f * std::pow(x, 1.0f / 2.4f) - 0.055f;
}

void AgxTransform(float* r, float* g, float* b) {
    double x = *r, y = *g, z = *b;
    if (!std::isfinite(x)) x = 0.0;
    if (!std::isfinite(y)) y = 0.0;
    if (!std::isfinite(z)) z = 0.0;

    // linear sRGB -> linear Rec.2020 -> AgX working space
    MatMul3x3(kSrgbToRec2020, x, y, z);
    MatMul3x3(kAgxInset, x, y, z);

    // Log2 encoding over the AgX allocation range
    x = std::log2(std::max(x, 1e-10));
    y = std::log2(std::max(y, 1e-10));
    z = std::log2(std::max(z, 1e-10));
    x = (x - kAgxMinEv) / (kAgxMaxEv - kAgxMinEv);
    y = (y - kAgxMinEv) / (kAgxMaxEv - kAgxMinEv);
    z = (z - kAgxMinEv) / (kAgxMaxEv - kAgxMinEv);
    x = std::clamp(x, 0.0, 1.0);
    y = std::clamp(y, 0.0, 1.0);
    z = std::clamp(z, 0.0, 1.0);

    // Sigmoid (Base look, no look modifications)
    x = AgxDefaultContrastApprox(x);
    y = AgxDefaultContrastApprox(y);
    z = AgxDefaultContrastApprox(z);

    MatMul3x3(kAgxOutset, x, y, z);

    // Linearize back to linear Rec.2020, then to linear sRGB
    x = std::pow(std::max(x, 0.0), 2.2);
    y = std::pow(std::max(y, 0.0), 2.2);
    z = std::pow(std::max(z, 0.0), 2.2);
    MatMul3x3(kRec2020ToSrgb, x, y, z);

    *r = static_cast<float>(std::clamp(x, 0.0, 1.0));
    *g = static_cast<float>(std::clamp(y, 0.0, 1.0));
    *b = static_cast<float>(std::clamp(z, 0.0, 1.0));
}

bool ConvertEncoding(const CanonicalColor& in, ColorEncoding target,
                     CanonicalColor* out) {
    if (!out || !in.valid()) return false;
    if (target == ColorEncoding::ExperimentalProxy) return false;  // D path

    *out = in;
    out->encoding = target;

    if (target == ColorEncoding::SceneLinear) {
        // Identity: keep the raw scene-linear values (NaN sanitized).
        for (float& v : out->rgba_f32) {
            if (!std::isfinite(v)) v = 0.0f;
        }
        return true;
    }
    if (target == ColorEncoding::StandardDisplay) {
        const size_t n = static_cast<size_t>(in.width) * in.height;
        for (size_t i = 0; i < n; ++i) {
            out->rgba_f32[i * 4 + 0] = SrgbEotf(in.rgba_f32[i * 4 + 0]);
            out->rgba_f32[i * 4 + 1] = SrgbEotf(in.rgba_f32[i * 4 + 1]);
            out->rgba_f32[i * 4 + 2] = SrgbEotf(in.rgba_f32[i * 4 + 2]);
            // alpha passes through untouched (A4 owns alpha semantics)
        }
        return true;
    }
    if (target == ColorEncoding::AgXDisplay) {
        const size_t n = static_cast<size_t>(in.width) * in.height;
        for (size_t i = 0; i < n; ++i) {
            float r = in.rgba_f32[i * 4 + 0];
            float g = in.rgba_f32[i * 4 + 1];
            float b = in.rgba_f32[i * 4 + 2];
            AgxTransform(&r, &g, &b);
            // AgxTransform outputs LINEAR tone-mapped values (three.js:
            // "outputs are encoded as Linear-sRGB"). The display encoding
            // (§13: "AgX view -> display/sRGB representation") must
            // include the sRGB encode, otherwise downstream consumers
            // mislabel linear data as sRGB and double-map it (user-found
            // bug 2026-09-05: dark output in Blender's AgX pipeline).
            out->rgba_f32[i * 4 + 0] = SrgbEotf(r);
            out->rgba_f32[i * 4 + 1] = SrgbEotf(g);
            out->rgba_f32[i * 4 + 2] = SrgbEotf(b);
        }
        return true;
    }
    return false;
}

}  // namespace blender_dlss5::canonical
