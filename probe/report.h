// SPDX-License-Identifier: MIT
// probe/report.h — probe result aggregation and JSON report (§16/§17/§18).

#pragma once

#include <cstdint>
#include <string>

#include "backends/interface/neural_backend.h"
#include "canonical/color.h"

namespace blender_dlss5::probe {

struct ProbeReport {
    // Static identity.
    std::string schema_version = "1";
    std::string probe_name = "blender_dlss5_nr_probe";
    std::string probe_version = "0.1.0-a1";
    std::string timestamp_utc;

    bool success = false;

    // GPU + driver.
    std::string gpu_name;
    std::string gpu_vendor;             // "0x10DE"
    int adapter_index = -1;
    int nvidia_index = -1;
    std::string architecture;
    int nvml_arch_raw = -1;
    bool rtx50_compatible = false;
    std::string detection_method;
    std::string driver_version;

    // Runtime identity (§16/§17).
    std::string runtime_path;
    uint64_t runtime_size = 0;
    std::string runtime_file_version;
    std::string runtime_sha256;
    std::string runtime_authenticode;
    std::string runtime_signer;
    std::string runtime_classification;
    std::string runtime_policy;         // "accepted" | "warned" | "rejected"

    // NGX core + shim.
    std::string ngx_core_path;
    std::string ngx_core_discovery;
    std::string shim_path;

    // Backend + settings.
    std::string backend_name;
    int style = 0, preset = 0;
    float intensity = 0, tone = 0, structure = 0, skin = 0;
    bool auto_mask = false;
    bool reset = true;
    std::string encoding;               // canonical ColorEncoding name

    // Frame.
    uint32_t width = 0, height = 0;

    // Hashes over the raw rgba_f32 bytes (§18).
    std::string input_sha256;
    std::string output_sha256;

    // NGX results (§18).
    int create_feature_result = 0;
    int evaluate_feature_result = 0;
    double processing_ms = 0.0;

    // Outputs.
    std::string output_png;
    std::string output_raw_f32;

    // Error (present on failure, §25).
    backends::BackendError error;
    bool has_error = false;
};

// Serializes the report to `path`; returns false on write failure.
bool WriteReportJson(const std::string& path, const ProbeReport& r,
                     std::string* error);

// Convenience: SHA-256 over a canonical frame's raw float bytes.
std::string FrameSha256(const canonical::CanonicalColor& frame);

}  // namespace blender_dlss5::probe
