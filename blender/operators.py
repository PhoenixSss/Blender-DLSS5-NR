# SPDX-License-Identifier: MIT
# blender/operators.py — processing operators (A7; migrated from the A2
# console-era add-on).

import os

import bpy

from . import ADDON_ID
from . import bridge
from . import image_output
from . import render_result

# Last-run diagnostics snapshot (§16), shown by panel.DLSS5NR_PT_Diagnostics.
_last_diagnostics = {}


def get_preferences(context=None):
    if context is None:
        context = bpy.context
    addon = context.preferences.addons.get(ADDON_ID)
    return addon.preferences if addon else None


# Scene-driven encoding selection (A3). The neural input domain follows the
# scene's view transform, which also defines the HDR strategy: display
# transforms compress scene-linear HDR values into [0,1] by design.
#
# A3 acceptance (2026-09-05, user visual test): the product default input
# encoding is AgX — no overexposure on strong highlights.
#   AgX               -> AgXDisplay (2)
#   Standard          -> StandardDisplay (1)
#   Filmic/Filmic Log -> AgXDisplay (2)  [product default; Filmic deferred]
#   Raw/False Color   -> SceneLinear (0) + clamp
#   anything else     -> AgXDisplay (2)  (product default)
def resolve_encoding(scene, choice="auto"):
    if choice != "auto":
        return int(choice)
    vt = getattr(getattr(scene, "view_settings", None), "view_transform", "")
    if vt == "AgX":
        return 2
    if vt == "Standard":
        return 1
    if vt in ("Filmic", "Filmic Log"):
        print("[DLSS5-NR] scene view transform is Filmic — mapped to the "
              "accepted product default (AgX); Filmic itself is deferred")
        return 2
    if vt in ("Raw", "False Color"):
        return 0
    return 2


def _run_processing(input_image, scene, settings, prefs):
    runtime_dir = (prefs.runtime_dir if prefs else "").strip()
    if not runtime_dir:
        runtime_dir = os.environ.get("DLSS5NR_RUNTIME_DIR", "")
    if not runtime_dir:
        raise RuntimeError(
            "Runtime directory not set. Open Preferences > Blender DLSS 5 "
            "Neural Rendering and choose the directory containing the "
            "user-provided nvngx_dlssnr.dll.")

    encoding = resolve_encoding(scene, settings.encoding)

    w, h = input_image.size
    rgba = render_result.image_pixels_top_left(input_image)

    kwargs = dict(
        runtime_dir=runtime_dir,
        clamp_input=settings.clamp,
    )
    if prefs:
        kwargs["gpu_index"] = prefs.gpu_index
        kwargs["reject_unsigned"] = prefs.reject_unsigned
        if prefs.ngx_core_path.strip():
            kwargs["ngx_core_path"] = prefs.ngx_core_path.strip()

    with bridge.Bridge(**kwargs) as b:
        b.initialize()
        diag = b.diagnostics()
        print(f"[DLSS5-NR] gpu={diag['gpu_name']} driver={diag['driver_version']} "
              f"arch={diag['architecture']} rtx50_ok={diag['rtx50_policy_ok']}")
        print(f"[DLSS5-NR] runtime={diag['runtime_path']}")
        print(f"[DLSS5-NR] runtime sha256={diag['runtime_sha256']} "
              f"class={diag['runtime_classification']}")
        out = b.evaluate(w, h, rgba, encoding=encoding,
                         settings=dict(
                             style=settings.style,
                             preset=settings.preset,
                             intensity=settings.intensity,
                             tone=settings.local_tone_strength,
                             structure=settings.local_structure_strength,
                             skin=settings.skin_structure_strength,
                             auto_mask=settings.use_auto_mask,
                         ))
        # Re-read after evaluate: create/evaluate results are filled by then.
        diag2 = b.diagnostics()
        print(f"[DLSS5-NR] encoding={encoding} clamp={settings.clamp} "
              f"CreateFeature={diag2['create_feature_result']} "
              f"EvaluateFeature={diag2['last_evaluate_result']}")
        _last_diagnostics.clear()
        _last_diagnostics.update(diag2)
        _last_diagnostics["encoding"] = encoding
        _last_diagnostics["clamp"] = settings.clamp

    # Scene-linear HDR output is labeled with Blender's canonical linear
    # colorspace name; display-referred encodings are sRGB-encoded values.
    colorspace = "Linear Rec.709" if encoding == 0 else "sRGB"
    return image_output.write_result_image("DLSS5_NR_Result", w, h, out,
                                           colorspace=colorspace)


class DLSS5NR_OT_ProcessImage(bpy.types.Operator):
    """Processes the active Image Editor image through DLSS 5 Neural
    Rendering and writes DLSS5_NR_Result."""
    bl_idname = "dlss5nr.process_image"
    bl_label = "Process Image Datablock"
    bl_description = "Run the active image through DLSS 5 Neural Rendering"

    @classmethod
    def poll(cls, context):
        space = getattr(context, "space_data", None)
        return bool(getattr(space, "image", None))

    def execute(self, context):
        try:
            result = _run_processing(
                context.space_data.image, context.scene,
                context.scene.dlss5nr, get_preferences(context))
        except Exception as e:  # noqa: BLE001 — surface everything to user
            self.report({"ERROR"}, str(e))
            print(f"[DLSS5-NR] ERROR: {e}")
            return {"CANCELLED"}
        self.report({"INFO"}, f"DLSS 5 NR result written to {result.name!r}")
        return {"FINISHED"}


class DLSS5NR_OT_ProcessRenderResult(bpy.types.Operator):
    """Processes the rendered Combined pass via the compositor Viewer Node
    (Blender 5.x Render Result is not Python-readable; see
    render_result.py)."""
    bl_idname = "dlss5nr.process_render_result"
    bl_label = "Process Render Result"
    bl_description = "Run the rendered Combined pass through DLSS 5 Neural " \
                     "Rendering (show the Compositing workspace once after " \
                     "rendering in Blender 5.x)"

    def execute(self, context):
        try:
            viewer = render_result.ensure_viewer_path(context.scene)
            if viewer is None:
                raise RuntimeError(
                    "No compositing node group on this scene "
                    "(Blender 5.x: Scene.compositing_node_group)")
            result = _run_processing(
                viewer, context.scene,
                context.scene.dlss5nr, get_preferences(context))
        except Exception as e:  # noqa: BLE001
            self.report({"ERROR"}, str(e))
            print(f"[DLSS5-NR] ERROR: {e}")
            return {"CANCELLED"}
        self.report({"INFO"}, f"DLSS 5 NR result written to {result.name!r}")
        return {"FINISHED"}


class DLSS5NR_OT_CopyRuntimeHash(bpy.types.Operator):
    """Copies the last runtime SHA-256 to the clipboard (§16 diagnostics)."""
    bl_idname = "dlss5nr.copy_runtime_hash"
    bl_label = "Copy runtime SHA-256"

    @classmethod
    def poll(cls, context):
        return bool(_last_diagnostics.get("runtime_sha256"))

    def execute(self, context):
        context.window_manager.clipboard = _last_diagnostics["runtime_sha256"]
        self.report({"INFO"}, "Runtime SHA-256 copied to clipboard")
        return {"FINISHED"}
