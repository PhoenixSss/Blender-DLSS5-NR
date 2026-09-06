# SPDX-License-Identifier: MIT
# blender/facade.py — compositor node façade (需求书 B2).
#
# The façade is a prebuilt CompositorNodeTree group: an Image node
# referencing the DLSS5_NR_Result datablock, wired to a group output. The
# user connects it like a normal node; its presence (and the Enabled
# group input) is the automatic-batch switch. The data flows through the
# native compositor pipeline (the Image node is a passive data source),
# so the Backdrop shows the result without any custom evaluation.

import bpy

FACADE_GROUP_NAME = "DLSS5NR_Facade"
FACADE_NODE_NAME = "DLSS 5 NR"
RESULT_IMAGE_NAME = "DLSS5_NR_Result"


def ensure_result_image():
    """Returns the result datablock, creating a placeholder if needed."""
    img = bpy.data.images.get(RESULT_IMAGE_NAME)
    if img is None:
        img = bpy.data.images.new(RESULT_IMAGE_NAME, width=8, height=8,
                                  float_buffer=True)
    return img


def repin_result_image(new_image):
    """Re-points every façade Image node at `new_image` (called after the
    result datablock is recreated on a size change)."""
    for group in bpy.data.node_groups:
        if group.name != FACADE_GROUP_NAME:
            continue
        for node in group.nodes:
            if node.type == "IMAGE":
                node.image = new_image


def ensure_facade_group():
    """Creates (or returns) the façade node group."""
    group = bpy.data.node_groups.get(FACADE_GROUP_NAME)
    if group is not None:
        return group

    group = bpy.data.node_groups.new(FACADE_GROUP_NAME, "CompositorNodeTree")

    # Group input: Enabled switch (§5.2: presence + Enabled = auto batch).
    ginput = group.nodes.new("NodeGroupInput")
    ginput.location = (-240, 0)
    group.interface.new_socket(
        "Enabled", in_out="INPUT", socket_type="NodeSocketBool")
    group.interface.items_tree["Enabled"].default_value = True

    # Passive data source: the NR result image.
    image_node = group.nodes.new("CompositorNodeImage")
    image_node.location = (0, 0)
    image_node.image = ensure_result_image()

    # Output socket.
    goutput = group.nodes.new("NodeGroupOutput")
    goutput.location = (240, 0)
    group.interface.new_socket(
        "Image", in_out="OUTPUT", socket_type="NodeSocketColor")

    group.links.new(image_node.outputs["Image"], goutput.inputs["Image"])
    return group


def add_facade_to_scene(scene):
    """Inserts a façade group instance into the scene's compositor tree
    (creating the tree when absent). Returns the group instance node."""
    ensure_facade_group()
    tree = getattr(scene, "compositing_node_group", None)
    if tree is None:
        tree = bpy.data.node_groups.new("DLSS5NR_Compositor",
                                        "CompositorNodeTree")
        scene.compositing_node_group = tree
        # A scene without compositing has no tree; build the standard
        # Render Layers -> output path (Blender 5.x: the tree's group
        # output IS the composite output — CompositorNodeComposite no
        # longer exists) so enabling the group never breaks frame output.
        rl = tree.nodes.new("CompositorNodeRLayers")
        rl.name = "Render Layers"
        gout = tree.nodes.new("NodeGroupOutput")
        tree.interface.new_socket("Image", in_out="OUTPUT",
                                  socket_type="NodeSocketColor")
        tree.links.new(rl.outputs["Image"], gout.inputs["Image"])
    node = tree.nodes.new("CompositorNodeGroup")
    node.name = FACADE_NODE_NAME
    node.node_tree = bpy.data.node_groups[FACADE_GROUP_NAME]
    node.label = FACADE_NODE_NAME
    return node


def facade_enabled(scene):
    """§5.2 semantics: an enabled façade instance in the compositor tree
    turns the automatic batch on; removing/disabling it turns it off."""
    tree = getattr(scene, "compositing_node_group", None)
    if tree is None:
        return False
    for node in tree.nodes:
        if node.type == "GROUP" and node.node_tree is not None and \
                node.node_tree.name == FACADE_GROUP_NAME:
            if node.mute:
                continue
            # The Enabled group input is exposed on the instance socket.
            try:
                return node.inputs["Enabled"].default_value
            except (KeyError, AttributeError):
                return True
    return False


class DLSS5NR_OT_AddFacade(bpy.types.Operator):
    """Inserts the DLSS 5 NR façade group into the scene's compositor."""
    bl_idname = "dlss5nr.add_facade"
    bl_label = "Add DLSS 5 NR Node"
    bl_description = "Insert the DLSS 5 NR node group into the compositor " \
                     "(connect its output to a Viewer to see results)"

    def execute(self, context):
        try:
            node = add_facade_to_scene(context.scene)
        except Exception as e:  # noqa: BLE001
            self.report({"ERROR"}, str(e))
            print(f"[DLSS5-NR] ERROR: {e}")
            return {"CANCELLED"}
        self.report({"INFO"}, f"Facade node {node.name!r} added to the "
                              "compositor; connect its output to a Viewer")
        return {"FINISHED"}


class DLSS5NR_OT_RefreshProbe(bpy.types.Operator):
    """B1 probe: fills the result datablock with a test pattern and nudges
    the compositor. Manual-only — watch whether the Backdrop updates
    automatically or only after the nudge."""
    bl_idname = "dlss5nr.refresh_probe"
    bl_label = "Probe Backdrop Refresh"
    bl_description = "B1 probe: fill DLSS5_NR_Result with a test pattern " \
                     "and nudge the compositor (watch the Backdrop)"

    def execute(self, context):
        import numpy as np
        scene = context.scene
        img = ensure_result_image()
        w, h = 256, 256
        if list(img.size) != [w, h]:
            img.scale(w, h)
        pat = np.zeros(w * h * 4, dtype=np.float32)
        pat[0::4] = 0.5  # gray
        img.pixels[:] = pat
        print("[DLSS5-NR] probe: result image filled; nudging compositor")
        force_viewer_refresh(scene)
        self.report({"INFO"}, "Probe done — check the compositor Backdrop")
        return {"FINISHED"}


def force_viewer_refresh(scene):
    """Nudge the 5.x compositor into re-evaluating so the Backdrop picks
    up the updated result datablock.

    SAFETY (2026-09-06): only ever call this OUTSIDE the render pipeline.
    Toggling node state mid-render crashes the compositor (driver crash
    observed). Manual/preview use only."""
    tree = getattr(scene, "compositing_node_group", None)
    if tree is None:
        return
    nodes = list(tree.nodes)
    if not nodes:
        return
    dummy = nodes[0]
    try:
        dummy.mute = not dummy.mute
        dummy.mute = not dummy.mute
    except Exception:
        pass  # never fail processing because of a preview nudge
