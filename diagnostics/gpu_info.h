// SPDX-License-Identifier: MIT
// diagnostics/gpu_info.h — NVIDIA GPU enumeration + RTX 50 policy check.
//
// Uses NVML (dynamically loaded nvml.dll, shipped with the display driver)
// for the architecture query and driver version, with a DXGI description
// fallback. Policy (§10): NVIDIA vendor + RTX 50 family is a hard gate for
// the Feature 18 backend; it is a necessary, not sufficient, condition.

#pragma once

#include <cstdint>
#include <string>

namespace blender_dlss5::diagnostics {

struct GpuInfo {
    std::string name;              // UTF-8 DXGI adapter description (authoritative)
    uint32_t vendor_id = 0;        // 0x10DE = NVIDIA
    int adapter_index = -1;        // DXGI enumeration index
    int nvidia_index = -1;         // index among NVIDIA adapters
    std::string driver_version;    // e.g. "616.64"
    int nvml_arch = -1;            // raw NVML_DEVICE_ARCH_* value (10..13 = Blackwell gen)
    std::string arch_name;         // human-readable, e.g. "Blackwell"
    bool nvml_ok = false;          // NVML path was usable
    bool is_rtx50 = false;         // project policy gate result
    std::string family_detection_method;  // "NVML" | "DXGI description" | "NVML/DXGI conflict"
};

// Enumerates the nvidia_index-th NVIDIA adapter via DXGI and queries NVML.
// Returns false only on hard enumeration failure (no NVIDIA GPU at all);
// a non-RTX-50 GPU is a successful query with is_rtx50 == false.
bool QueryGpuInfo(int nvidia_index, GpuInfo* out, std::string* error);

// Human-readable architecture name for a raw NVML architecture value.
const char* NvmlArchName(int arch);

}  // namespace blender_dlss5::diagnostics
