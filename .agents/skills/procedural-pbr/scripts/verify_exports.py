"""Run with Blender Python; compare decoded 16-bit exports and fixture contracts."""

import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

import bpy
import numpy as np


def read_channel(path, size):
    raw = path.read_bytes()
    header = raw[:26]
    if header[:8] != b"\x89PNG\r\n\x1a\n" or header[24] != 16:
        raise AssertionError(f"Expected 16-bit PNG: {path}")
    offset = 8
    chunks = set()
    while offset < len(raw):
        length = struct.unpack_from(">I", raw, offset)[0]
        chunks.add(raw[offset + 4:offset + 8])
        offset += length + 12
    color_chunks = {b"sRGB", b"gAMA", b"cHRM", b"iCCP", b"cICP", b"mDCv", b"cLLi"}
    if path.stem != "base_color" and chunks & color_chunks:
        raise AssertionError(f"Data PNG has misleading color metadata: {path}")
    if path.stem == "base_color" and b"sRGB" not in chunks:
        raise AssertionError(f"Base color missing sRGB metadata: {path}")
    image = bpy.data.images.load(str(path), check_existing=False)
    image.colorspace_settings.name = "Non-Color"
    if tuple(image.size) != (size, size):
        raise AssertionError(f"Unexpected dimensions: {path}")
    values = np.empty(size * size * 4, dtype=np.float32)
    image.pixels.foreach_get(values)
    values = values.reshape(size, size, 4)[:, :, :3].copy()
    bpy.data.images.remove(image)
    if not np.isfinite(values).all() or values.min() < 0 or values.max() > 1:
        raise AssertionError(f"Invalid channel range: {path}")
    return values


def load_run(path):
    manifest = json.loads((path / "manifest.json").read_text())
    channels = {}
    for channel, record in manifest["channels"].items():
        file = path / record["file"]
        if hashlib.sha256(file.read_bytes()).hexdigest() != record["sha256"]:
            raise AssertionError(f"File hash mismatch: {file}")
        channels[channel] = read_channel(file, manifest["resolution"])
    return manifest, channels


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("first", type=Path)
    parser.add_argument("repeat", type=Path)
    parser.add_argument("variant", type=Path)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args(sys.argv[sys.argv.index("--") + 1:])
    a_meta, a = load_run(args.first)
    b_meta, b = load_run(args.repeat)
    c_meta, c = load_run(args.variant)
    require(a_meta["seed"] == b_meta["seed"] != c_meta["seed"], "Incorrect trial seeds")
    require(a_meta["resolution"] == b_meta["resolution"] == c_meta["resolution"], "Resolution mismatch")
    report = {"status": "pass", "blender": bpy.app.version_string, "channels": {}}
    for name, values in a.items():
        require(np.array_equal(values, b[name]), f"Decoded repeat differs: {name}")
        changed = float(np.mean(np.any(values != c[name], axis=2)))
        require(changed > 0, f"Seed does not change {name}")
        report["channels"][name] = {
            "repeat_identical": True, "variant_changed_fraction": changed,
            "min": float(values.min()), "max": float(values.max()),
        }

    # Shared-mask contracts must hold for both seeds, independent of visual QA.
    for label, channels in (("first", a), ("variant", c)):
        mask = channels["chip_mask"][:, :, 0]
        # A gray RGB->VALUE luminance conversion can round by a PNG code step.
        require(float(np.max(abs(channels["metallic"] - channels["chip_mask"]))) < 4 / 65535,
                "Metal mask mismatch")
        require(float(np.max(abs(channels["height"][:, :, 0] + mask - 1))) < 4 / 65535,
                "Height does not invert the chip mask")
        require(float(np.max(abs(channels["roughness"][:, :, 0] - (.62 - .32 * mask)))) < 4 / 65535,
                "Roughness does not follow chip mask")
        color = channels["base_color"]
        linear = np.where(color <= .04045, color / 12.92, ((color + .055) / 1.055) ** 2.4)
        expected = np.array([.025, .105, .15]) + mask[:, :, None] * np.array([.495, .445, .43])
        require(float(np.max(abs(linear - expected))) < .0002, "Base color/mask/color-space mismatch")
        require(np.any(mask < .01) and np.any(mask > .99), "Missing paint or exposed metal")

        normals = channels["normal"] * 2 - 1
        length_error = float(np.max(abs(np.linalg.norm(normals, axis=2) - 1)))
        require(length_error < .001 and normals[:, :, 2].min() > 0, "Invalid tangent normals")
        height = channels["height"][:, :, 0]
        dy, dx = np.gradient(height)
        dot = -dx * normals[:, :, 0] - dy * normals[:, :, 1]
        # A finite-difference height sample can straddle a chip edge while the
        # shader's local derivative is flat. Encoded 0.5 is also biased by one
        # quantization step in a 16-bit normal. Compare orientation only where
        # both fields have a resolvable direction, not that quantization noise.
        active = (dx * dx + dy * dy > .0001) & (
            normals[:, :, 0] ** 2 + normals[:, :, 1] ** 2 > (8 / 65535) ** 2)
        require(np.count_nonzero(active) > 0, "No resolvable relief normals")
        agreement = float(np.mean(dot[active] > 0))
        require(agreement > .9, "Normal relief orientation disagrees with height")
        report[label + "_normal_orientation_agreement"] = agreement
        report[label + "_normal_max_length_error"] = length_error

    # First/last pixel centers are adjacent samples, not the same UV position.
    # Compare their jump with ordinary internal pixel jumps; exact border equality
    # would be an incorrect requirement. Periodicity also follows from the torus graph.
    report["seams"] = {}
    for name in ("base_color", "height", "normal"):
        values = a[name]
        metrics = {}
        for axis, label in ((0, "v"), (1, "u")):
            edge = float(np.sqrt(np.mean((np.take(values, 0, axis=axis) -
                                         np.take(values, -1, axis=axis)) ** 2)))
            difference = np.diff(values, axis=axis)
            reduction = tuple(i for i in range(3) if i != axis)
            internal = np.sqrt(np.mean(difference ** 2, axis=reduction))
            p95 = float(np.percentile(internal, 95))
            require(edge <= 2 * p95 + 1 / 65535, f"Suspicious {name} seam on {label}")
            metrics[label] = {"edge_rms": edge, "interior_p95_rms": p95}
        report["seams"][name] = metrics

    report["visual_review"] = "Separate inspection required: both relit previews, exported maps only"
    report["scope"] = "Reference painted-steel fixture on this Blender build and Linux host"
    args.report.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
