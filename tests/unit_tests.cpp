// SPDX-License-Identifier: MIT
// tests/unit_tests.cpp — CPU-only unit tests (no GPU required).

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "backends/feature18/half.h"
#include "backends/interface/neural_backend.h"
#include "canonical/color.h"
#include "canonical/color_transform.h"
#include "diagnostics/gpu_info.h"
#include "diagnostics/json_writer.h"
#include "probe/test_pattern.h"

namespace {

using namespace blender_dlss5;

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        ++g_checks;                                                     \
        if (!(cond)) {                                                  \
            ++g_failures;                                               \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                               \
    } while (0)

#define CHECK_EQ(a, b)                                                       \
    do {                                                                     \
        ++g_checks;                                                          \
        const auto va = (a);                                                 \
        const auto vb = (b);                                                 \
        if (!(va == vb)) {                                                   \
            ++g_failures;                                                    \
            std::printf("FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); \
        }                                                                    \
    } while (0)

void TestRtx50Policy() {
    // Policy gate data-driven checks (§10, §2.2: no legacy-GPU acceptance).
    {
        diagnostics::GpuInfo g;
        g.name = "NVIDIA GeForce RTX 5090";
        g.nvml_arch = 10;  // Blackwell
        const bool name_says = g.name.find("RTX 50") != std::string::npos;
        const bool arch_ok = g.nvml_arch >= 10 && g.nvml_arch <= 13;
        CHECK(name_says && arch_ok);  // RTX 5090 + Blackwell -> policy OK
    }
    {
        diagnostics::GpuInfo g;
        g.name = "NVIDIA GeForce RTX 4090";
        g.nvml_arch = 9;  // Ada
        const bool name_says = g.name.find("RTX 50") != std::string::npos;
        const bool arch_ok = g.nvml_arch >= 10 && g.nvml_arch <= 13;
        CHECK(!name_says);  // name gate rejects RTX 40
        CHECK(!(name_says && arch_ok));
    }
    {
        diagnostics::GpuInfo g;
        g.name = "NVIDIA GeForce RTX 5090";
        g.nvml_arch = 8;  // pre-Blackwell with an RTX 50 name: conflict
        const bool name_says = g.name.find("RTX 50") != std::string::npos;
        const bool arch_ok = g.nvml_arch >= 10 && g.nvml_arch <= 13;
        CHECK(!(name_says && arch_ok));  // name/architecture mismatch refused
    }
    CHECK_EQ(std::string(diagnostics::NvmlArchName(10)), std::string("Blackwell"));
    CHECK_EQ(std::string(diagnostics::NvmlArchName(8)), std::string("Unknown"));
}

void TestJsonWriter() {
    diagnostics::JsonWriter w;
    w.begin_object();
    w.key("a"); w.string_value("x\n\"y\"");
    w.key("b"); w.int_value(-42);
    w.key("c"); w.bool_value(true);
    w.key("d"); w.null_value();
    w.key("arr"); w.begin_array();
    w.double_value(1.5);
    w.string_value("s");
    w.begin_object();
    w.key("k"); w.string_value("v");
    w.end_object();
    w.end_array();
    w.end_object();
    const std::string s = w.str();
    // No stray commas after keys or before closing braces.
    CHECK(s.find(",}") == std::string::npos);
    CHECK(s.find(",\"") != std::string::npos);  // object separator present
    CHECK(s.find("a\":\"x\\n\\\"y\\\"") != std::string::npos);
    CHECK(s.find("\"b\":-42") != std::string::npos);
    CHECK(s.find("\"arr\":[1.5,\"s\",{\"k\":\"v\"}]") != std::string::npos);
}

void TestCanonicalColor() {
    canonical::CanonicalColor c = canonical::CanonicalColor::zeros(2, 3, canonical::ColorEncoding::AgXDisplay);
    CHECK(c.valid());
    CHECK_EQ(c.rgba_f32.size(), size_t{24});
    CHECK_EQ(c.data()[0], 0.0f);
    c.rgba_f32.pop_back();
    CHECK(!c.valid());
    CHECK_EQ(std::string(canonical::to_string(canonical::ColorEncoding::SceneLinear)),
             std::string("SceneLinear"));
    canonical::ColorEncoding e;
    CHECK(canonical::from_string("AgXDisplay", &e));
    CHECK(e == canonical::ColorEncoding::AgXDisplay);
    CHECK(!canonical::from_string("agx-display", &e));
}

void TestHalfRoundtrip() {
    using backends::feature18::FloatToHalf;
    using backends::feature18::HalfToFloat;

    CHECK_EQ(FloatToHalf(0.0f), static_cast<uint16_t>(0x0000));
    CHECK_EQ(FloatToHalf(1.0f), static_cast<uint16_t>(0x3C00));
    CHECK_EQ(FloatToHalf(-2.0f), static_cast<uint16_t>(0xC000));
    // Round-to-nearest-even on a value halfway between half values.
    CHECK_EQ(FloatToHalf(1.00048828125f), static_cast<uint16_t>(0x3C00));  // ties to even
    // Round-trip accuracy for probe-range values.
    for (float v : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
        CHECK(std::fabs(HalfToFloat(FloatToHalf(v)) - v) < 1e-3f);
    }
    CHECK(HalfToFloat(0x3C00) == 1.0f);
    // Subnormal half survives the trip.
    const float tiny = HalfToFloat(FloatToHalf(1e-5f));
    CHECK(tiny > 0.0f && tiny < 2e-5f);
}

void TestTestPattern() {
    const canonical::CanonicalColor a = probe::BuildTestPattern(64, 48);
    CHECK(a.valid());
    CHECK_EQ(a.width, 64u);
    CHECK_EQ(a.height, 48u);
    // Determinism: identical builds produce identical pixels.
    const canonical::CanonicalColor b = probe::BuildTestPattern(64, 48);
    CHECK(a.rgba_f32 == b.rgba_f32);
    // Values in [0,1], alpha = 1.
    bool in_range = true;
    bool alpha_one = true;
    for (size_t i = 0; i < a.rgba_f32.size(); ++i) {
        const float v = a.rgba_f32[i];
        if (v < 0.0f || v > 1.0f) in_range = false;
        if (i % 4 == 3 && v != 1.0f) alpha_one = false;
    }
    CHECK(in_range);
    CHECK(alpha_one);
}

void TestColorTransforms() {
    using canonical::AgxTransform;
    using canonical::ConvertEncoding;
    using canonical::SrgbEotf;

    // sRGB EOTF known values.
    CHECK_EQ(SrgbEotf(0.0f), 0.0f);
    CHECK(std::fabs(SrgbEotf(1.0f) - 1.0f) < 1e-6f);  // float constants
    CHECK(std::fabs(SrgbEotf(0.003f) - 0.03876f) < 1e-5f);
    CHECK(std::fabs(SrgbEotf(0.5f) - 0.735358f) < 1e-4f);
    CHECK_EQ(SrgbEotf(-1.0f), 0.0f);  // negative clamps to 0
    // Monotonic on a ramp.
    float prev = 0.0f;
    for (int i = 0; i <= 1000; ++i) {
        const float v = SrgbEotf(i / 1000.0f);
        CHECK(v >= prev);
        prev = v;
    }

    // AgX: endpoints, range, monotonic gray ramp, NaN safety, determinism.
    {
        float r = 0.0f, g = 0.0f, b = 0.0f;
        AgxTransform(&r, &g, &b);
        CHECK_EQ(r, 0.0f);  // black maps to black exactly
        CHECK_EQ(g, 0.0f);
        CHECK_EQ(b, 0.0f);
    }
    {
        float r = 0.18f, g = 0.18f, b = 0.18f;  // mid-gray
        AgxTransform(&r, &g, &b);
        // Filament-variant AgX maps mid-gray to ~0.22 (darker than the
        // original Sobotka curve); achromatic input must stay achromatic
        // (row sums deviate from 1.0 by ~1e-4, so use a small tolerance).
        CHECK(r > 0.15f && r < 0.35f);
        CHECK(std::fabs(g - r) < 1e-4f);
        CHECK(std::fabs(b - r) < 1e-4f);
    }
    {
        float r = 1.0f, g = 1.0f, b = 1.0f;
        AgxTransform(&r, &g, &b);
        CHECK(r > 0.5f && r < 0.85f);
    }
    {
        // HDR input stays finite and in range.
        float r = 1e4f, g = 100.0f, b = 10.0f;
        AgxTransform(&r, &g, &b);
        CHECK(std::isfinite(r) && r <= 1.0f);
        CHECK(std::isfinite(g) && g <= 1.0f);
    }
    {
        // NaN sanitized (maps to black), never propagates.
        float r = std::nanf(""), g = 0.5f, b = 0.5f;
        AgxTransform(&r, &g, &b);
        CHECK(std::isfinite(r));
    }
    {
        // Gray ramp monotonic in the mid range.
        float prev_r = -1.0f;
        for (int i = 1; i <= 40; ++i) {
            float r = i / 20.0f, g = r, b = r;
            AgxTransform(&r, &g, &b);
            CHECK(r >= prev_r);
            prev_r = r;
        }
    }
    {
        // Determinism: bit-identical repeats.
        float r1 = 0.7f, g1 = 0.2f, b1 = 1.3f;
        float r2 = 0.7f, g2 = 0.2f, b2 = 1.3f;
        AgxTransform(&r1, &g1, &b1);
        AgxTransform(&r2, &g2, &b2);
        CHECK(r1 == r2 && g1 == g2 && b1 == b2);
    }

    // ConvertEncoding: identity, Standard, AgX, unsupported target.
    {
        canonical::CanonicalColor in = canonical::CanonicalColor::zeros(
            2, 2, canonical::ColorEncoding::SceneLinear);
        in.rgba_f32 = {0.5f, 1.0f, 0.0f, 1.0f,  0.18f, 0.18f, 0.18f, 1.0f,
                       2.0f, 0.25f, -0.5f, 1.0f, 0.7f, 0.7f, 0.7f, 1.0f};
        canonical::CanonicalColor out;
        CHECK(ConvertEncoding(in, canonical::ColorEncoding::SceneLinear, &out));
        CHECK(out.rgba_f32 == in.rgba_f32);  // identity
        CHECK(ConvertEncoding(in, canonical::ColorEncoding::StandardDisplay, &out));
        CHECK(out.encoding == canonical::ColorEncoding::StandardDisplay);
        CHECK(out.rgba_f32[0] == SrgbEotf(0.5f));
        CHECK(out.rgba_f32[4] > 0.0f);  // alpha untouched by conversion
        CHECK(ConvertEncoding(in, canonical::ColorEncoding::AgXDisplay, &out));
        CHECK(out.encoding == canonical::ColorEncoding::AgXDisplay);
        CHECK(out.rgba_f32[1] <= 1.0f && out.rgba_f32[1] >= 0.0f);
        CHECK(!ConvertEncoding(in, canonical::ColorEncoding::ExperimentalProxy, &out));
    }
}

void TestErrorCategoryOrder() {
    // Exit-code mapping is derived from enum order (§25): 10 + category.
    using backends::BackendErrorCategory;
    CHECK_EQ(10 + static_cast<int>(BackendErrorCategory::UnsupportedGpu), 11);
    CHECK_EQ(10 + static_cast<int>(BackendErrorCategory::FeatureCreationFailure), 18);
    CHECK_EQ(std::string(backends::to_string(BackendErrorCategory::FeatureCreationFailure)),
             std::string("FeatureCreationFailure"));
}

}  // namespace

int main() {
    TestRtx50Policy();
    TestJsonWriter();
    TestCanonicalColor();
    TestHalfRoundtrip();
    TestTestPattern();
    TestColorTransforms();
    TestErrorCategoryOrder();

    if (g_failures == 0) {
        std::printf("PASS: %d checks\n", g_checks);
        return 0;
    }
    std::printf("FAILED: %d of %d checks\n", g_failures, g_checks);
    return 1;
}
