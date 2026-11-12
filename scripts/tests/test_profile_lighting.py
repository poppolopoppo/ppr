"""Tests for the bounded lighting profiling tool.

The vendor parser tests use retained real Nsight exports; capture supervision
tests use controlled local processes and fake CLIs, never GUI automation.
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import os
import shutil
import sqlite3
import stat
import subprocess
import sys
import tempfile
import textwrap
import time
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "profile_lighting.py"
SPEC = importlib.util.spec_from_file_location("profile_lighting", SCRIPT)
lighting = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = lighting
SPEC.loader.exec_module(lighting)

FIXTURES = Path(__file__).resolve().parent / "fixtures" / "lighting-profile"


def copy_graphics_fixture(destination: Path, source: Path | None = None) -> Path:
    shutil.copytree(source or (FIXTURES / "graphics"), destination)
    return destination


def copy_systems_fixture(destination: Path) -> Path:
    destination.mkdir(parents=True, exist_ok=True)
    shutil.copy2(FIXTURES / "systems" / "systems.sqlite", destination / "systems.sqlite")
    return destination


def write_capture_identity(directory: Path, tool: str) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    ready = {
        "schema_version": 1,
        "pid": 1234,
        "tool": tool,
        "workload": "frozen",
        "session_token": "session-test",
        "world_id": 7,
        "publication_id": 11,
        "tick": 0,
        "accepted_scene_frames": 120,
        "active_presentation_seconds": 2.5,
        "lighting_frame_index": 40,
        "screen_frame_index": 40,
        "camera": {"origin": [1.0, 2.0, 3.0], "forward": [0.0, 0.0, 1.0]},
        "extents": {"logical": [1280, 720], "framebuffer": [1280, 720]},
        "settings": [{"name": "worldgen_profile", "value": "lighting_lab"}],
    }
    final_state = {
        "schema_version": 1,
        "pid": 1234,
        "tool": tool,
        "workload": "frozen",
        "session_token": "session-test",
        "status": "complete",
        "reason": "matching session stop request",
        "ready": True,
        "accepted_scene_frames": 300,
        "ready_tick": 0,
        "stop_tick": 0,
        "lighting_frame_start": 40,
        "lighting_frame_end": 340,
        "screen_frame_start": 40,
        "screen_frame_end": 340,
    }
    manifest = {
        "schema_version": 1,
        "identity": {
            "source_commit": "deadbeef",
            "executable_sha256": "0" * 64,
            "executable_bytes": 1,
            "build_preset": "clang-rel-profile",
        },
        "environment": {
            "os": "linux",
            "gpu": {"name": "NVIDIA GeForce RTX 2080", "driver_version": "620.70.06", "architecture": "Turing"},
        },
        "tools": {"systems": {"version": "2026.5.1"}, "graphics": {"version": "2026.4"}},
        "capture": {"tool": tool, "workload": "frozen"},
        "collection": {"interval_seconds": 2.0, "requested_frames": 8 if tool == "graphics" else None},
        "artifacts": {},
    }
    (directory / "ready.json").write_text(json.dumps(ready), encoding="utf-8")
    (directory / "final-state.json").write_text(json.dumps(final_state), encoding="utf-8")
    (directory / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")


def fake_app_script(directory: Path) -> Path:
    path = directory / "fake_app.py"
    path.write_text(
        textwrap.dedent(
            """\
            #!/usr/bin/env python3
            import json, os, pathlib, sys, time

            output = None
            for argument in sys.argv[1:]:
                if argument.startswith("--profile-output="):
                    output = pathlib.Path(argument.split("=", 1)[1])
            mode = os.environ.get("PPR_FAKE_APP_MODE", "silent")
            order = os.environ.get("PPR_FAKE_ORDER_LOG")
            def note(event):
                if order:
                    with open(order, "a", encoding="utf-8") as stream:
                        stream.write(event + "\\n")
            if output is None:
                sys.exit(2)
            if mode == "silent":
                time.sleep(60)
                sys.exit(0)
            ready = {
                "schema_version": 1, "pid": os.getpid(), "tool": "systems", "workload": "frozen",
                "session_token": "session-fake", "world_id": 3, "publication_id": 4, "tick": 0,
                "accepted_scene_frames": 120, "active_presentation_seconds": 2.5,
                "lighting_frame_index": 1, "screen_frame_index": 1,
                "camera": {"origin": [0, 0, 0], "forward": [0, 0, 1]},
                "extents": {"logical": [1280, 720], "framebuffer": [1280, 720]},
                "settings": [{"name": "worldgen_profile", "value": "lighting_lab"}],
            }
            (output / "ready.json").write_text(json.dumps(ready))
            request = output / "stop.request"
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                if request.is_file() and request.read_text().strip() == "session-fake":
                    note("quit")
                    break
                time.sleep(0.05)
            final = {
                "schema_version": 1, "pid": os.getpid(), "tool": "systems", "workload": "frozen",
                "session_token": "session-fake", "status": "complete", "ready": True,
                "accepted_scene_frames": 240, "ready_tick": 0, "stop_tick": 0,
                "lighting_frame_start": 1, "lighting_frame_end": 120,
                "screen_frame_start": 1, "screen_frame_end": 120,
                "reason": "matching session stop request",
            }
            (output / "final-state.json").write_text(json.dumps(final))
            sys.exit(0)
            """
        ),
        encoding="utf-8",
    )
    path.chmod(path.stat().st_mode | stat.S_IXUSR)
    return path


def fake_nsys_script(directory: Path, app: Path, systems_fixture: Path) -> Path:
    path = directory / "fake_nsys.py"
    path.write_text(
        textwrap.dedent(
            f"""\
            #!/usr/bin/env python3
            import os, shutil, subprocess, sys
            from pathlib import Path

            APP = Path({str(app)!r})
            FIXTURE = Path({str(systems_fixture)!r})
            arguments = sys.argv[1:]
            if "--version" in arguments:
                print("fake Nsight Systems 2026.5.1 (controlled test CLI)")
                sys.exit(0)
            command = arguments[0]
            if command == "launch":
                passthrough = arguments[1:]
                while passthrough and passthrough[0].startswith("--"):
                    passthrough = passthrough[1:]
                completed = subprocess.run([sys.executable, str(APP), *passthrough])
                sys.exit(completed.returncode)
            if command == "start":
                output = Path(next(a.split("=", 1)[1] for a in arguments if a.startswith("--output=")))
                output.with_suffix(".nsys-rep").write_bytes(b"fake native report")
                sys.exit(0)
            if command == "stop":
                sys.exit(0)
            if command == "export":
                order = os.environ.get("PPR_FAKE_ORDER_LOG")
                if order:
                    with open(order, "a", encoding="utf-8") as stream:
                        stream.write("export\\n")
                if os.environ.get("PPR_FAKE_EXPORT_FAIL") == "1":
                    sys.exit(1)
                output = Path(next(a.split("=", 1)[1] for a in arguments if a.startswith("--output=")))
                shutil.copy2(FIXTURE, output)
                sys.exit(0)
            sys.exit(2)
            """
        ),
        encoding="utf-8",
    )
    path.chmod(path.stat().st_mode | stat.S_IXUSR)
    return path


class GraphicsAnalysisTests(unittest.TestCase):
    def setUp(self) -> None:
        self._temporary = tempfile.TemporaryDirectory(prefix="ppr-profile-test-")
        self.root = Path(self._temporary.name)

    def tearDown(self) -> None:
        self._temporary.cleanup()

    def test_real_fixture_normalizes_markers_and_metrics(self) -> None:
        capture_dir = self.root / "capture"
        copy_graphics_fixture(capture_dir / "graphics")
        write_capture_identity(capture_dir, "graphics")

        analysis = lighting.analyze(capture_dir, self.root / "analysis")

        self.assertEqual(analysis["schema_version"], 1)
        self.assertEqual(analysis["collection"]["observed_complete_frames"], 8)
        self.assertIn("ppr", analysis["validation"]["observed_markers"])
        self.assertIn("WorldLighting.ResolveCells", analysis["validation"]["observed_markers"])
        self.assertIn("Present.UI", analysis["validation"]["observed_markers"])
        self.assertTrue(analysis["validation"]["required_ranges_present"])
        self.assertEqual(analysis["validation"]["missing_markers"], [])
        # Frozen pauses before the first tick, so the solver marker is optional.
        self.assertEqual(analysis["validation"]["missing_optional_markers"], ["PBMPM.Tick"])
        self.assertEqual(analysis["validation"]["complete_abc_frames"], list(range(8)))
        self.assertEqual(analysis["validation"]["overall_status"], "complete")

        marker_ranges = [entry for entry in analysis["ranges"] if entry["stable_name"] == "scene.stage"]
        self.assertTrue(marker_ranges)
        self.assertTrue(all(entry["duration_ns"] > 0 for entry in marker_ranges))
        # Nested sub-pass ranges retain explicit parent links.
        self.assertTrue(any(entry["parent_id"] is not None for entry in analysis["ranges"]))
        frame_metrics = [entry for entry in analysis["metrics"] if entry["name"] == "gpu.frame_time"]
        self.assertEqual(len(frame_metrics), 8)
        self.assertEqual(frame_metrics[0]["unit"], "ms")

        yaml_text = (self.root / "analysis" / "analysis.yaml").read_text(encoding="utf-8")
        self.assertIn("schema_version: 1", yaml_text)
        self.assertIn("missing_markers:", yaml_text)

    def test_analysis_replay_is_identical_and_raw_exports_untouched(self) -> None:
        capture_dir = self.root / "capture"
        graphics_root = copy_graphics_fixture(capture_dir / "graphics")
        write_capture_identity(capture_dir, "graphics")
        raw_before = {path.name: path.read_bytes() for path in graphics_root.rglob("*") if path.is_file()}

        first = lighting.analyze(capture_dir, self.root / "first")
        second = lighting.analyze(capture_dir, self.root / "second")

        raw_after = {path.name: path.read_bytes() for path in graphics_root.rglob("*") if path.is_file()}
        self.assertEqual(raw_before, raw_after)
        self.assertEqual(
            (self.root / "first" / "analysis.json").read_bytes(),
            (self.root / "second" / "analysis.json").read_bytes(),
        )
        self.assertEqual(first["metrics"], second["metrics"])
        self.assertEqual(first["ranges"], second["ranges"])

    def test_decimal_comma_requires_a_declared_locale(self) -> None:
        capture_dir = self.root / "capture"
        copy_graphics_fixture(capture_dir / "graphics", FIXTURES / "graphics-comma")
        write_capture_identity(capture_dir, "graphics")

        with self.assertRaises(lighting.ProfileError) as raised:
            lighting.analyze(capture_dir, self.root / "point")
        self.assertEqual(raised.exception.cause, "locale-mismatch")

        analysis = lighting.analyze(capture_dir, self.root / "comma", number_format="comma")
        self.assertEqual(analysis["identity"]["number_format"], "comma")
        frame_metrics = [entry for entry in analysis["metrics"] if entry["name"] == "gpu.frame_time"]
        self.assertEqual(len(frame_metrics), 8)
        point_capture = self.root / "point-capture"
        copy_graphics_fixture(point_capture / "graphics")
        write_capture_identity(point_capture, "graphics")
        point = lighting.analyze(point_capture, self.root / "point-analysis")
        point_frames = [entry for entry in point["metrics"] if entry["name"] == "gpu.frame_time"]
        for comma_entry, point_entry in zip(frame_metrics, point_frames):
            self.assertAlmostEqual(comma_entry["value"], point_entry["value"], places=9)

    def test_missing_metrics_are_null_with_reason(self) -> None:
        capture_dir = self.root / "capture"
        copy_graphics_fixture(capture_dir / "graphics")
        write_capture_identity(capture_dir, "graphics")
        analysis = lighting.analyze(capture_dir, self.root / "analysis")

        unavailable = [entry for entry in analysis["metrics"] if entry["availability"] != "observed"]
        self.assertTrue(unavailable)
        for entry in unavailable:
            self.assertIsNone(entry["value"])
            self.assertTrue(entry["reason"])
            self.assertTrue(entry["name"].startswith("unavailable."))
        # A missing family is null+reason, never a zero or a misclassified tag.
        names = {entry["name"] for entry in unavailable}
        self.assertTrue(names <= {f"unavailable.{family}" for family in lighting.DESIRED_METRIC_FAMILIES})

    def test_unknown_graphics_schema_fails(self) -> None:
        capture_dir = self.root / "capture"
        graphics_root = copy_graphics_fixture(capture_dir / "graphics")
        write_capture_identity(capture_dir, "graphics")
        (graphics_root / "BASE_UNLOCKED" / "REPRO_INFO.xls").unlink()

        with self.assertRaises(lighting.ProfileError) as raised:
            lighting.analyze(capture_dir, self.root / "analysis")
        self.assertEqual(raised.exception.cause, "unknown-schema")

    def test_identity_mismatch_is_labeled(self) -> None:
        baseline = self.root / "baseline"
        candidate = self.root / "candidate"
        for directory, camera_origin in ((baseline, [1.0, 2.0, 3.0]), (candidate, [9.0, 9.0, 9.0])):
            copy_graphics_fixture(directory / "graphics")
            write_capture_identity(directory, "graphics")
            ready = json.loads((directory / "ready.json").read_text())
            ready["camera"]["origin"] = camera_origin
            (directory / "ready.json").write_text(json.dumps(ready))
            lighting.analyze(directory, directory)

        comparison = lighting.compare(baseline, candidate, self.root / "comparison")
        self.assertFalse(comparison["comparable"])
        self.assertIn("not like-for-like", comparison["conclusion"])
        fields = [entry["field"] for entry in comparison["identity_differences"]]
        self.assertIn("identity.camera", fields)

        matched = lighting.compare(baseline, baseline, self.root / "matched")
        self.assertTrue(matched["comparable"])
        self.assertEqual(matched["conclusion"], "like-for-like")
        self.assertIn("gpu.frame_time", matched["metrics"])


class SystemsAnalysisTests(unittest.TestCase):
    def setUp(self) -> None:
        self._temporary = tempfile.TemporaryDirectory(prefix="ppr-profile-test-")
        self.root = Path(self._temporary.name)

    def tearDown(self) -> None:
        self._temporary.cleanup()

    def test_real_export_keeps_cpu_and_gpu_durations_distinct(self) -> None:
        capture_dir = copy_systems_fixture(self.root / "capture")
        write_capture_identity(capture_dir, "systems")
        analysis = lighting.analyze(capture_dir, self.root / "analysis")

        # Systems cannot observe GPU pass markers; its own evidence is enough.
        self.assertFalse(analysis["validation"]["required_markers_applicable"])
        self.assertIsNone(analysis["validation"]["required_ranges_present"])
        self.assertEqual(analysis["validation"]["missing_markers"], [])
        self.assertEqual(analysis["validation"]["overall_status"], "complete")

        cpu_metrics = [entry for entry in analysis["metrics"] if entry["name"].startswith("cpu.api.")]
        self.assertTrue(cpu_metrics)
        self.assertTrue(all(entry["unit"] == "ns" for entry in cpu_metrics))
        gpu_metrics = [entry for entry in analysis["metrics"] if entry["name"].startswith("gpu.")]
        self.assertTrue(any(entry["name"] == "gpu.workload_busy" for entry in gpu_metrics))
        # No metric claims a per-pass GPU execution duration from CPU API time.
        self.assertFalse(any(entry["name"] == "gpu.frame_time" for entry in analysis["metrics"]))
        for entry in cpu_metrics:
            self.assertNotIn("GPU execution", entry["raw_location"] or "")
        gpu_ranges = [entry for entry in analysis["ranges"] if entry["queue"] == "gpu"]
        self.assertTrue(all("not a per-pass GPU execution range" in entry["provenance"] for entry in gpu_ranges))
        cpu_ranges = [entry for entry in analysis["ranges"] if entry["queue"] == "cpu"]
        self.assertTrue(all("not GPU execution" in entry["provenance"] for entry in cpu_ranges))

    def test_unknown_schema_fails_with_the_observed_version(self) -> None:
        capture_dir = copy_systems_fixture(self.root / "capture")
        write_capture_identity(capture_dir, "systems")
        with sqlite3.connect(capture_dir / "systems.sqlite") as connection:
            connection.execute(
                "UPDATE META_DATA_EXPORT SET value='9.9.9' WHERE name='EXPORT_SCHEMA_VERSION'"
            )

        with self.assertRaises(lighting.ProfileError) as raised:
            lighting.analyze(capture_dir, self.root / "analysis")
        self.assertEqual(raised.exception.cause, "unknown-schema")
        self.assertIn("9.9.9", str(raised.exception))

    def test_nested_ranges_use_union_not_sum(self) -> None:
        self.assertEqual(lighting._interval_union_ns([]), 0)
        self.assertEqual(lighting._interval_union_ns([(0, 10)]), 10)
        # Parent 0..100 with nested children: the union is the parent's length.
        self.assertEqual(lighting._interval_union_ns([(0, 100), (10, 20), (30, 40)]), 100)
        self.assertEqual(lighting._interval_union_ns([(0, 10), (5, 20), (30, 40)]), 30)

    def test_missing_export_fails(self) -> None:
        capture_dir = self.root / "capture"
        write_capture_identity(capture_dir, "systems")
        with self.assertRaises(lighting.ProfileError) as raised:
            lighting.analyze(capture_dir, self.root / "analysis")
        self.assertEqual(raised.exception.cause, "missing-export")


class MetricNormalizationTests(unittest.TestCase):
    def test_metric_families_use_exact_tag_names(self) -> None:
        self.assertEqual(lighting._metric_family("GPU frame time"), "gpu_duration")
        self.assertEqual(lighting._metric_family("dram__bytes.sum.per_second"), "memory_throughput")
        self.assertEqual(lighting._metric_family("gpu__dram_throughput.avg.pct_of_peak_sustained_elapsed"), "memory_throughput")
        self.assertEqual(lighting._metric_family("l1tex__t_sector_hit_rate.pct"), "cache_hit_rate")
        self.assertEqual(
            lighting._metric_family("LTS.TriageA.lts__average_t_sector_op_read_realtime_hit_rate.pct"),
            "cache_hit_rate",
        )
        self.assertEqual(
            lighting._metric_family("FBPA.TriageA.dramc__throughput.avg.pct_of_peak_sustained_elapsed"),
            "memory_throughput",
        )
        self.assertEqual(
            lighting._metric_family("TPC.TriageA.tpc__warps_active_shader_ps_realtime.avg"), "sm_occupancy"
        )
        self.assertEqual(lighting._metric_family("lts__t_sector_hit_rate.pct"), "cache_hit_rate")
        self.assertEqual(lighting._metric_family("sm__warps_active.avg.pct_of_peak_sustained_active"), "sm_occupancy")
        # Near-misses are not family members.
        self.assertIsNone(lighting._metric_family("vkAllocateMemory"))
        self.assertIsNone(lighting._metric_family("l1tex__throughput.avg.pct_of_peak_sustained_elapsed"))
        self.assertIsNone(lighting._metric_family("gpu__engine_cycles_active_gr_or_ce.sum"))

    def test_metric_units_preserve_rates_and_peak_references(self) -> None:
        self.assertEqual(lighting._metric_unit("dram__bytes.sum.per_second"), ("per second", "rate"))
        self.assertEqual(lighting._metric_unit("dram__bytes.avg.per_second"), ("per second", "mean"))
        self.assertEqual(
            lighting._metric_unit("sm__cycles.avg.per_cycle_elapsed"), ("per cycle elapsed", "mean")
        )
        self.assertEqual(
            lighting._metric_unit("gpu__cycles.sum.per_cycle_elapsed"), ("per cycle elapsed", "rate")
        )
        self.assertEqual(
            lighting._metric_unit("sm__warps_active.avg.pct_of_peak_sustained_active"),
            ("% of peak sustained active", "mean"),
        )
        self.assertEqual(
            lighting._metric_unit("sm__warps_active.avg.peak_sustained"), ("peak sustained", "mean")
        )
        self.assertEqual(lighting._metric_unit("gpu__cycles.sum"), ("count", "sum"))

    def test_unavailable_families_are_null_with_a_reason(self) -> None:
        entries = lighting._unavailable_metrics([], set(), "not collected: {family}")
        self.assertEqual({entry["name"] for entry in entries}, {
            f"unavailable.{family}" for family in lighting.DESIRED_METRIC_FAMILIES
        })
        for entry in entries:
            self.assertIsNone(entry["value"])
            self.assertIn("not collected:", entry["reason"])


class CaptureSupervisionTests(unittest.TestCase):
    def setUp(self) -> None:
        self._temporary = tempfile.TemporaryDirectory(prefix="ppr-profile-test-")
        self.root = Path(self._temporary.name)
        self.app = fake_app_script(self.root)
        self.nsys = fake_nsys_script(self.root, self.app, FIXTURES / "systems" / "systems.sqlite")

    def tearDown(self) -> None:
        self._temporary.cleanup()

    def _config(self, output: Path, **overrides: object) -> lighting.CaptureConfig:
        values: dict[str, object] = {
            "tool": "systems",
            "executable": self.app,
            "output": output,
            "workload": "frozen",
            "nsys": self.nsys,
            "ready_timeout": 10.0,
            "collect_seconds": 0.2,
            "export_timeout": 10.0,
            "total_timeout": 30.0,
            "shutdown_grace": 2.0,
            "working_directory": self.root,
        }
        values.update(overrides)
        return lighting.CaptureConfig(**values)  # type: ignore[arg-type]

    def test_output_collision_is_rejected(self) -> None:
        output = self.root / "occupied"
        output.mkdir()
        (output / "previous.txt").write_text("old", encoding="utf-8")
        with self.assertRaises(lighting.ProfileError) as raised:
            lighting.capture(self._config(output))
        self.assertEqual(raised.exception.cause, "output-collision")
        self.assertTrue((output / "previous.txt").is_file())

    def test_ready_timeout_cleans_only_the_owned_process(self) -> None:
        output = self.root / "timeout"
        unrelated = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"])
        try:
            with self.assertRaises(lighting.ProfileError) as raised:
                lighting.capture(
                    self._config(
                        output,
                        ready_timeout=0.5,
                        environment={"PPR_FAKE_APP_MODE": "silent"},
                    )
                )
            self.assertEqual(raised.exception.cause, "readiness-timeout")
            self.assertIsNone(unrelated.poll(), "an unrelated process must never be signalled")
            manifest = json.loads((output / "manifest.json").read_text())
            self.assertEqual(manifest["capture"]["status"], "failed")
            self.assertIn("readiness", manifest["error"]["message"])
        finally:
            unrelated.terminate()
            unrelated.wait(timeout=10)

    def test_successful_capture_uses_the_published_session_token(self) -> None:
        output = self.root / "capture"
        manifest = lighting.capture(
            self._config(output, environment={"PPR_FAKE_APP_MODE": "ready"})
        )

        self.assertEqual(manifest["capture"]["status"], "complete")
        self.assertEqual((output / "stop.request").read_text().strip(), "session-fake")
        self.assertTrue((output / "systems.sqlite").is_file())
        self.assertIn("launch", manifest["commands"])
        self.assertIn("export", manifest["commands"])
        self.assertEqual(manifest["readiness"]["session_token"], "session-fake")
        analysis = lighting.analyze(output, output)
        self.assertEqual(analysis["identity"]["source_commit"], manifest["identity"]["source_commit"])

    def test_stale_stop_request_in_a_used_directory_is_rejected(self) -> None:
        output = self.root / "stale-stop"
        output.mkdir()
        (output / "stop.request").write_text("someone-else\n", encoding="utf-8")
        # The maintained capture never mixes a previous run's rendezvous file
        # into a new session; the wrong-token behavior itself is covered by the
        # application-side driver tests.
        with self.assertRaises(lighting.ProfileError) as raised:
            lighting.capture(self._config(output, environment={"PPR_FAKE_APP_MODE": "ready"}))
        self.assertEqual(raised.exception.cause, "output-collision")


    def test_stop_then_export_then_quit_order(self) -> None:
        output = self.root / "ordered"
        order_log = self.root / "order.log"
        manifest = lighting.capture(
            self._config(
                output,
                environment={"PPR_FAKE_APP_MODE": "ready", "PPR_FAKE_ORDER_LOG": str(order_log)},
            )
        )
        self.assertEqual(manifest["capture"]["status"], "complete")
        events = order_log.read_text().split()
        self.assertIn("export", events)
        self.assertIn("quit", events)
        self.assertLess(events.index("export"), events.index("quit"))

    def test_export_failure_never_claims_complete(self) -> None:
        output = self.root / "export-failure"
        with self.assertRaises(lighting.ProfileError) as raised:
            lighting.capture(
                self._config(
                    output,
                    environment={"PPR_FAKE_APP_MODE": "ready", "PPR_FAKE_EXPORT_FAIL": "1"},
                )
            )
        self.assertEqual(raised.exception.cause, "export-failure")
        manifest = json.loads((output / "manifest.json").read_text())
        self.assertEqual(manifest["capture"]["status"], "failed")
        self.assertEqual(manifest["error"]["cause"], "export-failure")
        self.assertFalse((output / "systems.sqlite").is_file())

    def test_terminate_escalates_a_sigterm_ignoring_child(self) -> None:
        script = self.root / "stubborn.py"
        ready_marker = self.root / "stubborn.ready"
        script.write_text(
            textwrap.dedent(
                f"""\
                import pathlib, signal, time
                signal.signal(signal.SIGTERM, signal.SIG_IGN)
                pathlib.Path({str(ready_marker)!r}).write_text("ready")
                time.sleep(60)
                """
            ),
            encoding="utf-8",
        )
        owned = lighting._OwnedProcess(
            [sys.executable, str(script)],
            cwd=self.root,
            env=dict(os.environ),
            stdout=self.root / "stubborn.out",
            stderr=self.root / "stubborn.err",
        )
        try:
            deadline = time.monotonic() + 10
            while not ready_marker.is_file() and time.monotonic() < deadline:
                time.sleep(0.02)
            self.assertTrue(ready_marker.is_file())
            outcome = owned.terminate(0.2)
            self.assertEqual(outcome, "killed")
            self.assertIsNotNone(owned.poll())
        finally:
            owned.close()


class IdentityTests(unittest.TestCase):
    def test_dirty_diff_hash_covers_content_not_only_the_file_list(self) -> None:
        with tempfile.TemporaryDirectory(prefix="ppr-profile-git-") as temporary:
            root = Path(temporary)

            def run(*arguments: str) -> None:
                subprocess.run(["git", *arguments], cwd=root, check=True, capture_output=True)

            run("init")
            run("config", "user.email", "test@example.invalid")
            run("config", "user.name", "test")
            (root / "tracked.txt").write_text("base", encoding="utf-8")
            run("add", "tracked.txt")
            run("commit", "-m", "base")

            (root / "tracked.txt").write_text("first edit", encoding="utf-8")
            (root / "untracked.txt").write_text("one", encoding="utf-8")
            first = lighting.git_identity(root)

            (root / "tracked.txt").write_text("second edit", encoding="utf-8")
            (root / "untracked.txt").write_text("two", encoding="utf-8")
            second = lighting.git_identity(root)

            self.assertTrue(first["dirty"])
            self.assertTrue(second["dirty"])
            self.assertNotEqual(first["dirty_diff_hash"], second["dirty_diff_hash"])
            self.assertIn("tooling", first)
            self.assertRegex(first["tooling"]["profile_lighting_py_sha256"], "^[0-9a-f]{64}$")

            (root / "tracked.txt").write_text("base", encoding="utf-8")
            (root / "untracked.txt").unlink()
            clean = lighting.git_identity(root)
            self.assertFalse(clean["dirty"])
            self.assertIsNone(clean["dirty_diff_hash"])


class DiscoveryTests(unittest.TestCase):
    def test_missing_tools_are_reported_without_crashing(self) -> None:
        record = lighting.discover(nsys_explicit="/nonexistent/nsys", ngfx_explicit="/nonexistent/ngfx")
        self.assertEqual(record["schema_version"], 1)
        self.assertIsNone(record["tools"]["systems"]["path"])
        self.assertIsNone(record["tools"]["graphics"]["path"])
        self.assertIn("os", record)
        self.assertIn("gpu", record)

    def test_architecture_mapping_matches_observed_compute_capabilities(self) -> None:
        self.assertEqual(lighting.architecture_for_compute_capability("7.5"), "Turing")
        self.assertEqual(lighting.architecture_for_compute_capability("8.6"), "Ampere GA10x")
        self.assertEqual(lighting.architecture_for_compute_capability("8.9"), "Ada")
        self.assertEqual(lighting.architecture_for_compute_capability("12.0"), "Blackwell GB20x")
        self.assertIsNone(lighting.architecture_for_compute_capability(None))


class CommandLineTests(unittest.TestCase):
    def test_invalid_usage_returns_two(self) -> None:
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            self.assertEqual(lighting.main(["analyze"]), 2)
        self.assertIn("required", stderr.getvalue())

    def test_unknown_input_returns_nonzero(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr):
                code = lighting.main(
                    ["analyze", "--input", str(Path(temporary) / "missing"), "--output", temporary]
                )
            self.assertNotEqual(code, 0)
            self.assertIn("not found", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
