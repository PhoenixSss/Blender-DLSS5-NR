// SPDX-License-Identifier: MIT
// probe/main.cpp — standalone Feature 18 probe CLI (A1).
//
// test image -> INeuralRenderingBackend (default: registered backend)
//           -> output image + JSON report.
// Validates runtime / D3D12 / NGX / Feature 18 without Blender (§18).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "backends/interface/backend_registry.h"
#include "backends/interface/neural_backend.h"
#include "canonical/color.h"
#include "canonical/color_transform.h"
#include "png_io.h"
#include "report.h"
#include "test_pattern.h"

namespace {

using namespace blender_dlss5;

constexpr const char* kProbeVersion = "0.1.0-a1";

struct Options {
    std::string runtime_dir;
    std::string ngx_core_path;
    std::string input_png;  // PNG or EXR (by extension)
    uint32_t width = 1920;
    uint32_t height = 1080;
    std::string output_prefix;
    std::string report_path;
    int gpu_index = 0;
    std::string backend_name;  // empty = default registered backend
    backends::NeuralSettings settings;
    canonical::ColorEncoding encoding = canonical::ColorEncoding::SceneLinear;
    bool hdr_pattern = false;  // A3: synthetic pattern with >1 highlights
    bool clamp = true;         // default ON: [0,1] is the network's
                               // known-good domain (A3 artifact finding)
    bool reject_unsigned = false;
    bool show_help = false;
    bool show_version = false;
};

void PrintUsage() {
    std::printf(
        "blender_dlss5_nr_probe %s — standalone Feature 18 probe (A1)\n"
        "\n"
        "Usage:\n"
        "  dlss5nr_probe.exe [options]\n"
        "\n"
        "Options:\n"
        "  --runtime <dir>        Directory containing nvngx_dlssnr.dll\n"
        "  --ngx-core <path>      Explicit _nvngx.dll path (default: auto-discovery)\n"
        "  --input <png>          Input image (default: built-in synthetic pattern)\n"
        "  --width <n>            Synthetic pattern width (default 1920)\n"
        "  --height <n>           Synthetic pattern height (default 1080)\n"
        "  --output-prefix <path> Output file prefix (default <exe>\\a1_out)\n"
        "  --report <path>        JSON report path (default <prefix>.report.json)\n"
        "  --gpu <n>              NVIDIA adapter index (default 0)\n"
        "  --backend <name>       Registered backend name (default: first registered)\n"
        "  --style <int>          Style (default 1 = natural)\n"
        "  --preset <int>         Render preset (default 3)\n"
        "  --intensity <f>        Intensity (default 1.0)\n"
        "  --tone <f>             Local tone strength (default 1.0)\n"
        "  --structure <f>        Local structure strength (default 1.0)\n"
        "  --skin <f>             Skin structure strength (default -1.0)\n"
        "  --auto-mask <0|1>      Use auto mask (default 0)\n"
        "  --encoding <name>      scene-linear|standard|agx|proxy (default scene-linear)\n"
        "  --reject-unsigned      Treat an invalid Authenticode signature as an error\n"
        "  --help                 Show this help\n"
        "  --version              Show version\n"
        "\n"
        "Exit codes: 0 success, 2 usage error, 10+ error category (see report).\n",
        kProbeVersion);
}

bool ParseArgs(int argc, char** argv, Options* o, std::string* usage_error) {
    auto need = [&](int* i, const char* flag) -> const char* {
        if (*i + 1 >= argc) {
            if (usage_error) *usage_error = std::string("Missing value for ") + flag;
            return nullptr;
        }
        return argv[++*i];
    };
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--help") { o->show_help = true; return true; }
        if (a == "--version") { o->show_version = true; return true; }
        if (a == "--runtime") { const char* v = need(&i, a.c_str()); if (!v) return false; o->runtime_dir = v; }
        else if (a == "--ngx-core") { const char* v = need(&i, a.c_str()); if (!v) return false; o->ngx_core_path = v; }
        else if (a == "--input") { const char* v = need(&i, a.c_str()); if (!v) return false; o->input_png = v; }
        else if (a == "--width") { const char* v = need(&i, a.c_str()); if (!v) return false; o->width = static_cast<uint32_t>(std::atoi(v)); }
        else if (a == "--height") { const char* v = need(&i, a.c_str()); if (!v) return false; o->height = static_cast<uint32_t>(std::atoi(v)); }
        else if (a == "--output-prefix") { const char* v = need(&i, a.c_str()); if (!v) return false; o->output_prefix = v; }
        else if (a == "--report") { const char* v = need(&i, a.c_str()); if (!v) return false; o->report_path = v; }
        else if (a == "--gpu") { const char* v = need(&i, a.c_str()); if (!v) return false; o->gpu_index = std::atoi(v); }
        else if (a == "--backend") { const char* v = need(&i, a.c_str()); if (!v) return false; o->backend_name = v; }
        else if (a == "--style") { const char* v = need(&i, a.c_str()); if (!v) return false; o->settings.style = std::atoi(v); }
        else if (a == "--preset") { const char* v = need(&i, a.c_str()); if (!v) return false; o->settings.preset = std::atoi(v); }
        else if (a == "--intensity") { const char* v = need(&i, a.c_str()); if (!v) return false; o->settings.intensity = static_cast<float>(std::atof(v)); }
        else if (a == "--tone") { const char* v = need(&i, a.c_str()); if (!v) return false; o->settings.local_tone_strength = static_cast<float>(std::atof(v)); }
        else if (a == "--structure") { const char* v = need(&i, a.c_str()); if (!v) return false; o->settings.local_structure_strength = static_cast<float>(std::atof(v)); }
        else if (a == "--skin") { const char* v = need(&i, a.c_str()); if (!v) return false; o->settings.skin_structure_strength = static_cast<float>(std::atof(v)); }
        else if (a == "--auto-mask") { const char* v = need(&i, a.c_str()); if (!v) return false; o->settings.use_auto_mask = std::atoi(v) != 0; }
        else if (a == "--encoding") {
            const char* v = need(&i, a.c_str());
            if (!v) return false;
            // Canonical names first, then the short CLI aliases.
            if (!canonical::from_string(v, &o->encoding)) {
                const std::string s(v);
                if (s == "scene-linear") o->encoding = canonical::ColorEncoding::SceneLinear;
                else if (s == "standard") o->encoding = canonical::ColorEncoding::StandardDisplay;
                else if (s == "agx") o->encoding = canonical::ColorEncoding::AgXDisplay;
                else {
                    if (usage_error) *usage_error = std::string("Unknown encoding: ") + v;
                    return false;
                }
            }
        }
        else if (a == "--hdr-pattern") { o->hdr_pattern = true; }
        else if (a == "--clamp") { o->clamp = true; }
        else if (a == "--no-clamp") { o->clamp = false; }
        else if (a == "--reject-unsigned") { o->reject_unsigned = true; }
        else {
            if (usage_error) *usage_error = "Unknown option: " + a;
            return false;
        }
    }
    if (o->width == 0 || o->height == 0 ||
        o->width > canonical::CanonicalColor::kMaxDimension ||
        o->height > canonical::CanonicalColor::kMaxDimension) {
        if (usage_error) *usage_error = "Invalid --width/--height";
        return false;
    }
    return true;
}

std::string ExeDir() {
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::string path;
    const int len = WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(n), nullptr, 0, nullptr, nullptr);
    path.resize(static_cast<size_t>(len));
    WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(n), path.data(), len, nullptr, nullptr);
    const size_t pos = path.find_last_of("\\/");
    if (pos != std::string::npos) path.resize(pos);
    return path;
}

bool WriteRawF32(const std::string& path, const canonical::CanonicalColor& frame,
                 std::string* error) {
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) {
        if (error) *error = "Cannot open raw output: " + path;
        return false;
    }
    const size_t bytes = frame.rgba_f32.size() * sizeof(float);
    const size_t written = fwrite(frame.rgba_f32.data(), 1, bytes, f);
    fclose(f);
    if (written != bytes) {
        if (error) *error = "Failed to write raw output: " + path;
        return false;
    }
    return true;
}

// §26 / §25: one evaluation at a time; the report always carries GPU,
// driver and runtime context.
int ExitCodeFor(const backends::BackendError& e) {
    return 10 + static_cast<int>(e.category);
}

}  // namespace

int main(int argc, char** argv) {
    // Unbuffered stdout: the probe also runs piped, where default
    // buffering would hide stage-by-stage progress.
    setvbuf(stdout, nullptr, _IONBF, 0);

    Options o;
    std::string usage_error;
    if (!ParseArgs(argc, argv, &o, &usage_error)) {
        std::fprintf(stderr, "Error: %s\n\n", usage_error.c_str());
        PrintUsage();
        return 2;
    }
    if (o.show_help) { PrintUsage(); return 0; }
    if (o.show_version) {
        std::printf("%s\n", kProbeVersion);
        return 0;
    }

    std::printf("[probe] blender_dlss5_nr_probe %s\n", kProbeVersion);

    const std::string exe_dir = ExeDir();
    if (o.output_prefix.empty()) {
        o.output_prefix = exe_dir.empty() ? "a1_out" : exe_dir + "\\a1_out";
    }
    if (o.report_path.empty()) o.report_path = o.output_prefix + ".report.json";

    probe::ProbeReport report;
    report.probe_version = kProbeVersion;

    // ---- backend creation ------------------------------------------------
    std::unique_ptr<backends::INeuralRenderingBackend> backend(
        o.backend_name.empty()
            ? backends::CreateDefaultBackend()
            : backends::CreateBackendByName(o.backend_name.c_str()));
    if (!backend) {
        if (o.backend_name.empty()) {
            std::fprintf(stderr, "[error] No neural rendering backend available\n");
        } else {
            std::fprintf(stderr, "[error] No neural rendering backend available "
                                 "(requested: %s)\n", o.backend_name.c_str());
        }
        return 21;  // Internal
    }
    report.backend_name = backend->name();
    std::printf("[backend] using \"%s\"\n", backend->name());

    backend->set_option("gpu_index", std::to_string(o.gpu_index).c_str());
    backend->set_option("runtime_dir", o.runtime_dir.c_str());
    if (!o.ngx_core_path.empty()) backend->set_option("ngx_core_path", o.ngx_core_path.c_str());
    backend->set_option("reject_unsigned", o.reject_unsigned ? "1" : "0");
    backend->set_option("clamp_input", o.clamp ? "1" : "0");

    // ---- initialize (GPU policy -> runtime identity -> D3D12 -> NGX) -----
    std::printf("[stage] initializing backend (gpu_check -> runtime_identity -> "
                "d3d12_init -> ngx_init)\n");
    backends::BackendError err;
    if (!backend->initialize(&err)) {
        report.has_error = true;
        report.error = err;
        std::fprintf(stderr,
                     "[error] category=%s stage=%s raw=0x%08X message=%s gpu=%s driver=%s runtime_sha256=%s\n",
                     backends::to_string(err.category), err.stage.c_str(), err.raw_code,
                     err.message.c_str(), err.gpu_name.c_str(), err.driver_version.c_str(),
                     err.runtime_sha256.c_str());
        report.success = false;
        std::string werr;
        probe::WriteReportJson(o.report_path, report, &werr);
        return ExitCodeFor(err);
    }

    const backends::BackendDiagnostics& d = backend->diagnostics();
    report.gpu_name = d.gpu_name;
    report.gpu_vendor = [&]() {
        char buf[16] = {};
        std::snprintf(buf, sizeof(buf), "0x%04X", d.vendor_id);
        return std::string(buf);
    }();
    report.adapter_index = d.adapter_index;
    report.nvidia_index = d.nvidia_index;
    report.architecture = d.architecture;
    report.nvml_arch_raw = d.nvml_arch_raw;
    report.rtx50_compatible = d.rtx50_policy_ok;
    report.detection_method = d.family_detection_method;
    report.driver_version = d.driver_version;
    report.runtime_path = d.runtime_path;
    report.runtime_size = d.runtime_size;
    report.runtime_file_version = d.runtime_file_version;
    report.runtime_sha256 = d.runtime_sha256;
    report.runtime_authenticode = d.runtime_signature;
    report.runtime_signer = d.runtime_signer;
    report.runtime_classification = d.runtime_classification;
    report.ngx_core_path = d.ngx_core_path;
    report.ngx_core_discovery = d.ngx_core_discovery;
    report.shim_path = d.shim_path;

    // Runtime policy summary for the report (§17).
    if (d.runtime_classification == std::string("known_modified")) {
        report.runtime_policy = "rejected";
    } else if (d.runtime_classification == std::string("nvidia_original")) {
        report.runtime_policy = "accepted";
    } else {
        report.runtime_policy = "warned";
    }

    std::printf("[gpu] %s (%s, RTX50 policy %s, %s) driver %s\n",
                d.gpu_name.c_str(), d.architecture.empty() ? "unknown" : d.architecture.c_str(),
                d.rtx50_policy_ok ? "OK" : "FAIL", d.family_detection_method.c_str(),
                d.driver_version.c_str());
    std::printf("[runtime] %s\n", d.runtime_path.c_str());
    std::printf("[runtime] size=%llu file_version=%s sha256=%s\n",
                static_cast<unsigned long long>(d.runtime_size),
                d.runtime_file_version.empty() ? "(none)" : d.runtime_file_version.c_str(),
                d.runtime_sha256.c_str());
    std::printf("[runtime] authenticode=%s signer=%s classification=%s policy=%s\n",
                d.runtime_signature.c_str(),
                d.runtime_signer.empty() ? "(none)" : d.runtime_signer.c_str(),
                d.runtime_classification.c_str(), report.runtime_policy.c_str());
    if (report.runtime_policy == "warned") {
        std::printf("[runtime] WARNING: runtime is not a verified NVIDIA-signed original "
                    "(project policy: user responsibility, §3/§17).\n");
    }
    std::printf("[ngx-core] %s (discovery: %s)\n",
                d.ngx_core_path.c_str(), d.ngx_core_discovery.c_str());
    std::printf("[shim] %s\n", d.shim_path.c_str());

    // ---- input frame ------------------------------------------------------
    canonical::CanonicalColor input;
    if (!o.input_png.empty()) {
        const bool is_exr = o.input_png.size() > 4 &&
                            o.input_png.compare(o.input_png.size() - 4, 4, ".exr") == 0;
        std::string perr;
        const bool loaded = is_exr ? probe::LoadExr(o.input_png, &input, &perr)
                                   : probe::LoadPng(o.input_png, &input, &perr);
        if (!loaded) {
            std::fprintf(stderr, "[error] %s\n", perr.c_str());
            report.has_error = true;
            report.error.category = backends::BackendErrorCategory::Internal;
            report.error.stage = "input";
            report.error.message = perr;
            report.success = false;
            std::string werr;
            probe::WriteReportJson(o.report_path, report, &werr);
            return ExitCodeFor(report.error);
        }
        std::printf("[input] %s (%ux%u, %s)\n", o.input_png.c_str(), input.width,
                    input.height, is_exr ? "EXR" : "PNG");
    } else {
        input = o.hdr_pattern ? probe::BuildTestPatternHdr(o.width, o.height)
                              : probe::BuildTestPattern(o.width, o.height);
        std::printf("[input] synthetic %s pattern (%ux%u)\n",
                    o.hdr_pattern ? "HDR" : "test", input.width, input.height);
    }
    input.encoding = o.encoding;
    report.width = input.width;
    report.height = input.height;
    report.encoding = canonical::to_string(o.encoding);

    // §13 A/B/C: convert the scene-linear input to the requested color
    // domain (A3 — this used to be metadata-only).
    {
        canonical::CanonicalColor converted;
        if (!canonical::ConvertEncoding(input, o.encoding, &converted)) {
            std::fprintf(stderr, "[error] Unsupported encoding conversion: %s\n",
                         canonical::to_string(o.encoding));
            report.has_error = true;
            report.error.category = backends::BackendErrorCategory::Internal;
            report.error.stage = "input";
            report.error.message = "Unsupported encoding conversion";
            report.success = false;
            std::string werr;
            probe::WriteReportJson(o.report_path, report, &werr);
            return ExitCodeFor(report.error);
        }
        input = std::move(converted);
        std::printf("[input] encoded as %s (clamp=%s)\n", canonical::to_string(o.encoding),
                    o.clamp ? "on" : "off");
    }
    report.style = o.settings.style;
    report.preset = o.settings.preset;
    report.intensity = o.settings.intensity;
    report.tone = o.settings.local_tone_strength;
    report.structure = o.settings.local_structure_strength;
    report.skin = o.settings.skin_structure_strength;
    report.auto_mask = o.settings.use_auto_mask;

    report.input_sha256 = probe::FrameSha256(input);

    // ---- evaluate ---------------------------------------------------------
    canonical::CanonicalColor output;
    LARGE_INTEGER freq{}, t0{}, t1{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    const bool ok = backend->process(input, output, o.settings, &err);
    QueryPerformanceCounter(&t1);
    report.processing_ms = (static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0) /
                           static_cast<double>(freq.QuadPart);
    report.create_feature_result = backend->diagnostics().create_feature_result;
    report.evaluate_feature_result = backend->diagnostics().last_evaluate_result;

    if (!ok) {
        report.has_error = true;
        report.error = err;
        std::fprintf(stderr,
                     "[error] category=%s stage=%s raw=0x%08X message=%s gpu=%s driver=%s runtime_sha256=%s\n",
                     backends::to_string(err.category), err.stage.c_str(), err.raw_code,
                     err.message.c_str(), err.gpu_name.c_str(), err.driver_version.c_str(),
                     err.runtime_sha256.c_str());
        report.success = false;
        std::string werr;
        probe::WriteReportJson(o.report_path, report, &werr);
        return ExitCodeFor(err);
    }

    std::printf("[%s] CreateFeature -> %d\n",
                backend->name(), report.create_feature_result);
    std::printf("[evaluate] EvaluateFeature -> %d (%.1f ms)\n",
                report.evaluate_feature_result, report.processing_ms);

    // ---- outputs ----------------------------------------------------------
    report.output_sha256 = probe::FrameSha256(output);

    const std::string input_png = o.output_prefix + ".input.png";
    const std::string output_png = o.output_prefix + ".png";
    const std::string raw_path = o.output_prefix + ".rgba_f32.bin";
    std::string perr;

    bool io_ok = true;
    if (!probe::SavePng(input_png, input, &perr)) {
        std::fprintf(stderr, "[error] %s\n", perr.c_str());
        io_ok = false;
    }
    if (io_ok && !probe::SavePng(output_png, output, &perr)) {
        std::fprintf(stderr, "[error] %s\n", perr.c_str());
        io_ok = false;
    }
    if (io_ok && !WriteRawF32(raw_path, output, &perr)) {
        std::fprintf(stderr, "[error] %s\n", perr.c_str());
        io_ok = false;
    }
    if (!io_ok) {
        report.has_error = true;
        report.error.category = backends::BackendErrorCategory::Internal;
        report.error.stage = "output";
        report.error.message = perr;
        report.success = false;
        std::string werr;
        probe::WriteReportJson(o.report_path, report, &werr);
        return ExitCodeFor(report.error);
    }

    report.output_png = output_png;
    report.output_raw_f32 = raw_path;
    report.success = true;

    std::printf("[probe] input_sha256=%s\n[probe] output_sha256=%s\n",
                report.input_sha256.c_str(), report.output_sha256.c_str());
    std::printf("[probe] SUCCESS (report: %s)\n", o.report_path.c_str());

    std::string werr;
    if (!probe::WriteReportJson(o.report_path, report, &werr)) {
        std::fprintf(stderr, "[error] %s\n", werr.c_str());
        return 21;
    }
    return 0;
}
