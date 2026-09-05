// SPDX-License-Identifier: MIT
// native/nr_bridge.h — C ABI of the neural-rendering bridge DLL (§15).
//
// This header is the Python-facing contract. It must stay free of any
// backend-specific ABI detail; the category enum mirrors the backend
// interface's error taxonomy (§25). All strings are UTF-8.

#ifndef NR_BRIDGE_H
#define NR_BRIDGE_H

#include <stdint.h>

#ifdef NR_BRIDGE_EXPORTS
#define NR_API __declspec(dllexport)
#else
#define NR_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NR_Context NR_Context;

// Canonical frame: top-left origin, row-major, RGBA float32 (requirements
// §13/§24 — the Blender layer converts its own conventions to this form).
typedef struct NR_FrameDesc {
    uint32_t width;
    uint32_t height;
    const float* rgba_f32;  // 4 * width * height floats
    int reset;              // still-image mode: 1
} NR_FrameDesc;

// Defaults reproduce the verified still-image working set.
typedef struct NR_Settings {
    int style;
    int preset;
    float intensity;
    float local_tone_strength;
    float local_structure_strength;
    float skin_structure_strength;
    int use_auto_mask;
} NR_Settings;

typedef struct NR_Options {
    const char* runtime_dir;    // dir containing the user-provided runtime DLL
    const char* ngx_core_path;  // explicit core override; NULL = auto-discovery
    int gpu_index;              // NVIDIA adapter index
    int reject_unsigned;        // 1 = reject invalid Authenticode
} NR_Options;

// Error taxonomy values (§25; matches the backend interface enum order,
// exit codes are 10 + category).
enum {
    NR_CATEGORY_NONE = 0,
    NR_CATEGORY_UNSUPPORTED_GPU = 1,
    NR_CATEGORY_MODIFIED_RUNTIME_REJECTED = 2,
    NR_CATEGORY_RUNTIME_MISSING = 3,
    NR_CATEGORY_INVALID_AUTHENTICODE = 4,
    NR_CATEGORY_NGX_CORE_MISSING = 5,
    NR_CATEGORY_D3D12_INIT_FAILURE = 6,
    NR_CATEGORY_NGX_INIT_FAILURE = 7,
    NR_CATEGORY_FEATURE_CREATION_FAILURE = 8,
    NR_CATEGORY_EVALUATE_FAILURE = 9,
    NR_CATEGORY_READBACK_FAILURE = 10,
    NR_CATEGORY_INVALID_OUTPUT = 11,
    NR_CATEGORY_INTERNAL = 12
};

typedef struct NR_Error {
    int category;             // NR_CATEGORY_*
    char stage[40];
    uint32_t raw_code;
    char message[1024];
    char gpu_name[128];
    char driver_version[64];
    char runtime_version[64];
    char runtime_sha256[72];
} NR_Error;

// Runtime info snapshot (§16/§17).
typedef struct NR_Diagnostics {
    char backend_name[32];
    char gpu_name[128];
    uint32_t vendor_id;
    int adapter_index;
    int nvidia_index;
    char architecture[32];
    int nvml_arch_raw;
    int rtx50_policy_ok;
    char family_detection_method[32];
    char driver_version[64];
    char runtime_path[1024];
    uint64_t runtime_size;
    char runtime_file_version[64];
    char runtime_sha256[72];
    char runtime_signature[32];
    char runtime_signer[512];
    char runtime_classification[32];
    char ngx_core_path[1024];
    char ngx_core_discovery[32];
    char shim_path[1024];
    int create_feature_result;
    int last_evaluate_result;
} NR_Diagnostics;

// API. All functions return 1 on success, 0 on failure (nr_create/
// nr_initialize/nr_evaluate fill `err` on failure). Errors are written as
// fully NUL-terminated strings; pass NULL err to discard.

NR_API const char* nr_version(void);

// Creates a context and applies options. Does not touch the GPU.
NR_API int nr_create(NR_Context** out, const NR_Options* opts, NR_Error* err);

// GPU policy gate, runtime identity check, D3D12 + NGX initialization.
// Safe to call multiple times (idempotent once initialized).
NR_API int nr_initialize(NR_Context* ctx, NR_Error* err);

// Still-image evaluation: Reset=true, same resolution, color only.
// `out_rgba_f32` must hold 4 * width * height floats; output is written
// in the same canonical top-left RGBA form as the input.
NR_API int nr_evaluate(NR_Context* ctx, const NR_FrameDesc* frame,
                       const NR_Settings* settings, float* out_rgba_f32,
                       NR_Error* err);

// Copies the diagnostics snapshot into `out` (strings NUL-terminated).
NR_API int nr_get_diagnostics(NR_Context* ctx, NR_Diagnostics* out);

// Releases the context. Never unloads the NGX runtime modules (see
// backend implementation note); safe to call on NULL.
NR_API void nr_destroy(NR_Context* ctx);

#ifdef __cplusplus
}
#endif

#endif  // NR_BRIDGE_H
