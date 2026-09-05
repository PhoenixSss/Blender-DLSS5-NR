# Third-Party Notices

This file distinguishes the project's MIT-licensed code from third-party
technology, names and works referenced by the integration.

## NVIDIA

NVIDIA, DLSS, NGX, GeForce and related names/trademarks are property of
NVIDIA Corporation and/or its affiliates.

This repository does **not** include NVIDIA proprietary runtime binaries or
NVIDIA NGX SDK headers. In particular, it does not include `_nvngx.dll` or
`nvngx_dlssnr.dll`. The runtime is **user-provided** and is never
downloaded, patched or redistributed by this project.

The native code declares only the minimum ABI shapes and function
signatures needed to dynamically call a user-supplied NGX runtime, plus
parameter names and identifiers observed from reverse engineering of that
runtime ("feature 18"). Those interfaces and identifiers correspond to
NVIDIA technology; this project's MIT license does not grant rights in
NVIDIA software, APIs, trademarks or binaries.

The integration targets an undocumented / pre-release Neural Rendering
interface and is not affiliated with, endorsed by, or supported by NVIDIA.
Runtime behavior may change with NVIDIA driver or runtime versions.

Reference: https://github.com/NVIDIA/DLSS

## stb

The following public-domain single-header libraries are vendored under
`third_party/`:

- `stb_image.h` / `stb_image_write.h`
  - Copyright (c) 2017 Sean Barrett
  - Public domain / MIT — https://github.com/nothings/stb

## ComfyUI-DLSS5-NR

The D3D12/NGX bridge architecture, the NGX parameter-object ABI layout,
the caller-shim mechanism (return-address validation and `Init_Ext`
argument reorder), still-image `Reset` handling and the RGBA16F/RG16F
resource conventions were informed by the MIT-licensed
`ComfyUI-DLSS5-NR` project. No source files from that project are vendored
or redistributed.

MIT License — Copyright (c) 2026 ComfyUI-DLSS5-NR contributors

Reference: https://github.com/ComfyUI-DLSS5-NR/ComfyUI-DLSS5-NR

## DLSS 5 Visual Enhancer

The single-image processing workflow, parameter organization, runtime
diagnostics and failure-handling patterns were informed by the
MIT-licensed `DLSS 5 Visual Enhancer` project. No source files from that
project are vendored or redistributed.

MIT License — Copyright (c) 2026 Merserk

## Blender

This is an unofficial Blender add-on project and is not affiliated with,
endorsed by, or supported by the Blender Foundation.

Reference: https://www.blender.org
