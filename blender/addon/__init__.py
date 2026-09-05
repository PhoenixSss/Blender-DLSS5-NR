# SPDX-License-Identifier: MIT
# blender/addon/__init__.py — A2 minimal add-on: operators only.
#
# The UI panel, preferences (runtime dir picker etc.) and ZIP packaging
# arrive in A7. For A2 the runtime directory comes from the environment
# variable DLSS5NR_RUNTIME_DIR, or a module constant below.
#
# Usage (A2, GUI): run this file in the Scripting workspace (or install the
# folder as an add-on later in A7), then search for "DLSS 5 NR" operators
# in F3: "Process Render Result" / "Process Image Datablock".

import os
import sys

import bpy

def _resolve_blender_dir():
    """Directory containing bridge.py / render_result.py / image_output.py.

    __file__ is only meaningful when this file runs as a real script path
    (Blender's console exec has no __file__; the Text Editor gives only a
    bare name). Falls back to DLSS5NR_ADDON_DIR, then the A2 dev default."""
    f = globals().get("__file__")
    if f and os.path.isfile(str(f)):
        parent = os.path.dirname(os.path.dirname(os.path.abspath(str(f))))
        if os.path.isfile(os.path.join(parent, "bridge.py")):
            return parent
    env = os.environ.get("DLSS5NR_ADDON_DIR")
    if env and os.path.isfile(os.path.join(env, "bridge.py")):
        return env
    # A2 dev default on this machine (A7 replaces with proper packaging).
    return r"D:\workspace\program\DLSS5-Blender\Blender-DLSS5-NR\blender_dlss5\blender"


BLENDER_DIR = _resolve_blender_dir()
if BLENDER_DIR not in sys.path:
    sys.path.insert(0, BLENDER_DIR)

import bridge  # noqa: E402
import image_output  # noqa: E402
import render_result  # noqa: E402

# A2 dev default: the runtime directory on this machine. A7 replaces this
# with a UI preference. Environment variable takes precedence.
DEFAULT_RUNTIME_DIR = os.environ.get(
    "DLSS5NR_RUNTIME_DIR",
    r"D:\workspace\program\DLSS5-Blender\DLSS.5.Visual.Enhancer.v5.0\bin\runtime\host")


# A3: scene-driven encoding selection (user decision 2026-09-05).
# The neural input domain follows the scene's view transform, which also
# defines the HDR strategy: display transforms (Standard/AgX) compress
# scene-linear HDR values into [0,1] by design — no hard clamp is needed
# to "handle" HDR, and out-of-range values never reach the network.
#   AgX            -> AgXDisplay (2)
#   Standard       -> StandardDisplay (1)
#   Filmic/Filmic Log -> StandardDisplay (1)  [closest implemented family]
#   Raw/False Color -> SceneLinear (0) + clamp (values passthrough)
#   anything else  -> StandardDisplay (1)
def resolve_encoding(scene, choice="auto"):
    """Resolves the operator encoding choice. `choice` is the operator
    property value: 'auto' or '0'/'1'/'2'."""
    if choice != "auto":
        return int(choice)
    vt = getattr(getattr(scene, "view_settings", None), "view_transform", "")
    if vt == "AgX":
        return 2
    if vt == "Standard":
        return 1
    if vt in ("Filmic", "Filmic Log"):
        print("[DLSS5-NR] scene view transform is Filmic — mapped to the "
              "closest implemented family (Standard); Filmic itself is not "
              "implemented yet")
        return 1
    if vt in ("Raw", "False Color"):
        return 0
    return 1


def _runtime_dir():
    return os.environ.get("DLSS5NR_RUNTIME_DIR", DEFAULT_RUNTIME_DIR)


def _run_processing(input_image, scene, encoding="auto", clamp=True):
    runtime_dir = _runtime_dir()
    if not runtime_dir:
        raise RuntimeError(
            "Runtime directory not set. Set the environment variable "
            "DLSS5NR_RUNTIME_DIR to the directory containing nvngx_dlssnr.dll "
            "(A7 adds a UI preference for this).")

    encoding = resolve_encoding(scene, encoding)

    w, h = input_image.size
    rgba = render_result.image_pixels_top_left(input_image)

    with bridge.Bridge(runtime_dir=runtime_dir, clamp_input=clamp) as b:
        b.initialize()
        diag = b.diagnostics()
        print(f"[DLSS5-NR] gpu={diag['gpu_name']} driver={diag['driver_version']} "
              f"arch={diag['architecture']} rtx50_ok={diag['rtx50_policy_ok']}")
        print(f"[DLSS5-NR] runtime={diag['runtime_path']}")
        print(f"[DLSS5-NR] runtime sha256={diag['runtime_sha256']} "
              f"class={diag['runtime_classification']}")
        out = b.evaluate(w, h, rgba, encoding=encoding)
        # Re-read after evaluate: create/evaluate results are filled by then.
        diag2 = b.diagnostics()
        print(f"[DLSS5-NR] encoding={encoding} clamp={clamp} "
              f"CreateFeature={diag2['create_feature_result']} "
              f"EvaluateFeature={diag2['last_evaluate_result']}")

    # Scene-linear HDR output is labeled with Blender's canonical linear
    # colorspace name; display-referred encodings are sRGB-encoded values.
    colorspace = "Linear Rec.709" if encoding == 0 else "sRGB"
    return image_output.write_result_image("DLSS5_NR_Result", w, h, out,
                                           colorspace=colorspace)


class DLSS5NR_OT_ProcessImage(bpy.types.Operator):
    """Processes the active Image Editor image through DLSS 5 Neural
    Rendering and writes DLSS5_NR_Result (A2 test path)."""
    bl_idname = "dlss5nr.process_image"
    bl_label = "DLSS 5 NR: Process Image Datablock"
    bl_description = "Run the active image through DLSS 5 Neural Rendering " \
                     "(A2: image datablock input)"

    encoding: bpy.props.EnumProperty(
        name="Color Encoding",
        description="Color domain fed to the neural backend. 'Auto' follows "
                    "the scene's view transform (A3 decision: scene-driven)",
        items=[
            ("auto", "Auto (scene view transform)", "Follow the scene's "
             "view transform; display transforms compress HDR to [0,1]"),
            ("0", "Scene Linear", "Raw scene-linear (clamped [0,1])"),
            ("1", "Standard", "sRGB EOTF display encoding"),
            ("2", "AgX", "AgX display transform"),
        ],
        default="auto",
    )
    clamp: bpy.props.BoolProperty(
        name="Clamp [0,1]",
        description="Clamp RGB to [0,1] (default ON: the network's known-good "
                    "domain; HDR passthrough caused artifacts in A3 testing)",
        default=True,
    )

    @classmethod
    def poll(cls, context):
        space = getattr(context, "space_data", None)
        return bool(getattr(space, "image", None))

    def execute(self, context):
        try:
            result = _run_processing(context.space_data.image, context.scene,
                                     encoding=self.encoding,
                                     clamp=self.clamp)
        except Exception as e:  # noqa: BLE001 — surface everything to user
            self.report({"ERROR"}, str(e))
            print(f"[DLSS5-NR] ERROR: {e}")
            return {"CANCELLED"}
        self.report({"INFO"}, f"DLSS 5 NR result written to {result.name!r}")
        return {"FINISHED"}


class DLSS5NR_OT_ProcessRenderResult(bpy.types.Operator):
    """Renders/scene Combined via the compositor Viewer Node path (Blender
    5.x Render Result is not Python-readable; see render_result.py)."""
    bl_idname = "dlss5nr.process_render_result"
    bl_label = "DLSS 5 NR: Process Render Result"
    bl_description = "Run the rendered Combined pass through DLSS 5 Neural " \
                     "Rendering (requires the Compositing workspace shown " \
                     "once after rendering in Blender 5.x)"

    def execute(self, context):
        try:
            viewer = render_result.ensure_viewer_path(context.scene)
            if viewer is None:
                raise RuntimeError(
                    "No compositing node group on this scene "
                    "(Blender 5.x: Scene.compositing_node_group)")
            result = _run_processing(viewer, context.scene,
                                     encoding=self.encoding,
                                     clamp=self.clamp)
        except Exception as e:  # noqa: BLE001
            self.report({"ERROR"}, str(e))
            print(f"[DLSS5-NR] ERROR: {e}")
            return {"CANCELLED"}
        self.report({"INFO"}, f"DLSS 5 NR result written to {result.name!r}")
        return {"FINISHED"}

    # Same A3 experiment properties as the image operator (no F3 operator
    # properties dialog here — console-driven in A3, UI in A7).
    encoding: bpy.props.EnumProperty(
        name="Color Encoding",
        items=[
            ("auto", "Auto (scene view transform)", "Follow the scene's "
             "view transform"),
            ("0", "Scene Linear", "Raw scene-linear (clamped [0,1])"),
            ("1", "Standard", "sRGB EOTF display encoding"),
            ("2", "AgX", "AgX display transform"),
        ],
        default="auto",
    )
    clamp: bpy.props.BoolProperty(
        name="Clamp [0,1]",
        default=True,
    )


CLASSES = (
    DLSS5NR_OT_ProcessImage,
    DLSS5NR_OT_ProcessRenderResult,
)


def register():
    for cls in CLASSES:
        bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(CLASSES):
        bpy.utils.unregister_class(cls)


if __name__ == "__main__":
    register()
