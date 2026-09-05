# SPDX-License-Identifier: MIT
# blender/bridge.py — ctypes wrapper around nr_bridge.dll (§15 C ABI).
#
# The bridge DLL contains the registered neural-rendering backend; this
# module is the only place that talks to it. All pixel data crosses this
# boundary in the canonical top-left RGBA float32 form.

import ctypes
import os

# ---------------------------------------------------------------------------
# DLL discovery
# ---------------------------------------------------------------------------

def find_bridge_dll():
    """Locates nr_bridge.dll: DLSS5NR_NATIVE_DIR env override first, then
    the project bin/ directory relative to this file."""
    candidates = []
    env = os.environ.get("DLSS5NR_NATIVE_DIR")
    if env:
        candidates.append(os.path.join(env, "nr_bridge.dll"))
    here = os.path.dirname(os.path.abspath(__file__))
    candidates.append(os.path.normpath(os.path.join(here, "..", "bin", "nr_bridge.dll")))
    for path in candidates:
        if os.path.isfile(path):
            return path
    raise FileNotFoundError(
        "nr_bridge.dll not found. Tried: " + "; ".join(candidates) +
        " (set DLSS5NR_NATIVE_DIR to the directory containing it)")


_lib = ctypes.CDLL(find_bridge_dll())


# ---------------------------------------------------------------------------
# C ABI mirror (must match native/nr_bridge.h exactly)
# ---------------------------------------------------------------------------

class NR_Options(ctypes.Structure):
    _fields_ = [
        ("runtime_dir", ctypes.c_char_p),     # UTF-8
        ("ngx_core_path", ctypes.c_char_p),   # UTF-8, may be None
        ("gpu_index", ctypes.c_int),
        ("reject_unsigned", ctypes.c_int),
    ]


class NR_FrameDesc(ctypes.Structure):
    _fields_ = [
        ("width", ctypes.c_uint32),
        ("height", ctypes.c_uint32),
        ("rgba_f32", ctypes.POINTER(ctypes.c_float)),
        ("reset", ctypes.c_int),
    ]


class NR_Settings(ctypes.Structure):
    _fields_ = [
        ("style", ctypes.c_int),
        ("preset", ctypes.c_int),
        ("intensity", ctypes.c_float),
        ("local_tone_strength", ctypes.c_float),
        ("local_structure_strength", ctypes.c_float),
        ("skin_structure_strength", ctypes.c_float),
        ("use_auto_mask", ctypes.c_int),
    ]


class NR_Error(ctypes.Structure):
    _fields_ = [
        ("category", ctypes.c_int),
        ("stage", ctypes.c_char * 40),
        ("raw_code", ctypes.c_uint32),
        ("message", ctypes.c_char * 1024),
        ("gpu_name", ctypes.c_char * 128),
        ("driver_version", ctypes.c_char * 64),
        ("runtime_version", ctypes.c_char * 64),
        ("runtime_sha256", ctypes.c_char * 72),
    ]


class NR_Diagnostics(ctypes.Structure):
    _fields_ = [
        ("backend_name", ctypes.c_char * 32),
        ("gpu_name", ctypes.c_char * 128),
        ("vendor_id", ctypes.c_uint32),
        ("adapter_index", ctypes.c_int),
        ("nvidia_index", ctypes.c_int),
        ("architecture", ctypes.c_char * 32),
        ("nvml_arch_raw", ctypes.c_int),
        ("rtx50_policy_ok", ctypes.c_int),
        ("family_detection_method", ctypes.c_char * 32),
        ("driver_version", ctypes.c_char * 64),
        ("runtime_path", ctypes.c_char * 1024),
        ("runtime_size", ctypes.c_uint64),
        ("runtime_file_version", ctypes.c_char * 64),
        ("runtime_sha256", ctypes.c_char * 72),
        ("runtime_signature", ctypes.c_char * 32),
        ("runtime_signer", ctypes.c_char * 512),
        ("runtime_classification", ctypes.c_char * 32),
        ("ngx_core_path", ctypes.c_char * 1024),
        ("ngx_core_discovery", ctypes.c_char * 32),
        ("shim_path", ctypes.c_char * 1024),
        ("create_feature_result", ctypes.c_int),
        ("last_evaluate_result", ctypes.c_int),
    ]


_lib.nr_version.restype = ctypes.c_char_p
_lib.nr_create.argtypes = [
    ctypes.POINTER(ctypes.c_void_p),
    ctypes.POINTER(NR_Options),
    ctypes.POINTER(NR_Error),
]
_lib.nr_create.restype = ctypes.c_int
_lib.nr_initialize.argtypes = [ctypes.c_void_p, ctypes.POINTER(NR_Error)]
_lib.nr_initialize.restype = ctypes.c_int
_lib.nr_evaluate.argtypes = [
    ctypes.c_void_p,
    ctypes.POINTER(NR_FrameDesc),
    ctypes.POINTER(NR_Settings),
    ctypes.POINTER(ctypes.c_float),
    ctypes.POINTER(NR_Error),
]
_lib.nr_evaluate.restype = ctypes.c_int
_lib.nr_get_diagnostics.argtypes = [ctypes.c_void_p, ctypes.POINTER(NR_Diagnostics)]
_lib.nr_get_diagnostics.restype = ctypes.c_int
_lib.nr_destroy.argtypes = [ctypes.c_void_p]
_lib.nr_destroy.restype = None

CATEGORY_NAMES = {
    0: "None",
    1: "UnsupportedGpu",
    2: "ModifiedRuntimeRejected",
    3: "RuntimeMissing",
    4: "InvalidAuthenticode",
    5: "NgxCoreMissing",
    6: "D3D12InitFailure",
    7: "NgxInitFailure",
    8: "FeatureCreationFailure",
    9: "EvaluateFailure",
    10: "ReadbackFailure",
    11: "InvalidOutput",
    12: "Internal",
}


class NRBridgeError(Exception):
    """Raised when the native bridge reports a failure (§25 context)."""

    def __init__(self, err: NR_Error):
        self.category = err.category
        self.stage = err.stage.decode("utf-8", "replace")
        self.raw_code = err.raw_code
        self.message = err.message.decode("utf-8", "replace")
        self.gpu_name = err.gpu_name.decode("utf-8", "replace")
        self.driver_version = err.driver_version.decode("utf-8", "replace")
        self.runtime_version = err.runtime_version.decode("utf-8", "replace")
        self.runtime_sha256 = err.runtime_sha256.decode("utf-8", "replace")
        super().__init__(self.format())

    def format(self):
        return (
            f"category={CATEGORY_NAMES.get(self.category, self.category)} "
            f"stage={self.stage} raw=0x{self.raw_code:08X} message={self.message} "
            f"gpu={self.gpu_name} driver={self.driver_version} "
            f"runtime_sha256={self.runtime_sha256}"
        )


# ---------------------------------------------------------------------------
# High-level API
# ---------------------------------------------------------------------------

class Bridge:
    """One bridge DLL session: create -> initialize -> evaluate* -> destroy."""

    def __init__(self, runtime_dir=None, ngx_core_path=None, gpu_index=0,
                 reject_unsigned=False):
        opts = NR_Options()
        opts.runtime_dir = runtime_dir.encode("utf-8") if runtime_dir else None
        opts.ngx_core_path = ngx_core_path.encode("utf-8") if ngx_core_path else None
        opts.gpu_index = gpu_index
        opts.reject_unsigned = 1 if reject_unsigned else 0
        err = NR_Error()
        handle = ctypes.c_void_p()
        if not _lib.nr_create(ctypes.byref(handle), ctypes.byref(opts),
                              ctypes.byref(err)):
            raise NRBridgeError(err)
        self._handle = handle

    @property
    def version(self):
        return _lib.nr_version().decode("utf-8")

    def initialize(self):
        err = NR_Error()
        if not _lib.nr_initialize(self._handle, ctypes.byref(err)):
            raise NRBridgeError(err)

    def evaluate(self, width, height, rgba_top_left, settings=None):
        """rgba_top_left: numpy float32 array (4*w*h) or any object
        supporting ctypes.data_as. Returns a new numpy float32 array."""
        import numpy as np
        if rgba_top_left.size != width * height * 4:
            raise ValueError("rgba_top_left size does not match width/height")
        s = NR_Settings()
        if settings is None:
            s.style, s.preset = 1, 3
            s.intensity = s.local_tone_strength = s.local_structure_strength = 1.0
            s.skin_structure_strength = -1.0
            s.use_auto_mask = 0
        else:
            s.style = settings.get("style", 1)
            s.preset = settings.get("preset", 3)
            s.intensity = settings.get("intensity", 1.0)
            s.local_tone_strength = settings.get("tone", 1.0)
            s.local_structure_strength = settings.get("structure", 1.0)
            s.skin_structure_strength = settings.get("skin", -1.0)
            s.use_auto_mask = 1 if settings.get("auto_mask", False) else 0

        frame = NR_FrameDesc()
        frame.width = width
        frame.height = height
        frame.rgba_f32 = rgba_top_left.ctypes.data_as(ctypes.POINTER(ctypes.c_float))
        frame.reset = 1  # still-image mode (A2)

        out = np.empty(width * height * 4, dtype=np.float32)
        err = NR_Error()
        if not _lib.nr_evaluate(self._handle, ctypes.byref(frame),
                                ctypes.byref(s),
                                out.ctypes.data_as(ctypes.POINTER(ctypes.c_float)),
                                ctypes.byref(err)):
            raise NRBridgeError(err)
        return out

    def diagnostics(self):
        """Returns the diagnostics snapshot as a dict (§16)."""
        d = NR_Diagnostics()
        if not _lib.nr_get_diagnostics(self._handle, ctypes.byref(d)):
            raise RuntimeError("nr_get_diagnostics failed")
        def dec(v):
            return v.decode("utf-8", "replace")
        return {
            "backend_name": dec(d.backend_name),
            "gpu_name": dec(d.gpu_name),
            "vendor_id": d.vendor_id,
            "adapter_index": d.adapter_index,
            "nvidia_index": d.nvidia_index,
            "architecture": dec(d.architecture),
            "nvml_arch_raw": d.nvml_arch_raw,
            "rtx50_policy_ok": bool(d.rtx50_policy_ok),
            "family_detection_method": dec(d.family_detection_method),
            "driver_version": dec(d.driver_version),
            "runtime_path": dec(d.runtime_path),
            "runtime_size": d.runtime_size,
            "runtime_file_version": dec(d.runtime_file_version),
            "runtime_sha256": dec(d.runtime_sha256),
            "runtime_signature": dec(d.runtime_signature),
            "runtime_signer": dec(d.runtime_signer),
            "runtime_classification": dec(d.runtime_classification),
            "ngx_core_path": dec(d.ngx_core_path),
            "ngx_core_discovery": dec(d.ngx_core_discovery),
            "shim_path": dec(d.shim_path),
            "create_feature_result": d.create_feature_result,
            "last_evaluate_result": d.last_evaluate_result,
        }

    def close(self):
        if self._handle:
            _lib.nr_destroy(self._handle)
            self._handle = None

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        self.close()
