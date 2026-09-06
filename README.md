# Blender DLSS 5 Neural Rendering Compositor Add-on

**Unofficial, experimental Blender add-on for NVIDIA DLSS 5 Neural
Rendering (NGX feature 18).**

> **This project targets an undocumented / reverse-engineered experimental
> Neural Rendering interface ("Feature 18") and is not affiliated with,
> endorsed by, or supported by NVIDIA.** It is NOT the NVIDIA public DLSS 5
> Streamline API. Runtime behavior may change with NVIDIA driver or
> `nvngx_dlssnr.dll` versions. **RTX 50 Series only** — support for other
> GPU generations is explicitly out of scope by design.
>
> 本项目使用未公开的逆向实验接口（Feature 18），与 NVIDIA 无关联、未获认可，也
> 不是 NVIDIA 官方 DLSS 5 Streamline API。仅支持 RTX 50 系列。

## What it does

Runs NVIDIA DLSS 5 Neural Rendering on a single still image inside
Blender:

```
Blender Image / Render Result (Combined)
        -> ctypes bridge (nr_bridge.dll)
        -> canonical color frame (top-left RGBA float32)
        -> D3D12 + NGX + user-provided nvngx_dlssnr.dll (Feature 18)
        -> DLSS5_NR_Result Image datablock
```

Still-image mode (`Reset=1`), color-only input, same resolution, no
video/animation, no temporary files. The architecture isolates all
Feature-18-specific knowledge behind a replaceable backend
(`INeuralRenderingBackend`), so the official NVIDIA DLSS 5 Streamline API
can replace the experimental backend later without touching the Blender
layer.

## Requirements

- Windows 10/11 x64
- **NVIDIA GeForce RTX 50 Series GPU** (project policy: other generations
  are refused by design — there is no compatibility mode)
- Blender 5.x
- A **legally obtained, unmodified** `nvngx_dlssnr.dll` provided by the
  user (never downloaded, patched or redistributed by this project)
- MSVC (VS 2022+ with the C++ workload) + Windows SDK for building

## Project layout

```
canonical/        backend-independent frame model (no NGX knowledge)
backends/
  interface/      INeuralRenderingBackend contract, error taxonomy
  feature18/      all Feature-18 raw ABI code (parameter names, caller shim)
diagnostics/      GPU policy check, runtime identity (SHA-256/Authenticode)
native/           nr_bridge.dll C ABI (§15) for the Blender ctypes layer
probe/            standalone CLI probe (test image -> backend -> output + JSON report)
blender/          Blender Python layer (bridge/render_result/image_output/addon)
tests/            CPU-only unit tests
```

## Build

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1
```

Outputs in `bin\`: `dlss5nr_probe.exe`, `dlss5nr_unit_tests.exe`,
`nr_bridge.dll`, and the caller shim `nvngx.dll_dlss5.dll`.

The build script never downloads or copies NVIDIA DLLs.

## Usage

### Probe (standalone, no Blender)

```powershell
.\bin\dlss5nr_probe.exe --runtime <dir-with-nvngx_dlssnr.dll>
```

Writes the processed image, a raw float32 buffer and a JSON report with
GPU/driver/runtime identity, results and hashes. See `A1.md`.

### Blender add-on (A7)

1. Build and package:
   `powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Package`
   → `bin/blender_dlss5_addon.zip` (add-on code + native bridge/shim;
   never contains NVIDIA runtime files).
2. Blender 5.2+ → Preferences → Add-ons → Install from Disk → choose the
   ZIP → enable "Blender DLSS 5 Neural Rendering".
3. In the add-on preferences set the Runtime Directory (folder containing
   the user-provided `nvngx_dlssnr.dll`).
4. Render a frame, show the Compositing workspace once, then use the
   Image Editor sidebar (N) → "DLSS 5 NR" panel → Process Render Result.
   The result appears as the `DLSS5_NR_Result` float image datablock,
   with a diagnostics sub-panel (§16).

In Auto color-input mode, Render Result / Viewer follows the scene view
transform. A file-backed image (PNG/EXR, etc.) uses Blender's already
colorspace-decoded float buffer, so it is not display-mapped a second time.
Explicit Scene Linear / Standard / AgX choices remain available for testing
or deliberate overrides.

Details and acceptance records: `A2.md`, `A3.md`, `A7.md`.

## Runtime policy

- The `nvngx_dlssnr.dll` is **user-provided**. The project does not
  download, patch, or redistribute NVIDIA binaries, and does not bypass
  GPU-architecture checks.
- Runtime identity (file size, FileVersion, SHA-256, Authenticode) is
  reported. Known-modified runtimes are rejected; unsigned/unknown
  runtimes produce a warning. There is no legacy-GPU compatibility path —
  RTX 20/30/40 are unsupported by project policy.

## Legal / licensing

The project's original source code is MIT licensed. NVIDIA trademarks,
SDK/API names and proprietary binaries are owned by NVIDIA and are **not**
covered by this project's MIT license. This repository contains no NVIDIA
proprietary binaries or SDK headers.

The integration uses undocumented / reverse-engineered Neural Rendering
behavior, including observed parameter names and identifiers. Before
distributing or using this project, review the terms that apply to the
NVIDIA software/runtime you use. See
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

## Status

- **A1** (standalone Feature 18 probe): done and verified on RTX 5090.
- **A2** (Blender bridge integration): done and verified (headless +
  GUI). See `A2.md`.
- **A3** (SceneLinear / Standard / AgX color-domain experiment):
  accepted — product default input encoding is **AgX** (scene-driven
  selection, no overexposure on strong highlights). See `A3.md`.
- **A4** (alpha correctness): deferred by user decision — the workflow
  renders opaque frames only; the provisional alpha passthrough is a
  no-op for alpha=1 content. Transparent-film/silhouette validation
  stays an open item for when it is needed.
- Roadmap: A5 diagnostics UI, A6 policy validation, A7 UI / packaging;
  migration to the official NVIDIA Streamline API when it becomes
  public.

## References

- ComfyUI-DLSS5-NR: https://github.com/ComfyUI-DLSS5-NR/ComfyUI-DLSS5-NR
- DLSS 5 Visual Enhancer
- NVIDIA DLSS: https://github.com/NVIDIA/DLSS
