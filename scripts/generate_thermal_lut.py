"""Generate the calibrated visible-band thermal-emission lookup table.

The table is a fixed, offline artifact: ordinary builds and startup never
download the CIE colour-matching data and never rerun this integration. The
generator reads the vendored CIE 1931 2-degree table, verifies its pinned
SHA-256, and writes ``thermal_lut.bin`` plus the reproducible metadata in
``thermal_lut.json``.

Usage:
    python3 scripts/generate_thermal_lut.py --check
    python3 scripts/generate_thermal_lut.py --output <directory>

``--check`` regenerates in memory and verifies the committed bytes/metadata.
``--output`` writes regenerated artifacts into an arbitrary offline directory.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
CIE_CSV = REPOSITORY_ROOT / "third_party" / "CIE" / "CIE_xyz_1931_2deg.csv"
ASSET_DIRECTORY = REPOSITORY_ROOT / "assets" / "lighting"
TABLE_FILE = "thermal_lut.bin"
METADATA_FILE = "thermal_lut.json"

# CIE 1931 2-degree observer, 360-830 nm at 1 nm:
# https://doi.org/10.25039/CIE.DS.xvudnb9b (CC BY-SA 4.0).
CIE_SHA256 = "fa663e3535a7e0763a745993a1f0a192eb0275ac46ad2d1befd7626841e713c1"
CIE_DOI = "10.25039/CIE.DS.xvudnb9b"
CIE_LICENSE = "CC BY-SA 4.0"
CIE_LICENSE_URL = "https://creativecommons.org/licenses/by-sa/4.0/"
CIE_ATTRIBUTION = "International Commission on Illumination (CIE), Vienna, AT"
CIE_WAVELENGTHS_NM = (360, 830)
CIE_WAVELENGTH_STEP_NM = 1
CIE_SAMPLE_COUNT = 471

PLANCK_H = 6.62607015e-34
PLANCK_C = 299792458.0
BOLTZMANN_K = 1.380649e-23
NANOMETRE = 1e-9
EXPONENT_CUTOFF = 700.0

SRGB_FROM_XYZ = (
    (3.2406, -1.5372, -0.4986),
    (-0.9689, 1.8758, 0.0415),
    (0.0557, -0.2040, 1.0570),
)
LUMINANCE_WEIGHTS = (0.2126, 0.7152, 0.0722)

CALIBRATION_KELVIN = 6500.0
TABLE_ROWS = 2048
TABLE_STRIDE_BYTES = 16
TABLE_BYTES = TABLE_ROWS * TABLE_STRIDE_BYTES
MIN_KELVIN = 256.0
MAX_KELVIN = 65504.0


@dataclass(frozen=True)
class CieSample:
    wavelength_nm: float
    x_bar: float
    y_bar: float
    z_bar: float


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    return sha256_bytes(path.read_bytes())


def load_cie_samples(path: Path = CIE_CSV) -> list[CieSample]:
    """Load and validate the pinned CIE colour-matching table."""
    digest = sha256_file(path)
    if digest != CIE_SHA256:
        raise ValueError(f"{path} SHA-256 {digest} does not match pinned {CIE_SHA256}")

    samples: list[CieSample] = []
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        parts = line.split(",")
        if len(parts) != 4:
            raise ValueError(f"{path}:{line_number}: expected four comma-separated values")
        samples.append(CieSample(*(float(part) for part in parts)))

    expected_wavelengths = range(
        CIE_WAVELENGTHS_NM[0],
        CIE_WAVELENGTHS_NM[1] + CIE_WAVELENGTH_STEP_NM,
        CIE_WAVELENGTH_STEP_NM,
    )
    if len(samples) != CIE_SAMPLE_COUNT:
        raise ValueError(f"{path}: expected {CIE_SAMPLE_COUNT} samples, found {len(samples)}")
    for sample, wavelength in zip(samples, expected_wavelengths):
        if sample.wavelength_nm != float(wavelength):
            raise ValueError(f"{path}: expected {wavelength} nm, found {sample.wavelength_nm} nm")

    return samples


def planck_spectral_radiance(wavelength_m: float, kelvin: float) -> float:
    """Binary64 Planck spectral radiance; T<=0 and negligible exponents are zero."""
    if kelvin <= 0.0:
        return 0.0

    exponent = PLANCK_H * PLANCK_C / (wavelength_m * BOLTZMANN_K * kelvin)
    if exponent > EXPONENT_CUTOFF:
        return 0.0

    return 2.0 * PLANCK_H * PLANCK_C * PLANCK_C / (
        wavelength_m**5 * math.expm1(exponent)
    )


def integrate_xyz(samples: list[CieSample], kelvin: float) -> tuple[float, float, float]:
    """Trapezoidal visible-band integration with wavelength in metres."""
    if kelvin <= 0.0:
        return (0.0, 0.0, 0.0)

    x = y = z = 0.0
    last = len(samples) - 1
    for index, sample in enumerate(samples):
        weight = 0.5 if index == 0 or index == last else 1.0
        radiance = planck_spectral_radiance(sample.wavelength_nm * NANOMETRE, kelvin)
        scale = radiance * weight * NANOMETRE
        x += scale * sample.x_bar
        y += scale * sample.y_bar
        z += scale * sample.z_bar

    return (x, y, z)


def linear_srgb(xyz: tuple[float, float, float]) -> tuple[float, float, float]:
    """Explicit sRGB matrix, nonnegative-gamut projection and photopic scaling."""
    x, y, z = xyz
    if x == 0.0 and y == 0.0 and z == 0.0:
        return (0.0, 0.0, 0.0)

    linear = [
        max(SRGB_FROM_XYZ[0][0] * x + SRGB_FROM_XYZ[0][1] * y + SRGB_FROM_XYZ[0][2] * z, 0.0),
        max(SRGB_FROM_XYZ[1][0] * x + SRGB_FROM_XYZ[1][1] * y + SRGB_FROM_XYZ[1][2] * z, 0.0),
        max(SRGB_FROM_XYZ[2][0] * x + SRGB_FROM_XYZ[2][1] * y + SRGB_FROM_XYZ[2][2] * z, 0.0),
    ]
    luminance = LUMINANCE_WEIGHTS[0] * linear[0] + LUMINANCE_WEIGHTS[1] * linear[1] + LUMINANCE_WEIGHTS[2] * linear[2]
    if luminance <= 0.0:
        return (0.0, 0.0, 0.0)

    scale = y / luminance
    return (linear[0] * scale, linear[1] * scale, linear[2] * scale)


def engine_rgb(samples: list[CieSample], kelvin: float, calibration_y: float) -> tuple[float, float, float]:
    """Calibrated engine-unit RGB: a 6500 K black body has luminance one."""
    if kelvin <= 0.0:
        return (0.0, 0.0, 0.0)

    return tuple(component / calibration_y for component in linear_srgb(integrate_xyz(samples, kelvin)))


def calibration_y(samples: list[CieSample]) -> float:
    return integrate_xyz(samples, CALIBRATION_KELVIN)[1]


def row_temperature(index: int) -> float:
    return MIN_KELVIN * (MAX_KELVIN / MIN_KELVIN) ** (index / (TABLE_ROWS - 1))


def generate_table_bytes(samples: list[CieSample]) -> bytes:
    reference_y = calibration_y(samples)
    rows = bytearray()
    for index in range(TABLE_ROWS):
        temperature = row_temperature(index)
        red, green, blue = engine_rgb(samples, temperature, reference_y)
        rows += struct.pack("<4f", red, green, blue, 0.0)
    if len(rows) != TABLE_BYTES:
        raise ValueError(f"generated {len(rows)} bytes, expected {TABLE_BYTES}")
    return bytes(rows)


def build_metadata(samples: list[CieSample], table: bytes) -> dict[str, object]:
    return {
        "schema_version": 1,
        "generated_by": "scripts/generate_thermal_lut.py",
        "source": {
            "title": "Colour-matching functions of CIE 1931 standard colorimetric observer",
            "doi": CIE_DOI,
            "file": "CIE_xyz_1931_2deg.csv",
            "sha256": CIE_SHA256,
            "license": CIE_LICENSE,
            "license_url": CIE_LICENSE_URL,
            "attribution": CIE_ATTRIBUTION,
            "wavelength_first_nm": CIE_WAVELENGTHS_NM[0],
            "wavelength_last_nm": CIE_WAVELENGTHS_NM[1],
            "wavelength_step_nm": CIE_WAVELENGTH_STEP_NM,
            "sample_count": len(samples),
        },
        "integrator": {
            "precision": "binary64",
            "wavelength_unit": "m",
            "quadrature": "trapezoid with endpoint half weights",
            "planck_constants": {
                "h": PLANCK_H,
                "c": PLANCK_C,
                "k": BOLTZMANN_K,
            },
            "exponent_cutoff": EXPONENT_CUTOFF,
            "zero_kelvin": "zero",
            "srgb_from_xyz": [list(row) for row in SRGB_FROM_XYZ],
            "negative_gamut": "project each negative component to zero",
            "luminance_weights": list(LUMINANCE_WEIGHTS),
            "calibration_kelvin": CALIBRATION_KELVIN,
            "calibration_y": calibration_y(samples),
            "calibration": "divide every component by Y(6500 K); 6500 K has engine luminance one",
        },
        "table": {
            "file": TABLE_FILE,
            "rows": TABLE_ROWS,
            "stride_bytes": TABLE_STRIDE_BYTES,
            "bytes": TABLE_BYTES,
            "format": "binary32 linear RGB plus one zero padding word",
            "temperature_formula": "256*(65504/256)^(i/2047)",
            "temperature_first_kelvin": MIN_KELVIN,
            "temperature_last_kelvin": MAX_KELVIN,
            "sha256": sha256_bytes(table),
        },
    }


def encode_metadata(metadata: dict[str, object]) -> bytes:
    return (json.dumps(metadata, indent=2, ensure_ascii=True) + "\n").encode("utf-8")


def generate_artifacts(csv_path: Path = CIE_CSV) -> tuple[bytes, bytes]:
    samples = load_cie_samples(csv_path)
    table = generate_table_bytes(samples)
    metadata = encode_metadata(build_metadata(samples, table))
    return table, metadata


def write_artifacts(directory: Path, table: bytes, metadata: bytes) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    (directory / TABLE_FILE).write_bytes(table)
    (directory / METADATA_FILE).write_bytes(metadata)


def check_committed_artifacts(directory: Path = ASSET_DIRECTORY) -> list[str]:
    table, metadata = generate_artifacts()
    problems: list[str] = []

    table_path = directory / TABLE_FILE
    metadata_path = directory / METADATA_FILE
    if not table_path.is_file():
        problems.append(f"missing {table_path}")
    elif table_path.read_bytes() != table:
        problems.append(
            f"{table_path} differs from regenerated bytes (committed sha256 "
            f"{sha256_file(table_path)}, expected {sha256_bytes(table)})"
        )
    if not metadata_path.is_file():
        problems.append(f"missing {metadata_path}")
    elif metadata_path.read_bytes() != metadata:
        problems.append(f"{metadata_path} differs from regenerated metadata")

    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check",
        action="store_true",
        help="verify committed table bytes and metadata without writing files",
    )
    parser.add_argument(
        "--output",
        metavar="DIR",
        type=Path,
        help="write regenerated artifacts into DIR instead of checking them",
    )
    arguments = parser.parse_args(argv)

    if arguments.check == (arguments.output is not None):
        parser.error("choose exactly one of --check or --output DIR")

    try:
        table, metadata = generate_artifacts()
    except (OSError, ValueError) as error:
        print(f"thermal LUT generation failed: {error}", file=sys.stderr)
        return 1

    if arguments.output is not None:
        write_artifacts(arguments.output, table, metadata)
        print(
            f"wrote {arguments.output / TABLE_FILE} ({len(table)} bytes, sha256 "
            f"{sha256_bytes(table)}) and {arguments.output / METADATA_FILE}"
        )
        return 0

    problems = check_committed_artifacts()
    if problems:
        for problem in problems:
            print(f"thermal LUT check failed: {problem}", file=sys.stderr)
        return 1

    print(
        f"thermal LUT check passed: {TABLE_ROWS} rows, {TABLE_STRIDE_BYTES}-byte stride, "
        f"{TABLE_BYTES} bytes, sha256 {sha256_bytes(table)}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
