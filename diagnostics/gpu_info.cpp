// SPDX-License-Identifier: MIT
#include "gpu_info.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace blender_dlss5::diagnostics {

namespace {

// ---- NVML (dynamic, driver-shipped nvml.dll) ------------------------------

struct nvmlDevice_st;
using nvmlDevice_t = nvmlDevice_st*;
using nvmlReturn_t = int;
// NVML_DEVICE_ARCH_* subset. Consumer Blackwell (GB20x) and the Blackwell
// family report values in this range; the raw value is also reported so a
// new value is visible on first run instead of silently failing policy.
enum : int {
    kArchBlackwell = 10,
    kArchBlackwellMin = 10,
    kArchBlackwellMax = 13,
};

using NvmlInitFn = nvmlReturn_t(*)();
using NvmlShutdownFn = nvmlReturn_t(*)();
using NvmlDriverVersionFn = nvmlReturn_t(*)(char*, unsigned int);
using NvmlCountFn = nvmlReturn_t(*)(unsigned int*);
using NvmlHandleFn = nvmlReturn_t(*)(unsigned int, nvmlDevice_t*);
using NvmlNameFn = nvmlReturn_t(*)(nvmlDevice_t, char*, unsigned int);
using NvmlArchFn = nvmlReturn_t(*)(nvmlDevice_t, int*);

struct Nvml {
    bool ok = false;
    HMODULE mod = nullptr;
    NvmlInitFn init = nullptr;
    NvmlShutdownFn shutdown = nullptr;
    NvmlDriverVersionFn driver_version = nullptr;
    NvmlCountFn count = nullptr;
    NvmlHandleFn handle = nullptr;
    NvmlNameFn name = nullptr;
    NvmlArchFn arch = nullptr;
};

Nvml LoadNvml() {
    Nvml n;
    n.mod = LoadLibraryW(L"nvml.dll");
    if (!n.mod) return n;
    // Names prefixed with _v2 resolve to the same symbol; plain names are the
    // documented stable entry points.
    n.init = reinterpret_cast<NvmlInitFn>(GetProcAddress(n.mod, "nvmlInit_v2"));
    n.shutdown = reinterpret_cast<NvmlShutdownFn>(GetProcAddress(n.mod, "nvmlShutdown"));
    n.driver_version = reinterpret_cast<NvmlDriverVersionFn>(GetProcAddress(n.mod, "nvmlSystemGetDriverVersion"));
    n.count = reinterpret_cast<NvmlCountFn>(GetProcAddress(n.mod, "nvmlDeviceGetCount_v2"));
    n.handle = reinterpret_cast<NvmlHandleFn>(GetProcAddress(n.mod, "nvmlDeviceGetHandleByIndex_v2"));
    n.name = reinterpret_cast<NvmlNameFn>(GetProcAddress(n.mod, "nvmlDeviceGetName"));
    n.arch = reinterpret_cast<NvmlArchFn>(GetProcAddress(n.mod, "nvmlDeviceGetArchitecture"));
    if (!n.init || !n.shutdown || !n.driver_version || !n.count || !n.handle || !n.name) {
        FreeLibrary(n.mod);
        n.mod = nullptr;
        return n;
    }
    n.ok = true;
    return n;
}

struct NvmlDeviceQuery {
    bool ok = false;
    std::string name;
    int arch = -1;
};

NvmlDeviceQuery QueryNvmlDevice(const Nvml& n, int nvidia_index, std::string* driver_version) {
    NvmlDeviceQuery q;
    if (!n.ok || n.init() != 0) return q;

    char dv[128] = {};
    if (n.driver_version(dv, sizeof(dv)) == 0 && driver_version) *driver_version = dv;

    unsigned int count = 0;
    if (n.count(&count) != 0 || static_cast<int>(count) <= nvidia_index || nvidia_index < 0) {
        n.shutdown();
        return q;
    }
    nvmlDevice_t dev = nullptr;
    if (n.handle(static_cast<unsigned int>(nvidia_index), &dev) != 0 || !dev) {
        n.shutdown();
        return q;
    }
    char name[256] = {};
    if (n.name(dev, name, sizeof(name)) == 0) q.name = name;
    if (n.arch) n.arch(dev, &q.arch);
    q.ok = true;
    n.shutdown();
    return q;
}

// ---- DXGI ----------------------------------------------------------------

bool QueryDxgiAdapter(int nvidia_index, GpuInfo* out, std::string* error) {
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        if (error) *error = "CreateDXGIFactory1 failed";
        return false;
    }
    int seen = 0;
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc))) continue;
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || desc.VendorId != 0x10DE) continue;
        if (seen++ != nvidia_index) continue;

        char utf8[512] = {};
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, utf8,
                            static_cast<int>(sizeof(utf8)), nullptr, nullptr);
        out->name = utf8;
        out->vendor_id = desc.VendorId;
        out->adapter_index = static_cast<int>(i);
        out->nvidia_index = nvidia_index;

        // Best-effort UMD driver version, used when NVML is unavailable.
        LARGE_INTEGER umd{};
        if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
            char vbuf[32] = {};
            std::snprintf(vbuf, sizeof(vbuf), "%lld.%lld",
                          umd.QuadPart >> 48, (umd.QuadPart >> 32) & 0xFFFF);
            out->driver_version = vbuf;
        }
        return true;
    }
    if (error) *error = "No NVIDIA adapter found at the requested index";
    return false;
}

bool NameContainsRtx50(const std::string& name) {
    return name.find("RTX 50") != std::string::npos;
}

}  // namespace

const char* NvmlArchName(int arch) {
    switch (arch) {
        case 10: return "Blackwell";
        case 11: return "Blackwell (GB200)";
        case 12: return "Blackwell (GB300)";
        case 13: return "Blackwell (GB10)";
        default: return "Unknown";
    }
}

bool QueryGpuInfo(int nvidia_index, GpuInfo* out, std::string* error) {
    if (!out) return false;
    *out = GpuInfo{};

    if (!QueryDxgiAdapter(nvidia_index, out, error)) return false;

    const Nvml n = LoadNvml();
    std::string nvml_driver;
    const NvmlDeviceQuery dev = QueryNvmlDevice(n, nvidia_index, &nvml_driver);
    out->nvml_ok = dev.ok;
    out->nvml_arch = dev.arch;
    if (dev.arch >= 0) out->arch_name = NvmlArchName(dev.arch);
    if (!nvml_driver.empty()) out->driver_version = nvml_driver;

    const bool name_says_rtx50 = NameContainsRtx50(out->name);

    if (dev.ok && dev.arch >= 0) {
        // Cross-check: the NVML device at this index must be the same GPU the
        // DXGI enumeration selected. If names disagree, the architecture
        // answer cannot be trusted for the device we will actually use.
        const bool same_device =
            dev.name.empty() || dev.name == out->name;
        if (!same_device) {
            out->family_detection_method = "NVML/DXGI conflict";
            out->is_rtx50 = false;
            return true;
        }
        if (dev.arch >= kArchBlackwellMin && dev.arch <= kArchBlackwellMax &&
            name_says_rtx50) {
            out->is_rtx50 = true;
            out->family_detection_method = "NVML";
        } else if (dev.arch < kArchBlackwellMin && name_says_rtx50) {
            // Name claims RTX 50 but the architecture says otherwise — treat
            // as a name/architecture mismatch and refuse (§2.2).
            out->is_rtx50 = false;
            out->family_detection_method = "NVML/DXGI conflict";
        } else {
            out->is_rtx50 = false;
            out->family_detection_method = "NVML";
        }
        return true;
    }

    // NVML unavailable: fall back to the DXGI description string.
    out->family_detection_method = "DXGI description";
    out->is_rtx50 = name_says_rtx50;
    if (out->driver_version.empty()) {
        // Best-effort UMD driver version from the adapter itself.
        out->driver_version = "unknown";
    }
    return true;
}

}  // namespace blender_dlss5::diagnostics
