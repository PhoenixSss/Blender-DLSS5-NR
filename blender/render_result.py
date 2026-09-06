# SPDX-License-Identifier: MIT
# blender/render_result.py — input extraction for the neural backend.
#
# Converts Blender-side image conventions (bottom-up float buffers, RGBA)
# into the canonical top-left RGBA float32 form consumed by the bridge.
#
# Two extraction paths:
#   1. Image datablock (works headless and GUI; used for A2 testing with
#      the user-provided test image).
#   2. Render Result / Combined via the compositor Viewer Node (Blender 5.x:
#      the Render Result image itself is no longer Python-readable; the
#      supported route is an RLayers -> Viewer branch. Requires GUI mode —
#      the viewer only updates after the compositing workspace is shown).

import bpy


def _numpy_or_array_module():
    """numpy preferred (bundled with Blender); array module as fallback."""
    try:
        import numpy
        return numpy, True
    except ImportError:
        import array
        return array, False


def image_pixels_top_left(image):
    """Reads `image.pixels` (bottom-up RGBA float) and returns a flattened
    top-left numpy float32 array of length 4*w*h."""
    w, h = image.size
    if w == 0 or h == 0:
        raise ValueError(f"Image {image.name!r} has no pixel data (size {w}x{h})")
    numpy, _ = _numpy_or_array_module()
    pixels = numpy.asarray(image.pixels[:], dtype=numpy.float32)
    pixels = pixels.reshape(h, w, 4)
    flipped = numpy.ascontiguousarray(pixels[::-1])  # bottom-up -> top-left
    return flipped.reshape(-1)


def ensure_viewer_path(scene):
    """Ensures an RLayers -> Viewer branch exists in the scene's compositor
    node group (Blender 5.x: Scene.compositing_node_group). Creates the
    standard passthrough tree when the scene has no compositing at all.

    Returns the Viewer Node image; GUI path: the viewer updates after the
    compositing workspace is shown."""
    try:
        from . import facade
        group = facade.ensure_scene_compositor_tree(scene)
    except Exception:
        group = getattr(scene, "compositing_node_group", None)
        if group is None:
            return None
    rl = next((n for n in group.nodes if n.type == "R_LAYERS"), None)
    if rl is None:
        rl = group.nodes.new("CompositorNodeRLayers")
        rl.name = "DLSS5_NR_RenderLayers"
    viewer = next((n for n in group.nodes if n.type == "VIEWER"), None)
    if viewer is None:
        viewer = group.nodes.new("CompositorNodeViewer")
        viewer.name = "DLSS5_NR_Viewer"
    linked = any(l.from_node == rl and l.to_node == viewer for l in group.links)
    if not linked:
        group.links.new(rl.outputs["Image"], viewer.inputs["Image"])
    return bpy.data.images.get("Viewer Node")


def extract_viewer():
    """Reads the compositor Viewer Node image (top-left RGBA float32).

    Raises RuntimeError with guidance when the viewer has no data (the
    render result path requires GUI mode in Blender 5.x)."""
    viewer = bpy.data.images.get("Viewer Node")
    if viewer is None or viewer.size[0] == 0 or len(viewer.pixels) == 0:
        raise RuntimeError(
            "Viewer Node has no data. In Blender 5.x the Render Result is not "
            "Python-readable; render the frame, show the Compositing workspace "
            "once so the viewer updates, then run the operator again.")
    return image_pixels_top_left(viewer)
