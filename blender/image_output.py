# SPDX-License-Identifier: MIT
# blender/image_output.py — writes canonical top-left RGBA float32 frames
# into Blender Image datablocks (bottom-up convention).

import bpy


def write_result_image(name, width, height, rgba_top_left, colorspace="sRGB"):
    """Creates (or replaces) a float Image datablock named `name` with the
    given top-left RGBA float32 data. Returns the Image.

    `colorspace` labels the buffer semantics (A3): display-referred
    encodings use "sRGB", scene-linear HDR output uses "Linear Rec.709"
    (Blender 5.2's canonical scene-linear colorspace name; "Linear"
    alone is not valid).

    No temporary files are involved (§29); the data goes straight into the
    datablock buffer."""
    rgba_top_left = rgba_top_left.reshape(-1)
    if rgba_top_left.size != width * height * 4:
        raise ValueError("rgba_top_left size does not match width/height")

    # Replace any existing image with the same name.
    existing = bpy.data.images.get(name)
    if existing is not None:
        existing.user_clear()
        bpy.data.images.remove(existing)

    image = bpy.data.images.new(name, width=width, height=height,
                                float_buffer=True)
    # The buffer holds the numeric values as-is; the label only declares
    # their semantics (no color transform is applied here). Invalid or
    # unavailable colorspace names must not crash the write — fall back
    # to the image default with a warning.
    try:
        image.colorspace_settings.name = colorspace
    except Exception:
        print(f"[DLSS5-NR] WARNING: colorspace {colorspace!r} unavailable; "
              f"using {image.colorspace_settings.name!r}")

    # Blender buffers are bottom-up: flip the canonical top-left rows.
    numpy, _ = _numpy_or_array_module()
    pixels = rgba_top_left.reshape(height, width, 4)
    flipped = numpy.ascontiguousarray(pixels[::-1]).reshape(-1)
    image.pixels[:] = flipped
    return image


def _numpy_or_array_module():
    try:
        import numpy
        return numpy, True
    except ImportError:
        import array
        return array, False
