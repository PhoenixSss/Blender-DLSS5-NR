// SPDX-License-Identifier: MIT
// backends/interface/neural_backend.h — backend-independent contract.
//
// The Blender layer (and the probe) only knows these types. No raw
// backend ABI details (NGX parameter names, feature IDs, caller-module
// validation concepts) may appear here (requirements §7).

#pragma once

#include <string>

#include "canonical/color.h"

namespace blender_dlss5::backends {

// §25: distinct error categories. The enum order is load-bearing: probe exit
// codes are derived from it (10 + category value).
enum class BackendErrorCategory : int {
    None = 0,
    UnsupportedGpu = 1,
    ModifiedRuntimeRejected = 2,
    RuntimeMissing = 3,
    InvalidAuthenticode = 4,
    NgxCoreMissing = 5,
    D3D12InitFailure = 6,
    NgxInitFailure = 7,
    FeatureCreationFailure = 8,
    EvaluateFailure = 9,
    ReadbackFailure = 10,
    InvalidOutput = 11,
    Internal = 12,
};

inline const char* to_string(BackendErrorCategory c) {
    switch (c) {
        case BackendErrorCategory::None: return "None";
        case BackendErrorCategory::UnsupportedGpu: return "UnsupportedGpu";
        case BackendErrorCategory::ModifiedRuntimeRejected: return "ModifiedRuntimeRejected";
        case BackendErrorCategory::RuntimeMissing: return "RuntimeMissing";
        case BackendErrorCategory::InvalidAuthenticode: return "InvalidAuthenticode";
        case BackendErrorCategory::NgxCoreMissing: return "NgxCoreMissing";
        case BackendErrorCategory::D3D12InitFailure: return "D3D12InitFailure";
        case BackendErrorCategory::NgxInitFailure: return "NgxInitFailure";
        case BackendErrorCategory::FeatureCreationFailure: return "FeatureCreationFailure";
        case BackendErrorCategory::EvaluateFailure: return "EvaluateFailure";
        case BackendErrorCategory::ReadbackFailure: return "ReadbackFailure";
        case BackendErrorCategory::InvalidOutput: return "InvalidOutput";
        case BackendErrorCategory::Internal: return "Internal";
    }
    return "Unknown";
}

// §25: every error must carry context — GPU, driver, runtime identity,
// failed stage and the raw code. Never just "Processing failed".
struct BackendError {
    BackendErrorCategory category = BackendErrorCategory::None;
    std::string message;
    std::string stage;  // gpu_check | runtime_identity | ngx_core | d3d12_init |
                        // ngx_init | feature_create | evaluate | readback | output
    uint32_t raw_code = 0;
    std::string gpu_name;
    std::string driver_version;
    std::string runtime_version;
    std::string runtime_sha256;

    bool ok() const { return category == BackendErrorCategory::None; }
};

// Defaults reproduce the verified still-image working set of the reference
// implementation (ComfyUI-DLSS5-NR v0.3.0).
struct NeuralSettings {
    int style = 1;                       // "natural"
    int preset = 3;
    float intensity = 1.0f;
    float local_tone_strength = 1.0f;
    float local_structure_strength = 1.0f;
    float skin_structure_strength = -1.0f;
    bool use_auto_mask = false;
};

// §16/§17: runtime info snapshot. Generic fields only — the concrete backend
// fills what it can. No Feature-18 specific names here.
struct BackendDiagnostics {
    std::string backend_name;
    std::string gpu_name;
    uint32_t vendor_id = 0;
    int adapter_index = -1;
    int nvidia_index = -1;
    std::string architecture;         // e.g. "Blackwell"
    int nvml_arch_raw = -1;           // raw NVML architecture enum value
    bool rtx50_policy_ok = false;
    std::string family_detection_method;  // "NVML" | "DXGI description"
    std::string driver_version;
    std::string runtime_path;
    // Runtime identity (§16/§17). Values filled by the backend from its own
    // file-identity pass; signature/classification use the diagnostics
    // module's string forms.
    uint64_t runtime_size = 0;
    std::string runtime_file_version;
    std::string runtime_sha256;
    std::string runtime_signature;        // SignatureStatus name
    std::string runtime_signer;
    std::string runtime_classification;   // RuntimeClassification name
    std::string ngx_core_path;
    std::string ngx_core_discovery;   // "override" | "runtime_dir" | "loader" | "driver_store"
    std::string shim_path;
    int create_feature_result = 0;    // NGXResult; NGX_SUCCESS == 1
    int last_evaluate_result = 0;
};

class INeuralRenderingBackend {
public:
    virtual ~INeuralRenderingBackend() = default;

    // Backend name for registry/UI purposes; concrete names are
    // self-registered by their backend implementation.
    virtual const char* name() const = 0;

    // Generic string options; the probe maps its CLI onto these:
    //   gpu_index     (int)   NVIDIA adapter index
    //   runtime_dir   (path)  directory containing the neural runtime DLL
    //   ngx_core_path (path)  explicit NGX core (_nvngx.dll) override
    virtual bool set_option(const char* key, const char* value) = 0;

    // GPU policy gate (§10) then D3D12, NGX init and feature creation.
    virtual bool initialize(BackendError* err) = 0;

    // Still-image evaluation: Reset=true, same resolution, color only (§12).
    // Output keeps the input encoding/premultiplied metadata.
    virtual bool process(const canonical::CanonicalColor& in,
                         canonical::CanonicalColor& out,
                         const NeuralSettings& settings,
                         BackendError* err) = 0;

    virtual void shutdown() = 0;
    virtual const BackendDiagnostics& diagnostics() const = 0;
};

}  // namespace blender_dlss5::backends
