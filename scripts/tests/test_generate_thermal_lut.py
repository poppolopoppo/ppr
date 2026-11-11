"""Regression cases for the offline thermal-emission lookup generator.

The cases use the vendored CIE table and the committed generated assets; there
are no mocked colour samples or table values.
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import math
import struct
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "generate_thermal_lut.py"
SPEC = importlib.util.spec_from_file_location("generate_thermal_lut", SCRIPT)
generator = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = generator
SPEC.loader.exec_module(generator)


class ThermalLutGenerationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.samples = generator.load_cie_samples()
        cls.calibration = generator.calibration_y(cls.samples)
        cls.table, cls.metadata_bytes = generator.generate_artifacts()
        cls.metadata = json.loads(cls.metadata_bytes)

    def test_cie_source_checksum_and_shape(self) -> None:
        self.assertEqual(generator.sha256_file(generator.CIE_CSV), generator.CIE_SHA256)
        self.assertEqual(len(self.samples), 471)
        self.assertEqual(self.samples[0].wavelength_nm, 360.0)
        self.assertEqual(self.samples[-1].wavelength_nm, 830.0)

        # Published and independently checked sample row (479 nm).
        sample = self.samples[479 - 360]
        self.assertAlmostEqual(sample.wavelength_nm, 479.0)
        self.assertAlmostEqual(sample.x_bar, 0.104297900000, places=12)
        self.assertAlmostEqual(sample.y_bar, 0.1334528000000, places=12)
        self.assertAlmostEqual(sample.z_bar, 0.856619300000, places=12)

    def test_calibration_y_matches_spec_reference(self) -> None:
        self.assertLessEqual(abs(self.calibration - 4498563.65869), 1.0e-4)
        calibrated_y = generator.integrate_xyz(self.samples, generator.CALIBRATION_KELVIN)[1] / self.calibration
        self.assertLessEqual(abs(calibrated_y - 1.0), 1.0e-12)

    def test_spec_reference_engine_rgb_rows(self) -> None:
        reference = {
            1500: ((0.0000081848, 0.0000010894, 0.0), 2.5192e-6),
            2000: ((0.000380344, 0.0000976715, 0.00000309325), 0.000150939),
            3000: ((0.0174141, 0.00830468, 0.00267633), 0.00983498),
            4095: ((0.132717, 0.0886975, 0.0531728), 0.0954911),
            6500: ((1.04317, 0.983632, 1.03500), 1.0),
            10000: ((3.76238, 4.29269, 6.17463), 4.31582),
            65504: ((77.0781, 110.264, 238.496), 112.467),
        }
        for kelvin, (expected_rgb, expected_y) in reference.items():
            with self.subTest(kelvin=kelvin):
                rgb = generator.engine_rgb(self.samples, kelvin, self.calibration)
                luminance = generator.integrate_xyz(self.samples, kelvin)[1] / self.calibration
                self.assertLessEqual(abs(luminance - expected_y), abs(expected_y) * 1.0e-4 + 1.0e-12)
                for component, expected in zip(rgb, expected_rgb):
                    self.assertLessEqual(abs(component - expected), abs(expected) * 1.0e-4 + 1.0e-12)

    def test_generated_table_shape_stride_and_padding(self) -> None:
        self.assertEqual(len(self.table), generator.TABLE_BYTES)
        self.assertEqual(generator.TABLE_BYTES, 32768)
        self.assertEqual(generator.TABLE_STRIDE_BYTES, 16)
        for index in range(generator.TABLE_ROWS):
            padding = self.table[index * generator.TABLE_STRIDE_BYTES + 12 : index * generator.TABLE_STRIDE_BYTES + 16]
            self.assertEqual(padding, b"\x00\x00\x00\x00")

    def test_row_temperatures_are_logarithmic_endpoints(self) -> None:
        self.assertEqual(generator.row_temperature(0), generator.MIN_KELVIN)
        self.assertAlmostEqual(generator.row_temperature(generator.TABLE_ROWS - 1), generator.MAX_KELVIN, places=9)
        midpoint = generator.TABLE_ROWS // 2
        product = generator.row_temperature(midpoint - 1) * generator.row_temperature(midpoint)
        geometric = generator.MIN_KELVIN * generator.MAX_KELVIN
        self.assertLessEqual(abs(product / geometric - 1.0), 1.0e-9)

    def test_committed_artifacts_match_regenerated_bytes(self) -> None:
        committed_table = (generator.ASSET_DIRECTORY / generator.TABLE_FILE).read_bytes()
        committed_metadata = (generator.ASSET_DIRECTORY / generator.METADATA_FILE).read_bytes()
        self.assertEqual(committed_table, self.table)
        self.assertEqual(committed_metadata, self.metadata_bytes)

    def test_metadata_hashes_and_provenance(self) -> None:
        source = self.metadata["source"]
        table = self.metadata["table"]
        self.assertEqual(source["doi"], generator.CIE_DOI)
        self.assertEqual(source["sha256"], generator.CIE_SHA256)
        self.assertEqual(source["license"], generator.CIE_LICENSE)
        self.assertEqual(table["sha256"], generator.sha256_bytes(self.table))
        self.assertEqual(table["rows"], generator.TABLE_ROWS)
        self.assertEqual(table["stride_bytes"], generator.TABLE_STRIDE_BYTES)
        self.assertEqual(table["bytes"], generator.TABLE_BYTES)
        self.assertLessEqual(abs(self.metadata["integrator"]["calibration_y"] - self.calibration), 1.0e-9)

    def test_frozen_cpp_reference_matches_csv(self) -> None:
        include = Path(__file__).resolve().parents[2] / "third_party" / "CIE" / "CIE_xyz_1931_2deg_reference.inc"
        rows = []
        for line in include.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if not line or line.startswith("//"):
                continue
            values = line.rstrip(",").strip("{}")
            rows.append(tuple(float(part) for part in values.split(",")))

        self.assertEqual(len(rows), len(self.samples))
        for row, sample in zip(rows, self.samples):
            self.assertEqual(row, (sample.wavelength_nm, sample.x_bar, sample.y_bar, sample.z_bar))

    def test_check_verifies_committed_artifacts(self) -> None:
        with contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(generator.main(["--check"]), 0)
        self.assertIn("thermal LUT check passed", output.getvalue())

    def test_output_regenerates_identical_artifacts(self) -> None:
        with tempfile.TemporaryDirectory(prefix="ppr-thermal-lut-") as temporary:
            directory = Path(temporary)
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(generator.main(["--output", str(directory)]), 0)
            self.assertEqual((directory / generator.TABLE_FILE).read_bytes(), self.table)
            self.assertEqual((directory / generator.METADATA_FILE).read_bytes(), self.metadata_bytes)

    def test_sampling_cutoff_and_no_per_temperature_normalization(self) -> None:
        def sample(kelvin: float) -> tuple[float, float, float]:
            if kelvin <= generator.MIN_KELVIN:
                return (0.0, 0.0, 0.0)
            q = (generator.TABLE_ROWS - 1) * math.log(kelvin / generator.MIN_KELVIN) / math.log(
                generator.MAX_KELVIN / generator.MIN_KELVIN
            )
            lower = min(int(math.floor(q)), generator.TABLE_ROWS - 1)
            upper = min(lower + 1, generator.TABLE_ROWS - 1)
            fraction = q - lower
            rows = []
            for index in (lower, upper):
                red, green, blue, _ = struct.unpack_from("<4f", self.table, index * generator.TABLE_STRIDE_BYTES)
                rows.append((red, green, blue))
            return tuple(a + (b - a) * fraction for a, b in zip(*rows))

        self.assertEqual(sample(0.0), (0.0, 0.0, 0.0))
        self.assertEqual(sample(256.0), (0.0, 0.0, 0.0))

        for kelvin in (300.0, 1000.0, 3000.0, 6500.0, 10000.0, 50000.0):
            with self.subTest(kelvin=kelvin):
                exact = generator.engine_rgb(self.samples, kelvin, self.calibration)
                approximate = sample(kelvin)
                error = max(abs(a - b) for a, b in zip(exact, approximate))
                self.assertLessEqual(error / max(max(exact), 1.0e-6), 1.0e-3)

        # Absolute brightness is retained: a hotter black body is brighter.
        self.assertLess(
            sum(sample(3000.0)),
            sum(sample(6500.0)),
        )
        self.assertLess(
            sum(sample(6500.0)),
            sum(sample(10000.0)),
        )


if __name__ == "__main__":
    unittest.main()
