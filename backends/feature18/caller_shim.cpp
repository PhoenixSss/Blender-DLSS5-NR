// SPDX-License-Identifier: MIT
// backends/feature18/caller_shim.cpp — caller-module shim for the DLSSNR
// snippet runtime (builds to dlss5nr_shim.dll).
//
// Two load-bearing mechanisms, both verified against the reference
// implementation (ComfyUI-DLSS5-NR caller_shim.cpp):
//
// 1. Init_Ext ABI reorder: nvngx_dlssnr.dll is a snippet build whose
//    Init_Ext signature is (app_id, path, device, FeatureCommonInfo*, version)
//    — the last two arguments swapped versus the core NGX signature. The
//    exported shim keeps the bridge-facing order (version, common_info) and
//    reorders for the real call.
//
// 2. Return-address validation: the NR runtime validates the module that
//    owns its RETURN ADDRESS. A wrapper compiled with /O2 can be
//    tail-call-optimized into a JMP, leaving the return address in the
//    caller's module and triggering NGX error 0xBAD00002. Every wrapper here
//    is __declspec(noinline) and performs an observable post-call volatile
//    store, forcing a real CALL/RET through this DLL.
//
// BUILD REQUIREMENT: this file MUST be compiled with /Od (no optimization).
// build.ps1 asserts the flag and refuses to build without it.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>

namespace blender_dlss5::backends::feature18 {

using NGXResult = int;

struct NGXHandle { unsigned int Id; };
struct NGXParameter;

using SnippetInitFn = NGXResult(__cdecl*)(unsigned long long, const wchar_t*, ID3D12Device*, const void*, int);
using CreateFn = NGXResult(__cdecl*)(ID3D12GraphicsCommandList*, int, NGXParameter*, NGXHandle**);
using EvalFn = NGXResult(__cdecl*)(ID3D12GraphicsCommandList*, const NGXHandle*, const NGXParameter*, void*);
using ReleaseFn = NGXResult(__cdecl*)(NGXHandle*);

}  // namespace blender_dlss5::backends::feature18

using namespace blender_dlss5::backends::feature18;

// The observable post-call store. Even a misconfigured build that ignores
// /Od cannot tail-call through this because the store must execute after
// the wrapped call returns.
static volatile LONG g_post_call_sink = 0;
static __forceinline NGXResult FinishCall(NGXResult r) {
    g_post_call_sink = static_cast<LONG>(r);
    return r;
}

extern "C" {

__declspec(dllexport) __declspec(noinline) NGXResult __cdecl DLSSNR_CallInit(
    void* real_fn, unsigned long long app_id, const wchar_t* path,
    ID3D12Device* device, int version, const void* common_info) {
    NGXResult r = reinterpret_cast<SnippetInitFn>(real_fn)(app_id, path, device, common_info, version);
    return FinishCall(r);
}

__declspec(dllexport) __declspec(noinline) NGXResult __cdecl DLSSNR_CallCreate(
    void* real_fn, ID3D12GraphicsCommandList* list, int feature_id,
    NGXParameter* params, NGXHandle** handle) {
    NGXResult r = reinterpret_cast<CreateFn>(real_fn)(list, feature_id, params, handle);
    return FinishCall(r);
}

__declspec(dllexport) __declspec(noinline) NGXResult __cdecl DLSSNR_CallEvaluate(
    void* real_fn, ID3D12GraphicsCommandList* list, const NGXHandle* handle,
    const NGXParameter* params, void* callback) {
    NGXResult r = reinterpret_cast<EvalFn>(real_fn)(list, handle, params, callback);
    return FinishCall(r);
}

__declspec(dllexport) __declspec(noinline) NGXResult __cdecl DLSSNR_CallRelease(
    void* real_fn, NGXHandle* handle) {
    NGXResult r = reinterpret_cast<ReleaseFn>(real_fn)(handle);
    return FinishCall(r);
}

}  // extern "C"
