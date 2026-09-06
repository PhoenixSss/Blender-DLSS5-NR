# SPDX-License-Identifier: MIT
# blender/panel.py — sidebar panels (A7): main controls (§11) and runtime
# diagnostics (§16, closes out A5).

import os

import bpy

from . import ADDON_ID
from . import facade
from . import operators


class DLSS5NR_PT_Main(bpy.types.Panel):
    bl_idname = "DLSS5NR_PT_Main"
    bl_label = "DLSS 5 Neural Rendering"
    bl_space_type = "IMAGE_EDITOR"
    bl_region_type = "UI"
    bl_category = "DLSS 5 NR"

    def draw(self, context):
        layout = self.layout
        scene = context.scene
        s = scene.dlss5nr

        # Actions
        box = layout.box()
        box.label(text="Unofficial / Experimental / RTX 50 only",
                  icon="ERROR")
        box.operator("dlss5nr.process_render_result", icon="RENDER_STILL")
        box.operator("dlss5nr.process_image", icon="IMAGE_DATA")

        # §11 parameters
        box = layout.box()
        box.label(text="Parameters")
        box.prop(s, "style")
        box.prop(s, "preset")
        box.prop(s, "intensity")
        box.prop(s, "local_tone_strength")
        box.prop(s, "local_structure_strength")
        box.prop(s, "skin_structure_strength")
        box.prop(s, "use_auto_mask")
        box.prop(s, "encoding")
        box.prop(s, "clamp")

        # Session info (§11: source / GPU / runtime)
        prefs = operators.get_preferences(context)
        box = layout.box()
        box.label(text="Session")
        row = box.row()
        row.label(text="Source")
        row.label(text="Render Result / Combined")
        row = box.row()
        row.label(text="GPU")
        row.label(text=str(prefs.gpu_index if prefs else 0))
        row = box.row()
        row.label(text="Runtime")
        row.label(text=os.path.basename(prefs.runtime_dir.rstrip("\\/"))
                  if prefs and prefs.runtime_dir else "(not set)")
        box.operator("preferences.addon_show", text="Open Preferences",
                     icon="PREFERENCES").module = ADDON_ID

        # Compositor node façade (需求书 阶段 1: node + preview only —
        # no automatic behavior; batch processing is a later phase).
        box = layout.box()
        box.label(text="Compositor Node")
        present = facade.facade_enabled(context.scene)
        row = box.row()
        row.label(text="State",
                  icon="CHECKBOX_HLT" if present else "CHECKBOX_DEHLT")
        row.label(text="in compositor" if present else "not added")
        box.operator("dlss5nr.add_facade", icon="NODETREE")
        if not present:
            box.label(text="Add the node, connect its output to a Viewer, "
                           "then process a frame to preview the result.")
        box.operator("dlss5nr.refresh_probe", icon="RESTRICT_RENDER_OFF")


class DLSS5NR_PT_Diagnostics(bpy.types.Panel):
    """§16 runtime info snapshot of the last processing run."""
    bl_idname = "DLSS5NR_PT_Diagnostics"
    bl_label = "Diagnostics"
    bl_space_type = "IMAGE_EDITOR"
    bl_region_type = "UI"
    bl_category = "DLSS 5 NR"
    bl_parent_id = "DLSS5NR_PT_Main"

    @classmethod
    def poll(cls, context):
        return True

    def draw(self, context):
        layout = self.layout
        d = operators._last_diagnostics
        if not d:
            layout.label(text="No run yet — process an image first.")
            return

        layout.label(text=f"Backend: {d.get('backend_name', '?')}")
        layout.label(text=f"GPU: {d.get('gpu_name', '?')} "
                          f"({d.get('architecture', '?')}, "
                          f"RTX50 {'OK' if d.get('rtx50_policy_ok') else 'FAIL'})")
        layout.label(text=f"Driver: {d.get('driver_version', '?')}")

        box = layout.box()
        box.label(text="Runtime")
        box.label(text=f"Version: {d.get('runtime_file_version') or '(none)'}")
        box.label(text=f"Size: {d.get('runtime_size', 0)} bytes")
        box.label(text=f"Signature: {d.get('runtime_signature', '?')}")
        box.label(text=f"Classification: {d.get('runtime_classification', '?')}")
        row = box.row()
        row.label(text=f"SHA-256: {d.get('runtime_sha256', '?')[:24]}…")
        row.operator("dlss5nr.copy_runtime_hash", text="", icon="COPYDOWN")

        box = layout.box()
        box.label(text="Session")
        box.label(text=f"NGX core: {d.get('ngx_core_path', '?')}")
        box.label(text=f"NGX discovery: {d.get('ngx_core_discovery', '?')}")
        box.label(text=f"Shim: {d.get('shim_path', '?')}")
        box.label(text=f"CreateFeature: {d.get('create_feature_result')}")
        box.label(text=f"EvaluateFeature: {d.get('last_evaluate_result')}")
        box.label(text=f"Encoding: {d.get('encoding', '?')} "
                       f"Clamp: {d.get('clamp', '?')}")
