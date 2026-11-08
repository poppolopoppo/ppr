"""Blender-only reference recipe: editable periodic painted steel and raw PBR bakes."""

import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import sys

import bpy
from mathutils import Vector


MATERIAL = "ProceduralPaintedSteel"
CHANNELS = ("base_color", "roughness", "metallic", "height", "chip_mask", "normal")


def node(tree, kind, label, x, y):
    result = tree.nodes.new(kind)
    result.name = label
    result.label = label
    result.location = (x, y)
    return result


def math_node(tree, operation, a, b=None, label=None):
    result = node(tree, "ShaderNodeMath", label or operation, -600, -200)
    result.operation = operation
    for index, value in enumerate((a, b)):
        if value is None:
            continue
        if isinstance(value, (int, float)):
            result.inputs[index].default_value = value
        else:
            tree.links.new(value, result.inputs[index])
    return result.outputs[0]


def ramp(tree, source, name, low, high, first, last, x, y):
    result = node(tree, "ShaderNodeValToRGB", name, x, y)
    result.color_ramp.interpolation = "LINEAR"
    result.color_ramp.elements[0].position = low
    result.color_ramp.elements[0].color = (*first, 1)
    result.color_ramp.elements[1].position = high
    result.color_ramp.elements[1].color = (*last, 1)
    tree.links.new(source, result.inputs[0])
    return result.outputs[0]


def create_material(seed):
    material = bpy.data.materials.new(MATERIAL)
    material.use_nodes = True
    tree = material.node_tree
    tree.nodes.clear()
    uv = node(tree, "ShaderNodeTexCoord", "Periodic UV", -1800, 400)
    split = node(tree, "ShaderNodeSeparateXYZ", "UV components", -1600, 400)
    tree.links.new(uv.outputs["UV"], split.inputs[0])
    u = math_node(tree, "MULTIPLY", split.outputs["X"], 2 * math.pi)
    v = math_node(tree, "MULTIPLY", split.outputs["Y"], 2 * math.pi)
    coords = node(tree, "ShaderNodeCombineXYZ", "Torus coordinates", -1000, 400)
    for socket, value in zip(coords.inputs, (
        math_node(tree, "COSINE", u), math_node(tree, "SINE", u),
        math_node(tree, "COSINE", v),
    )):
        tree.links.new(value, socket)

    seed_node = node(tree, "ShaderNodeValue", "Seed", -1400, -200)
    seed_node.outputs[0].default_value = seed
    w = math_node(tree, "ADD", math_node(tree, "SINE", v),
                  math_node(tree, "MULTIPLY", seed_node.outputs[0], 13.37))
    noise = node(tree, "ShaderNodeTexNoise", "Chip structure", -800, 500)
    noise.noise_dimensions = "4D"
    noise.inputs["Scale"].default_value = 3.8
    noise.inputs["Detail"].default_value = 3
    noise.inputs["Roughness"].default_value = 0.65
    tree.links.new(coords.outputs[0], noise.inputs["Vector"])
    tree.links.new(w, noise.inputs["W"])
    chips = ramp(tree, noise.outputs["Fac"], "Exposed steel mask", .51, .535,
                 (0, 0, 0), (1, 1, 1), -500, 600)
    color = ramp(tree, chips, "Paint and steel reflectance (linear)", 0, 1,
                 (.025, .105, .15), (.52, .55, .58), -200, 800)
    roughness = ramp(tree, chips, "Paint and steel roughness", 0, 1,
                     (.62, .62, .62), (.3, .3, .3), -200, 500)
    height = math_node(tree, "SUBTRACT", 1, chips, "Paint thickness mask")
    shader = node(tree, "ShaderNodeBsdfPrincipled", "Painted steel surface", 400, 600)
    tree.links.new(color, shader.inputs["Base Color"])
    tree.links.new(roughness, shader.inputs["Roughness"])
    tree.links.new(chips, shader.inputs["Metallic"])
    bump = node(tree, "ShaderNodeBump", "Paint relief 0.15 mm", 100, 0)
    bump.inputs["Distance"].default_value = .00015
    bump.inputs["Strength"].default_value = 1
    tree.links.new(height, bump.inputs["Height"])
    tree.links.new(bump.outputs["Normal"], shader.inputs["Normal"])
    output = node(tree, "ShaderNodeOutputMaterial", "Material output", 800, 600)
    tree.links.new(shader.outputs[0], output.inputs["Surface"])

    # Keep the helper math readable without overlapping it beneath the main graph.
    helpers = [n for n in tree.nodes if n.bl_idname == "ShaderNodeMath"]
    for index, helper in enumerate(helpers):
        helper.location = (-1800 + (index % 5) * 230, -500 - (index // 5) * 180)
    return material


def channel_sockets(material):
    tree = material.node_tree
    nodes = tree.nodes
    shader = nodes["Painted steel surface"]
    temporary_nodes = []

    def source(socket):
        if socket.is_linked:
            upstream = socket.links[0].from_socket
            if socket.type == "VALUE" and upstream.type != "VALUE":
                # Preserve Blender's implicit conversion at the destination:
                # a colored link into Roughness is a scalar, not colored data.
                conversion = node(tree, "ShaderNodeMath", "Bake scalar conversion", 600, -400)
                conversion.operation = "ADD"
                conversion.inputs[1].default_value = 0
                tree.links.new(upstream, conversion.inputs[0])
                temporary_nodes.append(conversion)
                return conversion.outputs[0]
            return upstream
        kind = "ShaderNodeRGB" if socket.type == "RGBA" else "ShaderNodeValue"
        constant = node(tree, kind, "Temporary bake constant", 600, -400)
        constant.outputs[0].default_value = socket.default_value
        temporary_nodes.append(constant)
        return constant.outputs[0]

    sockets = {
        "base_color": source(shader.inputs["Base Color"]),
        "roughness": source(shader.inputs["Roughness"]),
        "metallic": source(shader.inputs["Metallic"]),
        "height": source(nodes["Paint relief 0.15 mm"].inputs["Height"]),
        "chip_mask": nodes["Exposed steel mask"].outputs[0],
    }
    return sockets, temporary_nodes


def remove_data_color_metadata(path):
    # Blender's image.save can tag Non-Color PNG pixels as sRGB. Keep the raw
    # samples while removing colorimetry that would mislead another importer.
    raw = path.read_bytes()
    result = bytearray(raw[:8])
    offset = 8
    color_chunks = {b"sRGB", b"gAMA", b"cHRM", b"iCCP", b"cICP", b"mDCv", b"cLLi"}
    while offset < len(raw):
        length = struct.unpack_from(">I", raw, offset)[0]
        end = offset + length + 12
        if raw[offset + 4:offset + 8] not in color_chunks:
            result.extend(raw[offset:end])
        offset = end
    path.write_bytes(result)


def setup_scene():
    scene = bpy.context.scene
    scene.render.engine = "CYCLES"
    scene.cycles.device = "CPU"
    scene.cycles.samples = 16
    scene.cycles.seed = 0
    scene.cycles.use_denoising = False
    scene.render.threads_mode = "FIXED"
    scene.render.threads = 8
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGB"
    scene.render.image_settings.color_depth = "16"
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.view_settings.exposure = 0
    scene.view_settings.gamma = 1
    scene.render.bake.use_selected_to_active = False
    scene.render.bake.margin = 0
    scene.render.bake.normal_space = "TANGENT"
    scene.render.bake.normal_r = "POS_X"
    scene.render.bake.normal_g = "POS_Y"
    scene.render.bake.normal_b = "POS_Z"
    return scene


def bake(material, output_dir, size):
    bpy.ops.object.select_all(action="DESELECT")
    bpy.ops.mesh.primitive_plane_add(size=.25)
    plane = bpy.context.object
    plane.name = "AuthoringTile_25cm"
    plane.data.materials.append(material)
    tree = material.node_tree
    output = tree.nodes["Material output"]
    shader = tree.nodes["Painted steel surface"]
    emitter = node(tree, "ShaderNodeEmission", "Temporary bake emission", 600, -200)
    target = node(tree, "ShaderNodeTexImage", "Bake target", 800, -200)
    sockets, temporary_nodes = channel_sockets(material)

    records = {}
    for channel in CHANNELS:
        image = bpy.data.images.new(channel, width=size, height=size, alpha=False, float_buffer=True)
        image.colorspace_settings.name = "sRGB" if channel == "base_color" else "Non-Color"
        target.image = image
        tree.nodes.active = target
        if channel == "normal":
            tree.links.new(shader.outputs[0], output.inputs["Surface"])
            bpy.ops.object.bake(type="NORMAL")
        else:
            tree.links.new(sockets[channel], emitter.inputs["Color"])
            tree.links.new(emitter.outputs[0], output.inputs["Surface"])
            bpy.ops.object.bake(type="EMIT")

        image.filepath_raw = str(output_dir / (channel + ".png"))
        image.file_format = "PNG"
        # Image.save writes channel data without the display/view transform.
        image.save()
        if channel != "base_color":
            remove_data_color_metadata(Path(image.filepath_raw))
        records[channel] = {
            "file": channel + ".png", "color_space": image.colorspace_settings.name,
            "sha256": hashlib.sha256(Path(image.filepath_raw).read_bytes()).hexdigest(),
        }

    tree.links.new(shader.outputs[0], output.inputs["Surface"])
    tree.nodes.remove(emitter)
    tree.nodes.remove(target)
    for temporary in temporary_nodes:
        tree.nodes.remove(temporary)
    # Keep a selectable live procedural surface in the editable artifact, but
    # exclude it from the proof renders, which use exported maps exclusively.
    plane.hide_render = True
    return records


def exported_material(output_dir):
    material = bpy.data.materials.new("ExportedMapsOnly")
    material.use_nodes = True
    tree = material.node_tree
    shader = tree.nodes.get("Principled BSDF")
    uv = node(tree, "ShaderNodeTexCoord", "Map UVs", -900, 300)
    for index, channel in enumerate(("base_color", "roughness", "metallic", "normal")):
        texture = node(tree, "ShaderNodeTexImage", channel, -600, 600 - index * 300)
        texture.image = bpy.data.images.load(str(output_dir / (channel + ".png")), check_existing=False)
        texture.image.colorspace_settings.name = "sRGB" if channel == "base_color" else "Non-Color"
        texture.extension = "REPEAT"
        tree.links.new(uv.outputs["UV"], texture.inputs["Vector"])
        if channel == "normal":
            normal = node(tree, "ShaderNodeNormalMap", "OpenGL tangent normals", -250, -300)
            tree.links.new(texture.outputs["Color"], normal.inputs["Color"])
            tree.links.new(normal.outputs[0], shader.inputs["Normal"])
        else:
            slot = {"base_color": "Base Color", "roughness": "Roughness", "metallic": "Metallic"}[channel]
            tree.links.new(texture.outputs["Color"], shader.inputs[slot])
    return material


def aim(obj, point):
    obj.rotation_euler = (Vector(point) - obj.location).to_track_quat("-Z", "Y").to_euler()


def previews(scene, material, output_dir):
    bpy.ops.mesh.primitive_plane_add(size=.5, location=(-.325, 0, 0))
    tile = bpy.context.object
    tile.name = "ExportedMaps_Tiled2x2"
    for loop in tile.data.uv_layers.active.data:
        loop.uv *= 2
    tile.data.materials.append(material)
    bpy.ops.mesh.primitive_uv_sphere_add(segments=64, ring_count=32, radius=.2, location=(.275, 0, .2))
    sphere = bpy.context.object
    sphere.name = "ExportedMaps_Sphere"
    sphere.data.materials.append(material)
    for polygon in sphere.data.polygons:
        polygon.use_smooth = True

    bpy.ops.object.camera_add(location=(.75, -1.5, 1.25))
    camera = bpy.context.object
    camera.data.type = "ORTHO"
    camera.data.ortho_scale = 1.35
    aim(camera, (0, 0, .075))
    scene.camera = camera
    scene.world.use_nodes = True
    scene.world.node_tree.nodes["Background"].inputs[0].default_value = (.12, .12, .12, 1)
    scene.world.node_tree.nodes["Background"].inputs[1].default_value = .5
    bpy.ops.object.light_add(type="AREA", location=(-.375, -.5, 1))
    light = bpy.context.object
    light.data.energy = 31.25
    light.data.shape = "DISK"
    light.data.size = .75
    aim(light, (0, 0, 0))
    scene.render.resolution_x = 800
    scene.render.resolution_y = 520
    scene.render.resolution_percentage = 100
    scene.cycles.samples = 32
    scene.view_settings.view_transform = "AgX"
    scene.render.image_settings.color_depth = "8"
    for name, position in (("preview_key_left", (-.375, -.5, 1)), ("preview_key_right", (.625, .25, .75))):
        light.location = position
        aim(light, (0, 0, 0))
        scene.render.filepath = str(output_dir / (name + ".png"))
        bpy.ops.render.render(write_still=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--seed", type=int, default=17)
    parser.add_argument("--resolution", type=int, default=512)
    parser.add_argument("--source", type=Path, help="Reopen a previously generated editable .blend")
    parser.add_argument("--previews", action="store_true")
    args = parser.parse_args(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
    if not 32 <= args.resolution <= 4096:
        parser.error("resolution must be between 32 and 4096")
    args.output = args.output.resolve()
    if args.output.exists() and any(args.output.iterdir()):
        parser.error("output must be empty; preserve previous run evidence")
    args.output.mkdir(parents=True, exist_ok=True)

    if args.source:
        bpy.ops.wm.open_mainfile(filepath=str(args.source.resolve()))
        material = bpy.data.materials.get(MATERIAL)
        if material is None:
            raise ValueError("source does not contain " + MATERIAL)
        material.node_tree.nodes["Seed"].outputs[0].default_value = args.seed
    else:
        bpy.ops.wm.read_factory_settings(use_empty=True)
        material = create_material(args.seed)
    material.use_fake_user = True
    # Reopened sources may contain a previous preview scene; clear objects only.
    for obj in list(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    scene = setup_scene()
    records = bake(material, args.output, args.resolution)
    if args.previews:
        if scene.world is None:
            scene.world = bpy.data.worlds.new("Neutral studio")
        previews(scene, exported_material(args.output), args.output)

    bpy.ops.object.select_all(action="DESELECT")
    authoring_tile = bpy.data.objects["AuthoringTile_25cm"]
    authoring_tile.select_set(True)
    bpy.context.view_layer.objects.active = authoring_tile

    # Pack preview images so the saved graph/preview survives moving the folder.
    for image in bpy.data.images:
        if image.source == "FILE" and image.has_data:
            image.pack()
    bpy.ops.wm.save_as_mainfile(filepath=str(args.output / "painted_steel.blend"))
    manifest = {
        "recipe": "periodic-painted-steel-v1", "seed": args.seed,
        "resolution": args.resolution, "tile_size_m": .25,
        "height_range_m": [0, .00015], "normal_convention": "tangent +X +Y +Z (OpenGL)",
        "blender": bpy.app.version_string, "device": "Cycles CPU", "png_bit_depth": 16,
        "channels": records, "script_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "source": str(args.source.resolve()) if args.source else None,
        "previews": args.previews,
    }
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print("PBR_EXPORT_COMPLETE " + str(args.output), flush=True)


if __name__ == "__main__":
    main()
