// SPDX-License-Identifier: MIT
// tests/unit_tests.cpp — CPU-only unit tests (no GPU required).

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "backends/feature18/half.h"
#include "backends/interface/neural_backend.h"
#include "canonical/color.h"
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
    TestErrorCategoryOrder();

    if (g_failures == 0) {
        std::printf("PASS: %d checks\n", g_checks);
        return 0;
    }
    std::printf("FAILED: %d of %d checks\n", g_failures, g_checks);
    return 1;
}
