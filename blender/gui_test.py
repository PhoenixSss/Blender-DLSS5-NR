# SPDX-License-Identifier: MIT
# blender/gui_test.py — GUI 控制台辅助脚本：加载测试图并在 Image Editor
# 上下文中执行 A2 operator。控制台单行执行即可：
#
#   exec(open(r"D:\workspace\program\DLSS5-Blender\Blender-DLSS5-NR\blender_dlss5\blender\gui_test.py").read())
#
# 之后可重复调用 run_nr("12140.png") 再次处理。

import os

import bpy

TEST_IMAGE_DIR = os.environ.get(
    "DLSS5NR_TEST_IMAGE_DIR",
    r"D:\workspace\program\DLSS5-Blender\Blender-DLSS5-NR")


def run_nr(imgname):
    """Loads the image, shows it in an Image Editor and runs the operator."""
    img = bpy.data.images.get(imgname)
    if img is None:
        img = bpy.data.images.load(os.path.join(TEST_IMAGE_DIR, imgname))

    areas = [a for a in bpy.context.screen.areas if a.type == "IMAGE_EDITOR"]
    area = areas[0] if areas else None
    if area is None:
        area = bpy.context.screen.areas[0]
        area.type = "IMAGE_EDITOR"
    space = area.spaces.active
    space.image = img

    with bpy.context.temp_override(area=area, space_data=space):
        bpy.ops.dlss5nr.process_image()
    result = bpy.data.images.get("DLSS5_NR_Result")
    if result is not None:
        print("[gui_test] result image:", result.name,
              result.size[0], "x", result.size[1], "float:", result.is_float)
    return result


run_nr("12140.png")
