// SPDX-License-Identifier: MIT
// backends/feature18/feature18_backend.cpp — Feature 18 backend (NGX
// snippet runtime driven directly through D3D12).
//
// This is the ONLY file allowed to know Feature-18 raw ABI details: the
// DLSSNR.* parameter names, feature ID 18, app/project IDs and the caller
// shim (§7). The rest of the project sees only CanonicalColor and
// INeuralRenderingBackend.
//
// The D3D12/NGX call flow mirrors the verified reference implementation
// (ComfyUI-DLSS5-NR v0.3.0): committed RGBA16F textures, upload/readback
// staging buffers with a manually computed 256-byte-aligned row pitch,
// single DIRECT queue, fence waits with a 30 s timeout, still-image mode
// with Reset=1 and no motion/depth/mask resources.

#include "feature18_backend.h"

#include "half.h"
#include "ngx_abi.h"

#include "backends/interface/backend_registry.h"
#include "diagnostics/file_identity.h"
#include "diagnostics/gpu_info.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace blender_dlss5::backends::feature18 {

namespace {

using diagnostics::FileIdentity;
using diagnostics::GpuInfo;
using diagnostics::RuntimeClassification;
using diagnostics::SignatureStatus;

// ---- small helpers --------------------------------------------------------

std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

std::wstring Join(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    const wchar_t c = a.back();
    if (c == L'\\' || c == L'/') return a + b;
    return a + L"\\" + b;
}

bool FileExists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Directory of whichever module contains this code — the probe executable
// (A1, statically linked) or the bridge DLL (A2). The caller shim ships
// next to that module.
std::wstring ThisModuleDirectory() {
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&ThisModuleDirectory), &mod)) {
        return {};
    }
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(mod, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::wstring path(buf, n);
    const size_t pos = path.find_last_of(L"\\/");
    if (pos != std::wstring::npos) path.resize(pos);
    return path;
}

}  // namespace

// ---- Impl -----------------------------------------------------------------

struct Feature18Backend::Impl {
    // Options (§15 probe mapping).
    int gpu_index = 0;
    std::wstring runtime_dir;
    std::wstring ngx_core_path;
    bool reject_unsigned = false;
    // Clamp upload/readback RGB to [0,1]. Default ON — the A3 experiment
    // proved that feeding raw scene-linear HDR values (up to ~300x in real
    // renders) to the network causes local block artifacts; the runtime's
    // known-good domain is [0,1]. --no-clamp restores the HDR passthrough
    // for experiments only.
    bool clamp_input = true;

    // Identity / diagnostics.
    GpuInfo gpu;
    FileIdentity runtime_id;
    bool runtime_id_ok = false;
    BackendDiagnostics diag;
    bool initialized = false;

    // Loaded modules.
    HMODULE core_mod = nullptr;
    HMODULE nr_mod = nullptr;
    HMODULE shim_mod = nullptr;

    // NGX function pointers.
    InitExtFn core_init_ext = nullptr;
    InitProjectIdFn core_init_project = nullptr;
    AllocParamsFn alloc_params = nullptr;
    CreateFeatureFn core_create = nullptr;
    EvaluateFeatureFn core_eval = nullptr;
    ReleaseFeatureFn core_release = nullptr;
    ShutdownFn core_shutdown = nullptr;
    SnippetInitFn nr_init = nullptr;
    CreateFeatureFn nr_create = nullptr;
    EvaluateFeatureFn nr_eval = nullptr;
    ReleaseFeatureFn nr_release = nullptr;
    ShimInitFn shim_init = nullptr;
    ShimCreateFn shim_create = nullptr;
    ShimEvaluateFn shim_eval = nullptr;
    ShimReleaseFn shim_release = nullptr;

    // D3D12 objects.
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> cmd_alloc;
    ComPtr<ID3D12GraphicsCommandList> cmd;
    ComPtr<ID3D12Fence> fence;
    UINT64 fence_value = 0;

    // NGX objects and frame resources.
    NGXParameter* params = nullptr;
    NGXHandle* feature = nullptr;
    ComPtr<ID3D12Resource> color;
    ComPtr<ID3D12Resource> output;
    ComPtr<ID3D12Resource> upload;
    ComPtr<ID3D12Resource> readback;
    UINT width = 0, height = 0, row_pitch = 0;
    UINT64 total_bytes = 0;
    int feature_style = -999;
    int feature_preset = -999;

    std::mutex mutex;  // §26: serialize all evaluation on this context

    // ---- error plumbing (§25) -------------------------------------------

    void Fail(BackendError* err, BackendErrorCategory category,
              const char* stage, uint32_t raw, const char* fmt, ...) {
        if (!err) return;
        err->category = category;
        err->stage = stage;
        err->raw_code = raw;
        char buf[1024];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        err->message = buf;
        err->gpu_name = gpu.name;
        err->driver_version = gpu.driver_version;
        err->runtime_version = runtime_id.file_version;
        err->runtime_sha256 = runtime_id.sha256;
    }

    void FillRuntimeContext(BackendError* err) {
        if (!err) return;
        err->gpu_name = gpu.name;
        err->driver_version = gpu.driver_version;
        err->runtime_version = runtime_id.file_version;
        err->runtime_sha256 = runtime_id.sha256;
    }

    // ---- NGX core discovery ----------------------------------------------

    // Order: explicit override -> runtime_dir\_nvngx.dll -> normal loader
    // search -> DriverStore scan (newest package first).
    HMODULE LoadCoreNgx() {
        if (!ngx_core_path.empty()) {
            if (FileExists(ngx_core_path)) {
                if (HMODULE m = LoadLibraryW(ngx_core_path.c_str())) {
                    diag.ngx_core_path = WideToUtf8(ngx_core_path);
                    diag.ngx_core_discovery = "override";
                    return m;
                }
            }
        }

        const std::wstring local = Join(runtime_dir, L"_nvngx.dll");
        if (FileExists(local)) {
            if (HMODULE m = LoadLibraryW(local.c_str())) {
                diag.ngx_core_path = WideToUtf8(local);
                diag.ngx_core_discovery = "runtime_dir";
                return m;
            }
        }

        if (HMODULE m = LoadLibraryW(L"_nvngx.dll")) {
            diag.ngx_core_path = "_nvngx.dll";
            diag.ngx_core_discovery = "loader";
            return m;
        }

        // NVIDIA ships NGX core inside the active display-driver package in
        // DriverStore. The INF prefix is not always nv_dispi (OEM-dependent:
        // nvddi, nvaci, nvhmui, ...), so scan every NVIDIA-looking nv*.inf_*
        // package and prefer the newest.
        wchar_t windows_dir[MAX_PATH] = {};
        const UINT windows_len = GetWindowsDirectoryW(windows_dir, MAX_PATH);
        if (windows_len == 0 || windows_len >= MAX_PATH) return nullptr;
        const std::wstring repo =
            std::wstring(windows_dir) + L"\\System32\\DriverStore\\FileRepository";
        const std::wstring pattern = repo + L"\\nv*.inf_*";

        struct Candidate {
            std::wstring path;
            unsigned long long stamp;
            bool preferred;  // nv_dispi prefix first
        };
        std::vector<Candidate> candidates;

        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
                const std::wstring candidate = repo + L"\\" + fd.cFileName + L"\\_nvngx.dll";
                WIN32_FILE_ATTRIBUTE_DATA fad{};
                if (GetFileAttributesExW(candidate.c_str(), GetFileExInfoStandard, &fad) &&
                    !(fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    ULARGE_INTEGER u{};
                    u.LowPart = fad.ftLastWriteTime.dwLowDateTime;
                    u.HighPart = fad.ftLastWriteTime.dwHighDateTime;
                    candidates.push_back(
                        {candidate, u.QuadPart, wcsncmp(fd.cFileName, L"nv_dispi", 8) == 0});
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }

        std::sort(candidates.begin(), candidates.end(),
                  [](const Candidate& a, const Candidate& b) {
                      if (a.preferred != b.preferred) return a.preferred;
                      return a.stamp > b.stamp;
                  });

        for (const Candidate& c : candidates) {
            if (HMODULE m = LoadLibraryW(c.path.c_str())) {
                diag.ngx_core_path = WideToUtf8(c.path);
                diag.ngx_core_discovery = "driver_store";
                return m;
            }
        }
        return nullptr;
    }

    // ---- runtime + shim loading ------------------------------------------

    bool LoadRuntimeAndShim(BackendError* err) {
        const std::wstring nr_path = Join(runtime_dir, L"nvngx_dlssnr.dll");
        if (!FileExists(nr_path)) {
            Fail(err, BackendErrorCategory::RuntimeMissing, "runtime_identity",
                 static_cast<uint32_t>(ERROR_FILE_NOT_FOUND),
                 "nvngx_dlssnr.dll not found in runtime folder");
            return false;
        }
        nr_mod = LoadLibraryW(nr_path.c_str());
        if (!nr_mod) {
            Fail(err, BackendErrorCategory::RuntimeMissing, "runtime_identity",
                 static_cast<uint32_t>(GetLastError()),
                 "LoadLibrary(nvngx_dlssnr.dll) failed: Win32 %lu", GetLastError());
            return false;
        }

        // The caller shim ships next to the module that contains this
        // backend — the probe exe (A1) or the bridge DLL (A2). Module-name
        // requirement: the snippet runtime's caller validation (0xBAD00002)
        // accepts only modules whose name matches the NGX loader pattern
        // "nvngx*.dll"; "nvngx.dll_dlss5.dll" satisfies it without colliding
        // with the real app-side loader name "nvngx.dll".
        const std::wstring shim_path = Join(ThisModuleDirectory(), L"nvngx.dll_dlss5.dll");
        if (!FileExists(shim_path)) {
            Fail(err, BackendErrorCategory::Internal, "runtime_identity",
                 static_cast<uint32_t>(ERROR_FILE_NOT_FOUND),
                 "caller shim not found (expected nvngx.dll_dlss5.dll next to the executable)");
            return false;
        }
        shim_mod = LoadLibraryW(shim_path.c_str());
        if (!shim_mod) {
            Fail(err, BackendErrorCategory::Internal, "runtime_identity",
                 static_cast<uint32_t>(GetLastError()),
                 "LoadLibrary(caller shim) failed: Win32 %lu", GetLastError());
            return false;
        }
        diag.shim_path = WideToUtf8(shim_path);

        core_init_ext = reinterpret_cast<InitExtFn>(GetProcAddress(core_mod, "NVSDK_NGX_D3D12_Init_Ext"));
        core_init_project = reinterpret_cast<InitProjectIdFn>(GetProcAddress(core_mod, "NVSDK_NGX_D3D12_Init_ProjectID"));
        alloc_params = reinterpret_cast<AllocParamsFn>(GetProcAddress(core_mod, "NVSDK_NGX_D3D12_AllocateParameters"));
        core_create = reinterpret_cast<CreateFeatureFn>(GetProcAddress(core_mod, "NVSDK_NGX_D3D12_CreateFeature"));
        core_eval = reinterpret_cast<EvaluateFeatureFn>(GetProcAddress(core_mod, "NVSDK_NGX_D3D12_EvaluateFeature"));
        core_release = reinterpret_cast<ReleaseFeatureFn>(GetProcAddress(core_mod, "NVSDK_NGX_D3D12_ReleaseFeature"));
        core_shutdown = reinterpret_cast<ShutdownFn>(GetProcAddress(core_mod, "NVSDK_NGX_D3D12_Shutdown"));

        nr_init = reinterpret_cast<SnippetInitFn>(GetProcAddress(nr_mod, "NVSDK_NGX_D3D12_Init_Ext"));
        nr_create = reinterpret_cast<CreateFeatureFn>(GetProcAddress(nr_mod, "NVSDK_NGX_D3D12_CreateFeature"));
        nr_eval = reinterpret_cast<EvaluateFeatureFn>(GetProcAddress(nr_mod, "NVSDK_NGX_D3D12_EvaluateFeature"));
        nr_release = reinterpret_cast<ReleaseFeatureFn>(GetProcAddress(nr_mod, "NVSDK_NGX_D3D12_ReleaseFeature"));

        shim_init = reinterpret_cast<ShimInitFn>(GetProcAddress(shim_mod, "DLSSNR_CallInit"));
        shim_create = reinterpret_cast<ShimCreateFn>(GetProcAddress(shim_mod, "DLSSNR_CallCreate"));
        shim_eval = reinterpret_cast<ShimEvaluateFn>(GetProcAddress(shim_mod, "DLSSNR_CallEvaluate"));
        shim_release = reinterpret_cast<ShimReleaseFn>(GetProcAddress(shim_mod, "DLSSNR_CallRelease"));

        if (!core_init_ext || !alloc_params || !core_create || !core_eval || !core_release || !core_shutdown) {
            Fail(err, BackendErrorCategory::NgxCoreMissing, "runtime_identity", 0,
                 "Required NGX core exports are missing from _nvngx.dll");
            return false;
        }
        if (!nr_init || !nr_create || !nr_eval || !nr_release) {
            Fail(err, BackendErrorCategory::RuntimeMissing, "runtime_identity", 0,
                 "Required DLSSNR exports are missing from nvngx_dlssnr.dll");
            return false;
        }
        if (!shim_init || !shim_create || !shim_eval || !shim_release) {
            Fail(err, BackendErrorCategory::Internal, "runtime_identity", 0,
                 "Required caller shim exports are missing");
            return false;
        }
        return true;
    }

    // ---- D3D12 ------------------------------------------------------------

    bool SetupD3D12(BackendError* err) {
        ComPtr<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            Fail(err, BackendErrorCategory::D3D12InitFailure, "d3d12_init",
                 static_cast<uint32_t>(GetLastError()), "CreateDXGIFactory1 failed");
            return false;
        }

        int seen = 0;
        bool found = false;
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> a;
            if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 desc{};
            if (FAILED(a->GetDesc1(&desc))) continue;
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || desc.VendorId != 0x10DE) continue;
            if (seen++ != gpu_index) continue;

            ComPtr<ID3D12Device> d;
            if (SUCCEEDED(D3D12CreateDevice(a.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&d)))) {
                adapter = a;
                device = d;
                found = true;
                break;
            }
        }
        if (!found) {
            Fail(err, BackendErrorCategory::D3D12InitFailure, "d3d12_init", 0,
                 "Could not create a D3D12 device for NVIDIA GPU index %d", gpu_index);
            return false;
        }

        D3D12_COMMAND_QUEUE_DESC q{};
        q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)))) {
            Fail(err, BackendErrorCategory::D3D12InitFailure, "d3d12_init", 0, "CreateCommandQueue failed");
            return false;
        }
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmd_alloc)))) {
            Fail(err, BackendErrorCategory::D3D12InitFailure, "d3d12_init", 0, "CreateCommandAllocator failed");
            return false;
        }
        if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmd_alloc.Get(),
                                             nullptr, IID_PPV_ARGS(&cmd)))) {
            Fail(err, BackendErrorCategory::D3D12InitFailure, "d3d12_init", 0, "CreateCommandList failed");
            return false;
        }
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
            Fail(err, BackendErrorCategory::D3D12InitFailure, "d3d12_init", 0, "CreateFence failed");
            return false;
        }
        return true;
    }

    bool ExecuteAndWait(BackendError* err) {
        if (FAILED(cmd->Close())) {
            Fail(err, BackendErrorCategory::Internal, "evaluate",
                 static_cast<uint32_t>(GetLastError()), "CommandList::Close failed");
            return false;
        }
        ID3D12CommandList* lists[] = {cmd.Get()};
        queue->ExecuteCommandLists(1, lists);
        ++fence_value;
        if (FAILED(queue->Signal(fence.Get(), fence_value))) {
            Fail(err, BackendErrorCategory::Internal, "evaluate",
                 static_cast<uint32_t>(GetLastError()), "Queue::Signal failed");
            return false;
        }
        if (fence->GetCompletedValue() < fence_value) {
            HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!ev) {
                Fail(err, BackendErrorCategory::Internal, "evaluate",
                     static_cast<uint32_t>(GetLastError()), "CreateEvent failed");
                return false;
            }
            fence->SetEventOnCompletion(fence_value, ev);
            const DWORD w = WaitForSingleObject(ev, 30000);
            CloseHandle(ev);
            if (w != WAIT_OBJECT_0) {
                Fail(err, BackendErrorCategory::EvaluateFailure, "evaluate", 0,
                     "Timed out waiting for DLSS5 NR GPU work");
                return false;
            }
        }
        cmd_alloc->Reset();
        cmd->Reset(cmd_alloc.Get(), nullptr);
        return true;
    }

    void WaitQueueIdle() {
        if (!queue || !fence) return;
        ++fence_value;
        if (SUCCEEDED(queue->Signal(fence.Get(), fence_value)) &&
            fence->GetCompletedValue() < fence_value) {
            HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (ev) {
                fence->SetEventOnCompletion(fence_value, ev);
                WaitForSingleObject(ev, 30000);
                CloseHandle(ev);
            }
        }
    }

    static D3D12_RESOURCE_BARRIER Barrier(ID3D12Resource* r,
                                          D3D12_RESOURCE_STATES before,
                                          D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = r;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = after;
        return b;
    }

    ComPtr<ID3D12Resource> CreateTexture(UINT w, UINT h, DXGI_FORMAT format,
                                         D3D12_RESOURCE_STATES state,
                                         D3D12_RESOURCE_FLAGS flags) {
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = w;
        d.Height = h;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = format;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        d.Flags = flags;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> r;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr,
                                                   IID_PPV_ARGS(&r))))
            return nullptr;
        return r;
    }

    ComPtr<ID3D12Resource> CreateLinearBuffer(UINT64 bytes, D3D12_HEAP_TYPE type,
                                              D3D12_RESOURCE_STATES state) {
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = bytes;
        d.Height = 1;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = type;
        ComPtr<ID3D12Resource> r;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr,
                                                   IID_PPV_ARGS(&r))))
            return nullptr;
        return r;
    }

    // ---- NGX session ------------------------------------------------------

    bool InitNgxSession(BackendError* err) {
        const wchar_t* paths[1] = {runtime_dir.c_str()};
        NGXPathListInfo pli{paths, 1};
        NGXFeatureCommonInfo fci{};
        fci.PathListInfo = pli;
        fci.LoggingInfo.LoggingLevel = NGX_LOG_OFF;

        bool core_ok = false;
        if (core_init_project) {
            for (int ver = 0x13; ver <= 0x20 && !core_ok; ++ver) {
                const NGXResult r = core_init_project(kProjectId, 0, "0.3.0",
                                                      runtime_dir.c_str(), device.Get(), ver, nullptr);
                core_ok = (r == NGX_SUCCESS);
            }
        }
        if (!core_ok) {
            for (int ver = 0x13; ver <= 0x20 && !core_ok; ++ver) {
                const NGXResult r = core_init_ext(kAppId, runtime_dir.c_str(), device.Get(), ver, &fci);
                core_ok = (r == NGX_SUCCESS);
            }
        }
        if (!core_ok) {
            Fail(err, BackendErrorCategory::NgxInitFailure, "ngx_init", 0,
                 "NGX core initialization failed for API versions 0x13..0x20");
            return false;
        }

        const NGXResult sr = shim_init(reinterpret_cast<void*>(nr_init), kAppId,
                                       runtime_dir.c_str(), device.Get(), 0x15, &fci);
        if (sr != NGX_SUCCESS) {
            Fail(err, BackendErrorCategory::NgxInitFailure, "ngx_init", static_cast<uint32_t>(sr),
                 "DLSSNR snippet Init_Ext via caller shim failed: 0x%08X; shim=%s",
                 static_cast<unsigned>(sr), diag.shim_path.c_str());
            return false;
        }

        const NGXResult ar = alloc_params(&params);
        if (ar != NGX_SUCCESS || !params) {
            Fail(err, BackendErrorCategory::NgxInitFailure, "ngx_init", static_cast<uint32_t>(ar),
                 "NVSDK_NGX_D3D12_AllocateParameters failed: 0x%08X", static_cast<unsigned>(ar));
            return false;
        }
        return true;
    }

    // ---- frame resources + parameter block --------------------------------

    bool AllocateFrameResources(UINT w, UINT h, BackendError* err) {
        color = CreateTexture(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        output = CreateTexture(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT,
                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (!color || !output) {
            Fail(err, BackendErrorCategory::D3D12InitFailure, "feature_create", 0,
                 "Failed to create RGBA16F D3D12 textures");
            return false;
        }

        row_pitch = (w * 8u + 255u) & ~255u;
        total_bytes = static_cast<UINT64>(row_pitch) * h;
        upload = CreateLinearBuffer(total_bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        readback = CreateLinearBuffer(total_bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        if (!upload || !readback) {
            Fail(err, BackendErrorCategory::D3D12InitFailure, "feature_create", 0,
                 "Failed to create D3D12 upload/readback buffers");
            return false;
        }
        width = w;
        height = h;
        return true;
    }

    // The complete still-mode parameter block. Every DLSSNR.* string lives
    // here and nowhere else (§7, §12: Color/Output/Reset only, no MV binding).
    void SetParams(int style, int preset, float intensity, float tone,
                   float structure, float skin, int automask) {
        params->Set("DLSSNR.Width", width);
        params->Set("DLSSNR.Height", height);
        params->Set("DLSSNR.Enabled", 1);
        params->Set("DLSSNR.Reset", 1);
        params->Set("DLSSNR.Style", style);
        params->Set("DLSSNR.Hint.Render.Preset", preset);
        params->Set("DLSSNR.Intensity", intensity);
        params->Set("DLSSNR.LocalToneStrength", tone);
        params->Set("DLSSNR.LocalStructureStrength", structure);
        params->Set("DLSSNR.SkinStructureStrength", skin);
        params->Set("DLSSNR.UseAutoMask", automask);
        params->Set("DLSSNR.UICorrection", 0);
        params->Set("DLSSNR.DepthInverted", 1);
        params->Set("DLSSNR.ScalingRatio", 1.0f);
        params->Set("DLSSNR.Color", color.Get());
        params->Set("DLSSNR.Output", output.Get());
        params->Set("DLSSNR.Backbuffer", output.Get());
        params->Set("DLSSNR.ColorSubrectBaseX", 0);
        params->Set("DLSSNR.ColorSubrectBaseY", 0);
        params->Set("DLSSNR.ColorSubrectWidth", width);
        params->Set("DLSSNR.ColorSubrectHeight", height);
        params->Set("DLSSNR.OutputSubrectBaseX", 0);
        params->Set("DLSSNR.OutputSubrectBaseY", 0);
        params->Set("DLSSNR.OutputSubrectWidth", width);
        params->Set("DLSSNR.OutputSubrectHeight", height);
        // Still-image mode: no motion vectors (A1 color-only).
        params->Set("DLSSNR.MVec", static_cast<ID3D12Resource*>(nullptr));
        params->Set("DLSSNR.MVecScaleX", 1.0f);
        params->Set("DLSSNR.MVecScaleY", 1.0f);
        params->Set("DLSSNR.MVecSubrectBaseX", 0);
        params->Set("DLSSNR.MVecSubrectBaseY", 0);
        params->Set("DLSSNR.MVecSubrectWidth", 0);
        params->Set("DLSSNR.MVecSubrectHeight", 0);
    }

    bool EnsureFeature(UINT w, UINT h, const NeuralSettings& s, BackendError* err) {
        const bool rebuild = !feature || w != width || h != height ||
                             s.style != feature_style || s.preset != feature_preset;
        if (!rebuild) {
            SetParams(s.style, s.preset, s.intensity, s.local_tone_strength,
                      s.local_structure_strength, s.skin_structure_strength,
                      s.use_auto_mask ? 1 : 0);
            return true;
        }

        ReleaseFeatureAndResources();
        if (!AllocateFrameResources(w, h, err)) return false;
        SetParams(s.style, s.preset, s.intensity, s.local_tone_strength,
                  s.local_structure_strength, s.skin_structure_strength,
                  s.use_auto_mask ? 1 : 0);

        const NGXResult r =
            shim_create(reinterpret_cast<void*>(nr_create), cmd.Get(), kFeatureId, params, &feature);
        diag.create_feature_result = static_cast<int>(r);
        if (r != NGX_SUCCESS || !feature) {
            Fail(err, BackendErrorCategory::FeatureCreationFailure, "feature_create",
                 static_cast<uint32_t>(r),
                 "CreateFeature failed: 0x%08X. Check GPU support, driver, nvngx_dlssnr.dll, "
                 "and the caller shim (a mis-optimized shim triggers 0xBAD00002).",
                 static_cast<unsigned>(r));
            return false;
        }
        feature_style = s.style;
        feature_preset = s.preset;
        return true;
    }

    void ReleaseFeatureAndResources() {
        WaitQueueIdle();
        if (feature) {
            if (nr_release && shim_release) {
                shim_release(reinterpret_cast<void*>(nr_release), feature);
            } else if (core_release) {
                core_release(feature);
            }
            feature = nullptr;
        }
        // NGX parameter objects do not necessarily AddRef resources stored
        // in them; clear the resource slots before releasing the textures.
        if (params) params->Set("DLSSNR.MVec", static_cast<ID3D12Resource*>(nullptr));
        color.Reset();
        output.Reset();
        upload.Reset();
        readback.Reset();
        width = height = row_pitch = 0;
        total_bytes = 0;
        feature_style = -999;
        feature_preset = -999;
    }

    // ---- evaluation -------------------------------------------------------

    // The snippet runtime has been observed to return B,G,R,A channel order
    // with some builds. The canonical contract is RGB, so interpret the
    // raw slots by whichever reading is closer to the input (deterministic
    // low-frequency comparison, as in the reference implementation's
    // Python-side auto-detection).
    bool AutoDetectChannelOrder(const std::vector<float>& raw,
                                const canonical::CanonicalColor& in,
                                bool* swap_rb) {
        const size_t n = static_cast<size_t>(width) * height;
        constexpr size_t kStride = 17;
        double diff_rgb = 0.0, diff_bgr = 0.0;
        const float* src = in.rgba_f32.data();
        for (size_t i = 0; i < n; i += kStride) {
            const float r = raw[i * 4 + 0], g = raw[i * 4 + 1], b = raw[i * 4 + 2];
            const float ir = src[i * 4 + 0], ig = src[i * 4 + 1], ib = src[i * 4 + 2];
            diff_rgb += std::fabs(r - ir) + std::fabs(g - ig) + std::fabs(b - ib);
            diff_bgr += std::fabs(b - ir) + std::fabs(g - ig) + std::fabs(r - ib);
        }
        *swap_rb = diff_bgr < diff_rgb;
        return true;
    }
};

// ---- Feature18Backend -----------------------------------------------------

Feature18Backend::Feature18Backend() : impl_(std::make_unique<Impl>()) {
    impl_->diag.backend_name = name();
}

Feature18Backend::~Feature18Backend() {
    shutdown();
}

const char* Feature18Backend::name() const {
    return "feature18";
}

bool Feature18Backend::set_option(const char* key, const char* value) {
    if (!key || !value) return false;
    const std::string k(key), v(value);
    if (k == "gpu_index") {
        impl_->gpu_index = std::atoi(v.c_str());
        return true;
    }
    if (k == "runtime_dir") {
        impl_->runtime_dir = Utf8ToWide(v);
        return true;
    }
    if (k == "ngx_core_path") {
        impl_->ngx_core_path = Utf8ToWide(v);
        return true;
    }
    if (k == "reject_unsigned") {
        impl_->reject_unsigned = (v == "1" || v == "true");
        return true;
    }
    if (k == "clamp_input") {
        impl_->clamp_input = (v == "1" || v == "true");
        return true;
    }
    return false;
}

bool Feature18Backend::initialize(BackendError* err) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (err) *err = BackendError{};
    if (impl_->initialized) return true;

    // COM may be needed by downstream components; a changed mode is harmless.
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    (void)co;

    // §10: GPU policy gate first. RTX 50 is necessary but not sufficient —
    // the real capability check (D3D12 -> NGX -> CreateFeature) follows.
    std::string gpu_error;
    if (!diagnostics::QueryGpuInfo(impl_->gpu_index, &impl_->gpu, &gpu_error)) {
        impl_->Fail(err, BackendErrorCategory::UnsupportedGpu, "gpu_check", 0,
                    "GPU enumeration failed: %s", gpu_error.c_str());
        return false;
    }
    BackendDiagnostics& d = impl_->diag;
    d.gpu_name = impl_->gpu.name;
    d.vendor_id = impl_->gpu.vendor_id;
    d.adapter_index = impl_->gpu.adapter_index;
    d.nvidia_index = impl_->gpu.nvidia_index;
    d.architecture = impl_->gpu.arch_name;
    d.nvml_arch_raw = impl_->gpu.nvml_arch;
    d.rtx50_policy_ok = impl_->gpu.is_rtx50;
    d.family_detection_method = impl_->gpu.family_detection_method;
    d.driver_version = impl_->gpu.driver_version;

    if (!impl_->gpu.is_rtx50) {
        impl_->Fail(err, BackendErrorCategory::UnsupportedGpu, "gpu_check", 0,
                    "Unsupported GPU. This experimental backend only supports "
                    "NVIDIA GeForce RTX 50 Series.");
        return false;
    }

    // §16/§17: runtime identity. Never silently accept an unknown binary.
    if (impl_->runtime_dir.empty()) {
        impl_->Fail(err, BackendErrorCategory::RuntimeMissing, "runtime_identity", 0,
                    "runtime_dir is not set");
        return false;
    }
    const std::wstring nr_path = Join(impl_->runtime_dir, L"nvngx_dlssnr.dll");
    std::string id_error;
    if (!diagnostics::ComputeFileIdentity(nr_path, &impl_->runtime_id, &id_error)) {
        impl_->Fail(err, BackendErrorCategory::RuntimeMissing, "runtime_identity",
                    static_cast<uint32_t>(GetLastError()), "%s", id_error.c_str());
        return false;
    }
    impl_->runtime_id_ok = true;
    d.runtime_path = WideToUtf8(nr_path);
    d.runtime_size = impl_->runtime_id.size;
    d.runtime_file_version = impl_->runtime_id.file_version;
    d.runtime_sha256 = impl_->runtime_id.sha256;
    d.runtime_signature = diagnostics::to_string(impl_->runtime_id.signature);
    d.runtime_signer = impl_->runtime_id.signer_subject;
    d.runtime_classification = diagnostics::to_string(impl_->runtime_id.classification);

    if (impl_->runtime_id.classification == RuntimeClassification::KnownModified) {
        impl_->Fail(err, BackendErrorCategory::ModifiedRuntimeRejected, "runtime_identity", 0,
                    "This project intentionally supports only the native RTX 50 DLSS 5 "
                    "runtime path.");
        return false;
    }
    if (impl_->runtime_id.signature == SignatureStatus::Invalid && impl_->reject_unsigned) {
        impl_->Fail(err, BackendErrorCategory::InvalidAuthenticode, "runtime_identity", 0,
                    "Runtime Authenticode signature is invalid and --reject-unsigned is set.");
        return false;
    }

    // D3D12 -> NGX core -> snippet init (§10 capability chain).
    if (!impl_->SetupD3D12(err)) return false;
    impl_->core_mod = impl_->LoadCoreNgx();
    if (!impl_->core_mod) {
        impl_->Fail(err, BackendErrorCategory::NgxCoreMissing, "ngx_core", 0,
                    "Could not load NVIDIA NGX core _nvngx.dll. Tried: explicit override, "
                    "runtime\\_nvngx.dll, normal DLL search, and NVIDIA DriverStore packages "
                    "matching nv*.inf_*.");
        return false;
    }
    if (!impl_->LoadRuntimeAndShim(err)) return false;
    if (!impl_->InitNgxSession(err)) return false;

    impl_->initialized = true;
    return true;
}

bool Feature18Backend::process(const canonical::CanonicalColor& in,
                               canonical::CanonicalColor& out,
                               const NeuralSettings& settings,
                               BackendError* err) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (err) *err = BackendError{};
    if (!impl_->initialized) {
        impl_->Fail(err, BackendErrorCategory::Internal, "evaluate", 0,
                    "Backend is not initialized");
        return false;
    }
    if (!in.valid()) {
        impl_->Fail(err, BackendErrorCategory::InvalidOutput, "evaluate", 0,
                    "Invalid input frame");
        return false;
    }
    const UINT w = in.width, h = in.height;
    const int style = settings.style;
    const int preset = settings.preset;
    const float intensity = settings.intensity;
    const float tone = settings.local_tone_strength;
    const float structure = settings.local_structure_strength;
    const float skin = settings.skin_structure_strength;
    const int automask = settings.use_auto_mask ? 1 : 0;

    if (!impl_->EnsureFeature(w, h, settings, err)) return false;
    impl_->SetParams(style, preset, intensity, tone, structure, skin, automask);

    // Upload: f32 -> RGBA16F. Alpha is forced to 1 (A4 owns alpha
    // semantics). RGB clamping is controlled by the clamp_input option
    // (A3: off by default so scene-linear HDR values pass through).
    void* mapped = nullptr;
    HRESULT hr = impl_->upload->Map(0, nullptr, &mapped);
    if (FAILED(hr) || !mapped) {
        impl_->Fail(err, BackendErrorCategory::Internal, "evaluate",
                    static_cast<uint32_t>(hr), "Upload buffer Map failed: 0x%08X",
                    static_cast<unsigned>(hr));
        return false;
    }
    std::memset(mapped, 0, static_cast<size_t>(impl_->total_bytes));
    auto* dst_base = static_cast<uint8_t*>(mapped);
    const float* src = in.rgba_f32.data();
    for (UINT y = 0; y < h; ++y) {
        auto* row = reinterpret_cast<uint16_t*>(dst_base + static_cast<size_t>(y) * impl_->row_pitch);
        for (UINT x = 0; x < w; ++x) {
            const size_t i = (static_cast<size_t>(y) * w + x) * 4;
            const float r = src[i + 0], g = src[i + 1], b = src[i + 2];
            if (impl_->clamp_input) {
                row[x * 4 + 0] = FloatToHalf(std::clamp(r, 0.0f, 1.0f));
                row[x * 4 + 1] = FloatToHalf(std::clamp(g, 0.0f, 1.0f));
                row[x * 4 + 2] = FloatToHalf(std::clamp(b, 0.0f, 1.0f));
            } else {
                row[x * 4 + 0] = FloatToHalf(r);
                row[x * 4 + 1] = FloatToHalf(g);
                row[x * 4 + 2] = FloatToHalf(b);
            }
            row[x * 4 + 3] = FloatToHalf(1.0f);
        }
    }
    impl_->upload->Unmap(0, nullptr);

    {
        auto b1 = Impl::Barrier(impl_->color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                D3D12_RESOURCE_STATE_COPY_DEST);
        impl_->cmd->ResourceBarrier(1, &b1);
        D3D12_TEXTURE_COPY_LOCATION dst_loc{};
        dst_loc.pResource = impl_->color.Get();
        dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION src_loc{};
        src_loc.pResource = impl_->upload.Get();
        src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src_loc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        src_loc.PlacedFootprint.Footprint.Width = w;
        src_loc.PlacedFootprint.Footprint.Height = h;
        src_loc.PlacedFootprint.Footprint.Depth = 1;
        src_loc.PlacedFootprint.Footprint.RowPitch = impl_->row_pitch;
        impl_->cmd->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);
        auto b2 = Impl::Barrier(impl_->color.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        impl_->cmd->ResourceBarrier(1, &b2);
    }

    const NGXResult er = impl_->shim_eval(reinterpret_cast<void*>(impl_->nr_eval),
                                          impl_->cmd.Get(), impl_->feature,
                                          impl_->params, nullptr);
    impl_->diag.last_evaluate_result = static_cast<int>(er);
    if (er != NGX_SUCCESS) {
        impl_->Fail(err, BackendErrorCategory::EvaluateFailure, "evaluate",
                    static_cast<uint32_t>(er), "DLSSNR EvaluateFeature failed: 0x%08X",
                    static_cast<unsigned>(er));
        // Reset the command list into a clean state before returning.
        BackendError dummy;
        impl_->ExecuteAndWait(&dummy);
        return false;
    }

    {
        auto b3 = Impl::Barrier(impl_->output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                D3D12_RESOURCE_STATE_COPY_SOURCE);
        impl_->cmd->ResourceBarrier(1, &b3);
        D3D12_TEXTURE_COPY_LOCATION dst_loc{};
        dst_loc.pResource = impl_->readback.Get();
        dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst_loc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        dst_loc.PlacedFootprint.Footprint.Width = w;
        dst_loc.PlacedFootprint.Footprint.Height = h;
        dst_loc.PlacedFootprint.Footprint.Depth = 1;
        dst_loc.PlacedFootprint.Footprint.RowPitch = impl_->row_pitch;
        D3D12_TEXTURE_COPY_LOCATION src_loc{};
        src_loc.pResource = impl_->output.Get();
        src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        impl_->cmd->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);
        auto b4 = Impl::Barrier(impl_->output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        impl_->cmd->ResourceBarrier(1, &b4);
    }

    if (!impl_->ExecuteAndWait(err)) {
        // ExecuteAndWait filled err with the failure detail; re-tag the stage
        // so the report shows this as a readback-stage failure.
        if (err) err->stage = "readback";
        return false;
    }

    void* rmap = nullptr;
    hr = impl_->readback->Map(0, nullptr, &rmap);
    if (FAILED(hr) || !rmap) {
        impl_->Fail(err, BackendErrorCategory::ReadbackFailure, "readback",
                    static_cast<uint32_t>(hr), "Readback Map failed: 0x%08X",
                    static_cast<unsigned>(hr));
        return false;
    }
    const size_t n = static_cast<size_t>(w) * h;
    std::vector<float> raw(n * 4);
    const auto* base = static_cast<const uint8_t*>(rmap);
    for (UINT y = 0; y < h; ++y) {
        const auto* row = reinterpret_cast<const uint16_t*>(base + static_cast<size_t>(y) * impl_->row_pitch);
        for (UINT x = 0; x < w; ++x) {
            const size_t i = (static_cast<size_t>(y) * w + x) * 4;
            raw[i + 0] = HalfToFloat(row[x * 4 + 0]);
            raw[i + 1] = HalfToFloat(row[x * 4 + 1]);
            raw[i + 2] = HalfToFloat(row[x * 4 + 2]);
            raw[i + 3] = HalfToFloat(row[x * 4 + 3]);
        }
    }
    impl_->readback->Unmap(0, nullptr);

    bool swap_rb = false;
    impl_->AutoDetectChannelOrder(raw, in, &swap_rb);

    out = canonical::CanonicalColor::zeros(w, h, in.encoding);
    out.premultiplied_alpha = in.premultiplied_alpha;
    for (size_t i = 0; i < n; ++i) {
        const size_t p = i * 4;
        const float v0 = swap_rb ? raw[p + 2] : raw[p + 0];
        const float v1 = raw[p + 1];
        const float v2 = swap_rb ? raw[p + 0] : raw[p + 2];
        if (impl_->clamp_input) {
            out.rgba_f32[p + 0] = std::clamp(v0, 0.0f, 1.0f);
            out.rgba_f32[p + 1] = std::clamp(v1, 0.0f, 1.0f);
            out.rgba_f32[p + 2] = std::clamp(v2, 0.0f, 1.0f);
        } else {
            out.rgba_f32[p + 0] = v0;
            out.rgba_f32[p + 1] = v1;
            out.rgba_f32[p + 2] = v2;
        }
        // §14 (provisional): the network output alpha is not meaningful in
        // A1; preserve the input alpha. Full alpha semantics are A4.
        out.rgba_f32[p + 3] = in.rgba_f32[p + 3];
    }
    return true;
}

void Feature18Backend::shutdown() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->initialized && !impl_->device) return;
    impl_->ReleaseFeatureAndResources();
    if (impl_->core_shutdown) impl_->core_shutdown();
    impl_->params = nullptr;
    impl_->device.Reset();
    impl_->adapter.Reset();
    impl_->queue.Reset();
    impl_->cmd_alloc.Reset();
    impl_->cmd.Reset();
    impl_->fence.Reset();
    if (impl_->shim_mod) FreeLibrary(impl_->shim_mod);
    // NOTE: the NGX modules (nvngx_dlssnr.dll, _nvngx.dll) are intentionally
    // NOT unloaded. Measured: FreeLibrary(nvngx_dlssnr.dll) after a
    // successful Evaluate blocks forever inside its DLL_PROCESS_DETACH,
    // hanging the host process. Both the probe CLI and the Blender add-on
    // hold the runtime for the process lifetime, so keeping the modules
    // loaded is safe; revisit if an official API ever provides a clean
    // unload path.
    impl_->shim_mod = impl_->nr_mod = impl_->core_mod = nullptr;
    impl_->initialized = false;
}

const BackendDiagnostics& Feature18Backend::diagnostics() const {
    return impl_->diag;
}

// Self-registration. The literal backend name lives only in this directory.
namespace {
const bool s_registered = RegisterBackend("feature18",
    []() -> INeuralRenderingBackend* { return new Feature18Backend(); });
}  // namespace

}  // namespace blender_dlss5::backends::feature18
