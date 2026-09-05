// SPDX-License-Identifier: MIT
// backends/feature18/ngx_abi.h — raw NGX / Feature-18 ABI mirror.
//
// This header is the single source of truth for the undocumented NGX
// parameter object layout and the function-pointer signatures used to drive
// nvngx_dlssnr.dll directly. NOTHING in this header may be referenced from
// outside backends/feature18/ (requirements §7).
//
// Layout notes (verified against the working reference implementation,
// ComfyUI-DLSS5-NR v0.3.0):
//  * NGX_SUCCESS is 1, not 0.
//  * The NGXParameter vtable order below is load-bearing ABI. NGX invokes
//    Set/Get virtually with (name, typed value); reordering breaks silently.
//  * nvngx_dlssnr.dll is a "snippet" build: its Init_Ext expects
//    (app_id, path, device, FeatureCommonInfo*, version) — the last two
//    arguments swapped relative to the core signature. The caller shim
//    performs the reorder.

#pragma once

#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>

namespace blender_dlss5::backends::feature18 {

using NGXResult = int;
inline constexpr NGXResult NGX_SUCCESS = 1;

inline constexpr int kFeatureId = 18;
inline constexpr unsigned long long kAppId = 141959980ULL;
inline constexpr const char* kProjectId = "53f803cc-a12f-4d69-90d5-19b7599cad19";

struct NGXHandle {
    unsigned int Id;
};

// Minimal ABI-compatible interface used by the NVIDIA NGX parameter object.
struct NGXParameter {
    virtual void Set(const char*, unsigned long long) = 0;
    virtual void Set(const char*, float) = 0;
    virtual void Set(const char*, double) = 0;
    virtual void Set(const char*, unsigned int) = 0;
    virtual void Set(const char*, int) = 0;
    virtual void Set(const char*, ID3D11Resource*) = 0;
    virtual void Set(const char*, ID3D12Resource*) = 0;
    virtual void Set(const char*, void*) = 0;
    virtual NGXResult Get(const char*, unsigned long long*) const = 0;
    virtual NGXResult Get(const char*, float*) const = 0;
    virtual NGXResult Get(const char*, double*) const = 0;
    virtual NGXResult Get(const char*, unsigned int*) const = 0;
    virtual NGXResult Get(const char*, int*) const = 0;
    virtual NGXResult Get(const char*, ID3D11Resource**) const = 0;
    virtual NGXResult Get(const char*, ID3D12Resource**) const = 0;
    virtual NGXResult Get(const char*, void**) const = 0;
    virtual void Reset() = 0;
};

struct NGXPathListInfo {
    wchar_t const* const* Path;
    unsigned int Length;
};

enum NGXLoggingLevel {
    NGX_LOG_OFF = 0,
    NGX_LOG_ON = 1,
    NGX_LOG_VERBOSE = 2,
};

using NGXLogCallback = void(__cdecl*)(const char*, NGXLoggingLevel, int);

struct NGXLoggingInfo {
    NGXLoggingLevel LoggingLevel;
    NGXLogCallback Callback;
    void* UserData;
    bool DisableOtherLoggingSinks;
};

struct NGXFeatureCommonInfoInternal;
struct NGXFeatureCommonInfo {
    NGXPathListInfo PathListInfo;
    NGXFeatureCommonInfoInternal* InternalData;
    NGXLoggingInfo LoggingInfo;
};

// Core (_nvngx.dll) exports.
using InitExtFn = NGXResult(__cdecl*)(unsigned long long, const wchar_t*, ID3D12Device*, int, const void*);
using SnippetInitFn = NGXResult(__cdecl*)(unsigned long long, const wchar_t*, ID3D12Device*, const void*, int);
using InitProjectIdFn = NGXResult(__cdecl*)(const char*, int, const char*, const wchar_t*, ID3D12Device*, int, const void*);
using AllocParamsFn = NGXResult(__cdecl*)(NGXParameter**);
using CreateFeatureFn = NGXResult(__cdecl*)(ID3D12GraphicsCommandList*, int, NGXParameter*, NGXHandle**);
using EvaluateFeatureFn = NGXResult(__cdecl*)(ID3D12GraphicsCommandList*, const NGXHandle*, const NGXParameter*, void*);
using ReleaseFeatureFn = NGXResult(__cdecl*)(NGXHandle*);
using ShutdownFn = NGXResult(__cdecl*)();

// Caller-shim exports (dlss5nr_shim.dll). First parameter is the real
// function pointer inside nvngx_dlssnr.dll; the shim performs the actual
// CALL so the return address lives in the shim module.
using ShimInitFn = NGXResult(__cdecl*)(void*, unsigned long long, const wchar_t*, ID3D12Device*, int, const void*);
using ShimCreateFn = NGXResult(__cdecl*)(void*, ID3D12GraphicsCommandList*, int, NGXParameter*, NGXHandle**);
using ShimEvaluateFn = NGXResult(__cdecl*)(void*, ID3D12GraphicsCommandList*, const NGXHandle*, const NGXParameter*, void*);
using ShimReleaseFn = NGXResult(__cdecl*)(void*, NGXHandle*);

}  // namespace blender_dlss5::backends::feature18
