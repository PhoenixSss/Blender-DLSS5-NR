# SPDX-License-Identifier: MIT
# blender/__init__.py — add-on entry point (A7).
#
# Unofficial, experimental DLSS 5 Neural Rendering for Blender still
# images. Not affiliated with, endorsed by, or supported by NVIDIA.
# RTX 50 Series only. The neural runtime (nvngx_dlssnr.dll) is
# user-provided and never redistributed.

import bpy

bl_info = {
    "name": "Blender DLSS 5 Neural Rendering",
    "description": "Unofficial, experimental DLSS 5 Neural Rendering for "
                   "still images (NGX feature 18, reverse-engineered "
                   "experimental backend). RTX 50 Series only. Not "
                   "affiliated with or endorsed by NVIDIA.",
    "author": "Blender-DLSS5-NR contributors",
    "version": (0, 1, 0),
    "blender": (5, 2, 0),
    "category": "Render",
    "location": "Image Editor > Sidebar > DLSS 5 NR",
}

# The add-on module id (must match the installed package name: the ZIP
# file is named blender_dlss5_addon.zip).
ADDON_ID = "blender_dlss5_addon"

from . import facade  # noqa: E402
from . import operators  # noqa: E402
from . import panel  # noqa: E402


class DLSS5NR_Settings(bpy.types.PropertyGroup):
    """§11: per-scene neural rendering parameters."""

    style: bpy.props.IntProperty(
        name="Style",
        description="Neural rendering style (0=default, 1=natural, 2=cinematic)",
        min=0, max=2, default=1,
    )
    preset: bpy.props.IntProperty(
        name="Preset",
        description="Internal render preset",
        default=3,
    )
    intensity: bpy.props.FloatProperty(
        name="Intensity",
        description="Overall neural rendering strength",
        min=0.0, max=2.0, default=1.0,
    )
    local_tone_strength: bpy.props.FloatProperty(
        name="Local Tone Strength",
        description="Local tone / lighting strength",
        min=0.0, max=2.0, default=1.0,
    )
    local_structure_strength: bpy.props.FloatProperty(
        name="Local Structure Strength",
        description="Local structure / detail strength",
        min=0.0, max=2.0, default=1.0,
    )
    skin_structure_strength: bpy.props.FloatProperty(
        name="Skin Structure Strength",
        description="Skin structure strength (-1 leaves it to the runtime)",
        min=-1.0, max=2.0, default=-1.0,
    )
    use_auto_mask: bpy.props.BoolProperty(
        name="Auto Mask",
        description="Let the runtime generate its own masking",
        default=False,
    )
    encoding: bpy.props.EnumProperty(
        name="Color Input Mode",
        description="Color domain fed to the neural backend. 'Auto' follows "
                    "the scene for Render Result and respects Blender's "
                    "image colorspace for file images (product default: AgX)",
        items=[
            ("auto", "Auto (scene view transform)", "Follow the scene's "
             "view transform; display transforms compress HDR to [0,1]"),
            ("0", "Scene Linear", "Raw scene-linear (clamped [0,1])"),
            ("1", "Standard", "sRGB EOTF display encoding"),
            ("2", "AgX", "AgX display transform (product default)"),
        ],
        default="auto",
    )
    clamp: bpy.props.BoolProperty(
        name="Clamp [0,1]",
        description="Clamp RGB to [0,1] (the network's known-good domain; "
                    "unclamped HDR passthrough causes artifacts)",
        default=True,
    )


class DLSS5NR_Preferences(bpy.types.AddonPreferences):
    """§11/§16: user-provided runtime location and session options."""

    bl_idname = ADDON_ID

    runtime_dir: bpy.props.StringProperty(
        name="Runtime Directory",
        description="Directory containing the user-provided nvngx_dlssnr.dll",
        subtype="DIR_PATH",
        default="",
    )
    ngx_core_path: bpy.props.StringProperty(
        name="NGX Core (optional)",
        description="Explicit _nvngx.dll path (leave empty for automatic "
                    "driver-store discovery)",
        subtype="FILE_PATH",
        default="",
    )
    gpu_index: bpy.props.IntProperty(
        name="GPU Index",
        description="Index among NVIDIA adapters (0 = first)",
        min=0, default=0,
    )
    reject_unsigned: bpy.props.BoolProperty(
        name="Reject Unsigned Runtime",
        description="Treat an invalid Authenticode signature as an error "
                    "instead of a warning",
        default=False,
    )

    def draw(self, context):
        layout = self.layout
        layout.prop(self, "runtime_dir")
        layout.prop(self, "ngx_core_path")
        layout.prop(self, "gpu_index")
        layout.prop(self, "reject_unsigned")


CLASSES = (
    DLSS5NR_Settings,
    DLSS5NR_Preferences,
    operators.DLSS5NR_OT_ProcessImage,
    operators.DLSS5NR_OT_ProcessRenderResult,
    operators.DLSS5NR_OT_CopyRuntimeHash,
    facade.DLSS5NR_OT_AddFacade,
    facade.DLSS5NR_OT_RefreshProbe,
    panel.DLSS5NR_PT_Main,
    panel.DLSS5NR_PT_Diagnostics,
)


def register():
    for cls in CLASSES:
        bpy.utils.register_class(cls)
    bpy.types.Scene.dlss5nr = bpy.props.PointerProperty(
        type=DLSS5NR_Settings)


def unregister():
    del bpy.types.Scene.dlss5nr
    for cls in reversed(CLASSES):
        bpy.utils.unregister_class(cls)


if __name__ == "__main__":
    register()
