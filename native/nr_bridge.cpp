// SPDX-License-Identifier: MIT
// native/nr_bridge.cpp — C ABI bridge around the backend interface.
//
// The bridge owns a single backend instance (default = first registered)
// and serializes all calls on a mutex (§26). It knows nothing about any
// concrete backend's ABI: only the interface layer types appear here.

#define NR_BRIDGE_EXPORTS
#include "nr_bridge.h"

#include <cstring>
#include <memory>
#include <mutex>
#include <string>

#include "backends/interface/backend_registry.h"
#include "backends/interface/neural_backend.h"
#include "canonical/color_transform.h"

namespace {

using blender_dlss5::backends::BackendError;
using blender_dlss5::backends::BackendErrorCategory;
using blender_dlss5::backends::INeuralRenderingBackend;
using blender_dlss5::backends::NeuralSettings;
using blender_dlss5::canonical::CanonicalColor;
using blender_dlss5::canonical::ColorEncoding;
using blender_dlss5::canonical::ConvertEncoding;

void CopyStr(char* dst, size_t cap, const std::string& src) {
    if (!dst || cap == 0) return;
    const size_t n = src.size() < cap - 1 ? src.size() : cap - 1;
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

void FillError(NR_Error* err, const BackendError& be) {
    if (!err) return;
    std::memset(err, 0, sizeof(*err));
    err->category = static_cast<int>(be.category);
    err->raw_code = be.raw_code;
    CopyStr(err->stage, sizeof(err->stage), be.stage);
    CopyStr(err->message, sizeof(err->message), be.message);
    CopyStr(err->gpu_name, sizeof(err->gpu_name), be.gpu_name);
    CopyStr(err->driver_version, sizeof(err->driver_version), be.driver_version);
    CopyStr(err->runtime_version, sizeof(err->runtime_version), be.runtime_version);
    CopyStr(err->runtime_sha256, sizeof(err->runtime_sha256), be.runtime_sha256);
}

void ClearError(NR_Error* err) {
    if (err) std::memset(err, 0, sizeof(*err));
}

}  // namespace

struct NR_Context {
    std::unique_ptr<INeuralRenderingBackend> backend;
    std::mutex mutex;  // §26: at most one evaluation at a time
};

extern "C" {

const char* __cdecl nr_version(void) {
    return "0.2.0-a3";
}

int __cdecl nr_create(NR_Context** out, const NR_Options* opts, NR_Error* err) {
    ClearError(err);
    if (!out) {
        if (err) {
            BackendError be;
            be.category = BackendErrorCategory::Internal;
            be.message = "nr_create: out is NULL";
            FillError(err, be);
        }
        return 0;
    }
    *out = nullptr;

    auto ctx = std::unique_ptr<NR_Context>(new NR_Context());
    ctx->backend.reset(blender_dlss5::backends::CreateDefaultBackend());
    if (!ctx->backend) {
        BackendError be;
        be.category = BackendErrorCategory::Internal;
        be.stage = "backend_create";
        be.message = "No neural rendering backend registered";
        FillError(err, be);
        return 0;
    }

    if (opts) {
        const auto set = [&](const char* key, const char* value) {
            ctx->backend->set_option(key, value);
        };
        if (opts->runtime_dir && opts->runtime_dir[0]) {
            set("runtime_dir", opts->runtime_dir);
        }
        if (opts->ngx_core_path && opts->ngx_core_path[0]) {
            set("ngx_core_path", opts->ngx_core_path);
        }
        const std::string gpu = std::to_string(opts->gpu_index);
        set("gpu_index", gpu.c_str());
        set("reject_unsigned", opts->reject_unsigned ? "1" : "0");
        set("clamp_input", opts->clamp_input ? "1" : "0");
    }

    *out = ctx.release();
    return 1;
}

int __cdecl nr_initialize(NR_Context* ctx, NR_Error* err) {
    ClearError(err);
    if (!ctx || !ctx->backend) {
        if (err) {
            BackendError be;
            be.category = BackendErrorCategory::Internal;
            be.message = "nr_initialize: invalid context";
            FillError(err, be);
        }
        return 0;
    }
    std::lock_guard<std::mutex> lock(ctx->mutex);
    BackendError be;
    if (!ctx->backend->initialize(&be)) {
        FillError(err, be);
        return 0;
    }
    return 1;
}

int __cdecl nr_evaluate(NR_Context* ctx, const NR_FrameDesc* frame,
                        const NR_Settings* settings, float* out_rgba_f32,
                        NR_Error* err) {
    ClearError(err);
    if (!ctx || !ctx->backend || !frame || !out_rgba_f32 || !settings) {
        if (err) {
            BackendError be;
            be.category = BackendErrorCategory::Internal;
            be.message = "nr_evaluate: invalid arguments";
            FillError(err, be);
        }
        return 0;
    }

    CanonicalColor in;
    in.width = frame->width;
    in.height = frame->height;
    in.rgba_f32.assign(frame->rgba_f32,
                       frame->rgba_f32 +
                           static_cast<size_t>(frame->width) * frame->height * 4);

    // §13 A/B/C: the frame always carries scene-linear values; convert to
    // the requested color domain before evaluation. The backend's resource
    // format is a separate concern from these color semantics.
    CanonicalColor encoded;
    const int enc = frame->encoding;
    if (enc < 0 || enc > 2 ||
        !ConvertEncoding(in, static_cast<ColorEncoding>(enc), &encoded)) {
        BackendError e0;
        e0.category = BackendErrorCategory::Internal;
        e0.stage = "evaluate";
        e0.message = "Unsupported encoding value in frame descriptor";
        FillError(err, e0);
        return 0;
    }
    CanonicalColor out;

    NeuralSettings s;
    s.style = settings->style;
    s.preset = settings->preset;
    s.intensity = settings->intensity;
    s.local_tone_strength = settings->local_tone_strength;
    s.local_structure_strength = settings->local_structure_strength;
    s.skin_structure_strength = settings->skin_structure_strength;
    s.use_auto_mask = settings->use_auto_mask != 0;

    std::lock_guard<std::mutex> lock(ctx->mutex);
    BackendError be;
    if (!ctx->backend->process(encoded, out, s, &be)) {
        FillError(err, be);
        return 0;
    }
    if (out.rgba_f32.size() != encoded.rgba_f32.size()) {
        BackendError e2;
        e2.category = BackendErrorCategory::InvalidOutput;
        e2.stage = "evaluate";
        e2.message = "Backend returned a mismatched output buffer size";
        FillError(err, e2);
        return 0;
    }
    std::memcpy(out_rgba_f32, out.rgba_f32.data(), out.rgba_f32.size() * sizeof(float));
    return 1;
}

int __cdecl nr_get_diagnostics(NR_Context* ctx, NR_Diagnostics* out) {
    if (!ctx || !ctx->backend || !out) return 0;
    std::memset(out, 0, sizeof(*out));
    std::lock_guard<std::mutex> lock(ctx->mutex);
    const auto& d = ctx->backend->diagnostics();
    CopyStr(out->backend_name, sizeof(out->backend_name), d.backend_name);
    CopyStr(out->gpu_name, sizeof(out->gpu_name), d.gpu_name);
    out->vendor_id = d.vendor_id;
    out->adapter_index = d.adapter_index;
    out->nvidia_index = d.nvidia_index;
    CopyStr(out->architecture, sizeof(out->architecture), d.architecture);
    out->nvml_arch_raw = d.nvml_arch_raw;
    out->rtx50_policy_ok = d.rtx50_policy_ok ? 1 : 0;
    CopyStr(out->family_detection_method, sizeof(out->family_detection_method),
            d.family_detection_method);
    CopyStr(out->driver_version, sizeof(out->driver_version), d.driver_version);
    CopyStr(out->runtime_path, sizeof(out->runtime_path), d.runtime_path);
    out->runtime_size = d.runtime_size;
    CopyStr(out->runtime_file_version, sizeof(out->runtime_file_version),
            d.runtime_file_version);
    CopyStr(out->runtime_sha256, sizeof(out->runtime_sha256), d.runtime_sha256);
    CopyStr(out->runtime_signature, sizeof(out->runtime_signature), d.runtime_signature);
    CopyStr(out->runtime_signer, sizeof(out->runtime_signer), d.runtime_signer);
    CopyStr(out->runtime_classification, sizeof(out->runtime_classification),
            d.runtime_classification);
    CopyStr(out->ngx_core_path, sizeof(out->ngx_core_path), d.ngx_core_path);
    CopyStr(out->ngx_core_discovery, sizeof(out->ngx_core_discovery),
            d.ngx_core_discovery);
    CopyStr(out->shim_path, sizeof(out->shim_path), d.shim_path);
    out->create_feature_result = d.create_feature_result;
    out->last_evaluate_result = d.last_evaluate_result;
    return 1;
}

void __cdecl nr_destroy(NR_Context* ctx) {
    delete ctx;
}

}  // extern "C"
