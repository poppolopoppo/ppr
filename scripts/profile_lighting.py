"""Maintained bounded lighting profiling with real Nsight exports.

Subcommands:
    discover   Record installed Nsight Systems/Graphics tools, GPU/driver,
               architecture-specific metric sets and the installed SDK.
    capture    Launch the ordinary ``app.game`` in its permanent profiling mode,
               wait for its readiness record, bound the collection interval,
               export native reports and retain every log/artifact.
    analyze    Offline, repeatable normalization of the retained vendor exports
               into ``analysis.json``/``analysis.yaml`` (schema version 1).
    compare    Matched-capture comparison with explicit identity checks.

The vendor adapters are pinned to observed export schemas; unknown schemas fail
with an actionable report instead of guessing. Raw exports are never modified.
"""

from __future__ import annotations

import argparse
import contextlib
import datetime as _datetime
import hashlib
import json
import math
import os
import platform
import shutil
import signal
import sqlite3
import statistics
import subprocess
import sys
import tempfile
import time
import uuid
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable, Sequence

try:  # POSIX host (the geometry/tool capture host); Windows reports unavailable.
    import fcntl
except ImportError:  # pragma: no cover - Windows import compatibility
    fcntl = None  # type: ignore[assignment]

SCHEMA_VERSION = 1

TOOL_SYSTEMS = "systems"
TOOL_GRAPHICS = "graphics"
WORKLOADS = ("frozen", "live")

READY_FILE = "ready.json"
FINAL_STATE_FILE = "final-state.json"
MANIFEST_FILE = "manifest.json"
STOP_REQUEST_FILE = "stop.request"
ANALYSIS_JSON = "analysis.json"
ANALYSIS_YAML = "analysis.yaml"
COMPARISON_JSON = "comparison.json"
COMPARISON_YAML = "comparison.yaml"

READY_TIMEOUT_SECONDS = 180.0
COLLECT_SECONDS = 2.0
EXPORT_TIMEOUT_SECONDS = 120.0
TOTAL_TIMEOUT_SECONDS = 480.0
SHUTDOWN_GRACE_SECONDS = 30.0
GRAPHICS_FRAMES = 8
GRAPHICS_MAX_DURATION_MS = 2000
LOCK_NAME = "ppr-lighting-profile.lock"

# Observed export schemas. A different version is reported as an unsupported
# adapter target, never parsed optimistically.
SYSTEMS_EXPORT_SCHEMA = "3.31.0"
SYSTEMS_EXPORT_SCHEMA_MAJOR = 3

# Stable GPU marker names required by the design contract for the Graphics GPU
# trace. A missing marker is reported, never invented. Systems cannot observe
# GPU pass ranges, so this set does not gate a Systems capture.
REQUIRED_GPU_MARKERS = (
    "WorldLighting.ResolveCells",
    "WorldLighting.Visibility",
    "WorldLighting.Jump",
    "WorldLighting.Source",
    "WorldLighting.Cascade.L5",
    "WorldLighting.Cascade.L4",
    "WorldLighting.Cascade.L3",
    "WorldLighting.Cascade.L2",
    "WorldLighting.Cascade.L1",
    "WorldLighting.FineCH",
    "Scene.Materials",
    "Scene.Gas",
    "Hable.Resolve",
    "Post.Exposure",
    "Post.Bloom",
    "Post.AgX",
    "Present.Scene",
    "Present.UI",
    "PBMPM.Tick",
)

# A frozen workload pauses before the first simulation tick, so the PB-MPM
# solver legitimately never runs; a live workload must emit it.
FROZEN_OPTIONAL_MARKERS = ("PBMPM.Tick",)

# Normalized metric families the analysis would like to report. Membership is
# decided by exact vendor tag families, never by a loose substring: e.g.
# ``vkAllocateMemory`` is a CPU API call, not memory throughput, and
# ``l1tex__throughput`` is a unit throughput metric, not a cache hit rate.
DESIRED_METRIC_FAMILIES = (
    "gpu_duration",
    "sm_occupancy",
    "memory_throughput",
    "cache_hit_rate",
)

class ProfileError(RuntimeError):
    """Operational failure carrying a machine-readable cause."""

    def __init__(self, message: str, *, cause: str = "failed", details: Any = None) -> None:
        super().__init__(message)
        self.cause = cause
        self.details = details


# ----------------------------------------------------------------------
# generic helpers
# ----------------------------------------------------------------------


def _utc_now() -> str:
    return _datetime.datetime.now(tz=_datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def git_identity(repository: Path) -> dict[str, Any]:
    """Content identity of the captured working tree, not just its file list.

    The hash covers ``git diff HEAD --binary`` and the bytes of every untracked,
    non-ignored file (plus the profiling tool's own bytes), so two different
    edits with an identical status listing cannot claim the same identity.
    """

    toplevel = subprocess.run(
        ["git", "rev-parse", "--show-toplevel"], cwd=repository, capture_output=True, text=True, check=False
    )
    if toplevel.returncode != 0:
        return {
            "source_commit": None,
            "dirty": False,
            "dirty_diff_hash": None,
            "working_tree_hash": None,
            "tooling": {"profile_lighting_py_sha256": sha256_file(Path(__file__))},
        }

    root = Path(toplevel.stdout.strip())
    commit = subprocess.run(
        ["git", "rev-parse", "HEAD"], cwd=root, capture_output=True, text=True, check=False
    ).stdout.strip()
    tracked_diff = subprocess.run(
        ["git", "diff", "HEAD", "--binary"], cwd=root, capture_output=True, check=False
    ).stdout
    untracked = subprocess.run(
        ["git", "ls-files", "--others", "--exclude-standard", "-z"],
        cwd=root,
        capture_output=True,
        check=False,
    ).stdout

    hasher = hashlib.sha256()
    hasher.update(tracked_diff)
    names = sorted(name for name in untracked.split(b"\0") if name)
    for name in names:
        hasher.update(name)
        hasher.update(b"\0")
        try:
            hasher.update((root / name.decode("utf-8", errors="replace")).read_bytes())
        except OSError:
            hasher.update(b"<unreadable>")
    digest = hasher.hexdigest()

    return {
        "source_commit": commit or None,
        "dirty": bool(tracked_diff or names),
        "dirty_diff_hash": digest if (tracked_diff or names) else None,
        "working_tree_hash": digest,
        "tooling": {"profile_lighting_py_sha256": sha256_file(Path(__file__))},
    }


def build_preset_from_path(executable: Path) -> str | None:
    """Records the configured build preset from the conventional tree layout."""
    parts = executable.resolve().parts
    if "build" in parts:
        index = parts.index("build")
        if index + 1 < len(parts):
            return parts[index + 1]
    return os.environ.get("PPR_BUILD_PRESET")


def find_repository(start: Path) -> Path | None:
    for candidate in [start, *start.parents]:
        if (candidate / ".git").exists():
            return candidate
    return None


def _json_load(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return None
    except json.JSONDecodeError as error:
        raise ProfileError(f"{path} is not valid JSON: {error}", cause="invalid-artifact") from error


def _write_text_atomic(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(text, encoding="utf-8")
    os.replace(temporary, path)


def _write_json(path: Path, value: Any) -> None:
    _write_text_atomic(path, json.dumps(value, indent=2, sort_keys=False) + "\n")


def _yaml_scalar(value: Any) -> str:
    if value is None:
        return "null"
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, (int, float)):
        if isinstance(value, float) and not math.isfinite(value):
            return "null"
        return repr(value) if isinstance(value, float) else str(value)
    text = str(value)
    if text == "" or any(character in text for character in ":#{}[]&*?|>!%@`,\n\"'"):
        return json.dumps(text)
    return text


def _write_yaml(path: Path, value: Any, indent: int = 0) -> None:
    """Minimal deterministic YAML emitter for the normalized analysis."""

    lines = _yaml_lines(value, indent) if isinstance(value, (dict, list)) else [(" " * indent) + _yaml_scalar(value)]
    _write_text_atomic(path, "\n".join(lines) + "\n")


def _yaml_lines(value: Any, indent: int) -> list[str]:
    pad = " " * indent
    lines: list[str] = []
    if isinstance(value, dict):
        for key, entry in value.items():
            if isinstance(entry, (dict, list)) and entry:
                lines.append(f"{pad}{key}:")
                lines.extend(_yaml_lines(entry, indent + 2))
            elif isinstance(entry, dict):
                lines.append(f"{pad}{key}: {{}}")
            elif isinstance(entry, list):
                lines.append(f"{pad}{key}: []")
            else:
                lines.append(f"{pad}{key}: {_yaml_scalar(entry)}")
    elif isinstance(value, list):
        for entry in value:
            if isinstance(entry, dict) and entry:
                first = True
                for key, nested in entry.items():
                    prefix = f"{pad}- " if first else f"{pad}  "
                    first = False
                    if isinstance(nested, (dict, list)) and nested:
                        lines.append(f"{prefix}{key}:")
                        lines.extend(_yaml_lines(nested, indent + 4))
                    elif isinstance(nested, dict):
                        lines.append(f"{prefix}{key}: {{}}")
                    elif isinstance(nested, list):
                        lines.append(f"{prefix}{key}: []")
                    else:
                        lines.append(f"{prefix}{key}: {_yaml_scalar(nested)}")
            elif isinstance(entry, list):
                lines.append(f"{pad}- {_yaml_scalar(entry)}")
            else:
                lines.append(f"{pad}- {_yaml_scalar(entry)}")
    return lines


def _command_version(executable: Path, arguments: Sequence[str] = ("--version",)) -> str | None:
    try:
        completed = subprocess.run(
            [str(executable), *arguments], capture_output=True, text=True, timeout=30, check=False
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    text = (completed.stdout or completed.stderr).strip()
    if not text:
        return None
    return text.splitlines()[0].strip()


def _command_help(executable: Path, arguments: Sequence[str]) -> str:
    try:
        completed = subprocess.run(
            [str(executable), *arguments], capture_output=True, text=True, timeout=60, check=False
        )
    except (OSError, subprocess.TimeoutExpired):
        return ""
    # The Graphics help commands print help and return a nonzero exit code.
    return (completed.stdout or "") + (completed.stderr or "")


def _gpu_information() -> dict[str, Any]:
    info: dict[str, Any] = {"name": None, "driver_version": None, "compute_capability": None, "architecture": None}
    try:
        completed = subprocess.run(
            [
                "nvidia-smi",
                "--query-gpu=name,driver_version,compute_cap",
                "--format=csv,noheader",
            ],
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        return info
    if completed.returncode != 0 or not completed.stdout.strip():
        return info
    first = completed.stdout.strip().splitlines()[0]
    parts = [part.strip() for part in first.split(",")]
    if len(parts) >= 3:
        info["name"] = parts[0]
        info["driver_version"] = parts[1]
        info["compute_capability"] = parts[2]
        info["architecture"] = architecture_for_compute_capability(parts[2])
    return info


def architecture_for_compute_capability(capability: str | None) -> str | None:
    if not capability:
        return None
    try:
        major_text, minor_text = capability.split(".")
        major = int(major_text)
        minor = int(minor_text)
    except (ValueError, TypeError):
        return None
    if major == 7 and minor == 5:
        return "Turing"
    if major == 8 and minor in (0, 6, 7):
        return "Ampere GA10x"
    if major == 8 and minor == 9:
        return "Ada"
    if major == 9:
        return "Hopper"
    if major in (10, 12):
        return "Blackwell GB20x"
    return None


# ----------------------------------------------------------------------
# tool discovery
# ----------------------------------------------------------------------


def _candidate_roots() -> list[Path]:
    roots: list[Path] = []
    configured = os.environ.get("PPR_NSIGHT_ROOTS")
    if configured:
        roots.extend(Path(entry) for entry in configured.split(os.pathsep) if entry)
    for base in (
        Path.home() / ".local" / "nvidia",
        Path("/opt/nvidia"),
        Path("/usr/local/nvidia"),
    ):
        if base.is_dir():
            roots.append(base)
    return roots


def _discover_binary(names: Sequence[str], explicit: str | None, environment: str | None) -> Path | None:
    if explicit:
        candidate = Path(explicit).expanduser()
        return candidate if candidate.is_file() else None
    if environment:
        configured = os.environ.get(environment)
        if configured:
            candidate = Path(configured).expanduser()
            if candidate.is_file():
                return candidate
    for name in names:
        located = shutil.which(name)
        if located:
            return Path(located)
    for root in _candidate_roots():
        for name in names:
            try:
                matches = sorted(root.glob(f"**/{name}"))
            except OSError:
                continue
            for match in reversed(matches):
                if match.is_file() and os.access(match, os.X_OK):
                    return match
    return None


def _metric_sets(help_text: str) -> dict[str, list[dict[str, Any]]]:
    """Parse the observed ``--metric-set-id`` block from the Graphics help."""

    sets: dict[str, list[dict[str, Any]]] = {}
    architecture: str | None = None
    inside = False
    lines = [line.strip() for line in help_text.splitlines()]
    for index, line in enumerate(lines):
        if "--metric-set-id" in line:
            inside = True
            continue
        if not inside:
            continue
        if "--multi-pass-metrics" in line or "--advanced-mode" in line:
            break
        stripped = line
        if not stripped:
            continue
        if not stripped[:1].isdigit():
            # Each architecture is followed by its own numeric metric-set
            # entries; the surrounding prose is not an architecture.
            following = next((entry for entry in lines[index + 1 :] if entry), "")
            if following[:1].isdigit() and "-" in following:
                architecture = stripped[:-1].strip() if stripped.endswith(":") else stripped
                sets.setdefault(architecture, [])
            continue
        if architecture and stripped[:1].isdigit() and "-" in stripped:
            index_text, _, name = stripped.partition("-")
            try:
                metric_id = int(index_text)
            except ValueError:
                continue
            sets[architecture].append({"id": metric_id, "name": name.strip()})
    return sets


def _sdk_installations() -> list[dict[str, Any]]:
    installations: list[dict[str, Any]] = []
    seen: set[Path] = set()
    for root in _candidate_roots():
        try:
            candidates = list(root.glob("**/NsightGraphicsSDK/*/include/NGFX_GPUTrace_Vulkan.h"))
        except OSError:
            continue
        for header in candidates:
            sdk_root = header.parent.parent
            if sdk_root in seen:
                continue
            seen.add(sdk_root)
            license_text = None
            license_path = sdk_root / "LICENSE.txt"
            if license_path.is_file():
                license_text = license_path.read_text(encoding="utf-8", errors="replace")
            installations.append(
                {
                    "root": str(sdk_root),
                    "version": sdk_root.name,
                    "header": str(header),
                    "license": "Apache-2.0" if "Apache License" in (license_text or "") else None,
                }
            )
    installations.sort(key=lambda entry: entry["root"])
    return installations


def discover(nsys_explicit: str | None = None, ngfx_explicit: str | None = None) -> dict[str, Any]:
    nsys = _discover_binary(("nsys",), nsys_explicit, "PPR_NSYS")
    ngfx = _discover_binary(("ngfx",), ngfx_explicit, "PPR_NGFX")
    nsys_help = _command_help(nsys, ("profile", "--help")) if nsys else ""
    ngfx_help = _command_help(ngfx, ("--help-all",)) if ngfx else ""

    gpu = _gpu_information()
    metric_sets = _metric_sets(ngfx_help) if ngfx else {}
    architecture = gpu.get("architecture")
    available_sets = metric_sets.get(architecture or "", [])
    chosen = None
    for preferred in ("Top-Level Triage", "Throughput Metrics"):
        if any(entry["name"] == preferred for entry in available_sets):
            chosen = preferred
            break
    if chosen is None and available_sets:
        chosen = available_sets[0]["name"]

    return {
        "schema_version": SCHEMA_VERSION,
        "discovered_utc": _utc_now(),
        "os": {
            "system": platform.system(),
            "release": platform.release(),
            "machine": platform.machine(),
        },
        "gpu": gpu,
        "backend": "vulkan",
        "tools": {
            TOOL_SYSTEMS: {
                "path": str(nsys) if nsys else None,
                "version": _command_version(nsys) if nsys else None,
                "capabilities": {
                    "vulkan_trace": "--trace=" in nsys_help and "vulkan" in nsys_help,
                    "named_sessions": "--session-new" in nsys_help,
                    "sqlite_export": "sqlite" in nsys_help,
                    "vulkan_gpu_workload": "--vulkan-gpu-workload" in nsys_help,
                    "gpu_metrics": "--gpu-metrics-devices" in nsys_help,
                },
            },
            TOOL_GRAPHICS: {
                "path": str(ngfx) if ngfx else None,
                "version": _command_version(ngfx) if ngfx else None,
                "capabilities": {
                    "gpu_trace_profiler": "GPU Trace Profiler" in ngfx_help,
                    "sdk_start": "--start-with-ngfx-sdk" in ngfx_help,
                    "auto_export": "--auto-export" in ngfx_help,
                    "clocks_unaltered": "unaltered" in ngfx_help,
                    "multi_pass_metrics": "--multi-pass-metrics" in ngfx_help,
                },
            },
        },
        "graphics": {
            "architecture": architecture,
            "metric_sets": metric_sets,
            "selected_metric_set": chosen,
        },
        "sdk_installations": _sdk_installations(),
        "environment": {
            "PPR_NSIGHT_GRAPHICS_SDK_ROOT": os.environ.get("PPR_NSIGHT_GRAPHICS_SDK_ROOT"),
            "PPR_NSYS": os.environ.get("PPR_NSYS"),
            "PPR_NGFX": os.environ.get("PPR_NGFX"),
        },
    }


# ----------------------------------------------------------------------
# machine-local capture lock
# ----------------------------------------------------------------------


class CaptureLock:
    """Exclusive machine-local lock so builds/tests/captures never interleave."""

    def __init__(self) -> None:
        self._stream: Any = None
        self.path = Path(os.environ.get("XDG_RUNTIME_DIR", tempfile.gettempdir())) / LOCK_NAME

    def __enter__(self) -> "CaptureLock":
        self.path.parent.mkdir(parents=True, exist_ok=True)
        stream = self.path.open("a+", encoding="utf-8")
        try:
            if fcntl is None:
                raise ProfileError(
                    "machine-local capture locking is unavailable on this host", cause="unsupported"
                )
            fcntl.flock(stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            stream.seek(0)
            owner = stream.read().strip()
            stream.close()
            raise ProfileError(
                f"another profiling capture owns {self.path} (owner record: {owner!r})",
                cause="busy",
            ) from error
        stream.seek(0)
        stream.truncate()
        stream.write(f"pid={os.getpid()} started={_utc_now()}\n")
        stream.flush()
        self._stream = stream
        return self

    def __exit__(self, *_: Any) -> None:
        if self._stream is None:
            return
        with contextlib.suppress(OSError):
            self._stream.seek(0)
            self._stream.truncate()
            if fcntl is not None:
                fcntl.flock(self._stream.fileno(), fcntl.LOCK_UN)
        self._stream.close()
        self._stream = None


# ----------------------------------------------------------------------
# capture supervision
# ----------------------------------------------------------------------


@dataclass
class CaptureConfig:
    tool: str
    executable: Path
    output: Path
    workload: str = "frozen"
    nsys: Path | None = None
    ngfx: Path | None = None
    architecture: str | None = None
    metric_set: str | None = None
    ready_timeout: float = READY_TIMEOUT_SECONDS
    collect_seconds: float = COLLECT_SECONDS
    export_timeout: float = EXPORT_TIMEOUT_SECONDS
    total_timeout: float = TOTAL_TIMEOUT_SECONDS
    shutdown_grace: float = SHUTDOWN_GRACE_SECONDS
    working_directory: Path | None = None
    environment: dict[str, str] = field(default_factory=dict)


class _OwnedProcess:
    """A process group this capture owns; nothing else is ever signalled."""

    def __init__(self, argv: Sequence[str], *, cwd: Path | None, env: dict[str, str], stdout: Path, stderr: Path):
        self.argv = [str(entry) for entry in argv]
        self.stdout_path = stdout
        self.stderr_path = stderr
        self.start_utc = _utc_now()
        self._stdout = stdout.open("wb")
        self._stderr = stderr.open("wb")
        self.process = subprocess.Popen(
            self.argv,
            cwd=str(cwd) if cwd else None,
            env=env,
            stdout=self._stdout,
            stderr=self._stderr,
            start_new_session=True,
        )

    @property
    def pid(self) -> int:
        return self.process.pid

    def poll(self) -> int | None:
        return self.process.poll()

    def wait(self, timeout: float) -> int | None:
        try:
            return self.process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            return None

    def terminate(self, grace: float) -> str:
        """Terminates only this owned process group; never raises a timeout.

        A child that ignores SIGTERM is escalated to SIGKILL after the grace
        period, so the original capture error remains the reported cause.
        """

        if self.process.poll() is not None:
            return "exited"
        with contextlib.suppress(ProcessLookupError):
            os.killpg(os.getpgid(self.process.pid), signal.SIGTERM)

        try:
            self.process.wait(timeout=grace)
            return "terminated"
        except subprocess.TimeoutExpired:
            pass

        with contextlib.suppress(ProcessLookupError):
            os.killpg(os.getpgid(self.process.pid), signal.SIGKILL)
        try:
            self.process.wait(timeout=grace)
        except subprocess.TimeoutExpired:
            return "kill-unreaped"
        return "killed"

    def close(self) -> None:
        self._stdout.close()
        self._stderr.close()


def _run_tool(
    argv: Sequence[str], *, log_path: Path, timeout: float, cwd: Path | None = None, env: dict[str, str] | None = None
) -> int:
    """Runs a bounded, non-interactive tool command and retains its output."""
    with log_path.open("wb") as stream:
        try:
            completed = subprocess.run(
                [str(entry) for entry in argv],
                cwd=str(cwd) if cwd else None,
                env=env,
                stdout=stream,
                stderr=subprocess.STDOUT,
                timeout=timeout,
                check=False,
            )
        except subprocess.TimeoutExpired as error:
            raise ProfileError(
                f"{Path(argv[0]).name} did not finish within {timeout:.0f} s", cause="tool-timeout"
            ) from error
    return completed.returncode


def _app_arguments(config: CaptureConfig, output: Path) -> list[str]:
    # Absolute paths keep the application's own working directory irrelevant.
    return [
        f"--profile-capture={config.tool}",
        f"--profile-output={output}",
        f"--profile-workload={config.workload}",
    ]


def _wait_for_readiness(output: Path, deadline: float, process: _OwnedProcess | None) -> dict[str, Any] | None:
    """Waits for ready.json but stops early when the application reports that
    the requested capture cannot be performed."""
    while time.monotonic() < deadline:
        if (output / READY_FILE).is_file():
            ready = _read_json_if_present(output / READY_FILE)
            if isinstance(ready, dict):
                return ready
        final_state = _read_json_if_present(output / FINAL_STATE_FILE)
        if isinstance(final_state, dict) and final_state.get("status") in ("unsupported", "failed", "incomplete"):
            return None
        if process is not None and process.poll() is not None:
            return None
        time.sleep(0.1)
    return None


def _wait_for_json(path: Path, deadline: float, process: _OwnedProcess | None) -> dict[str, Any] | None:
    while time.monotonic() < deadline:
        if path.is_file():
            try:
                return json.loads(path.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError):
                pass
        if process is not None and process.poll() is not None:
            return None
        time.sleep(0.1)
    return None


def _write_stop_request(output: Path, token: str) -> None:
    _write_text_atomic(output / STOP_REQUEST_FILE, token + "\n")


def _read_json_if_present(path: Path) -> Any:
    if not path.is_file():
        return None
    with contextlib.suppress(OSError, json.JSONDecodeError):
        return json.loads(path.read_text(encoding="utf-8"))
    return None


def _artifact_inventory(root: Path) -> dict[str, list[str]]:
    native: list[str] = []
    exports: list[str] = []
    logs: list[str] = []
    for path in sorted(root.rglob("*")):
        if not path.is_file():
            continue
        relative = str(path.relative_to(root))
        if path.suffix in (".nsys-rep", ".ngfx-gputrace"):
            native.append(relative)
        elif path.suffix in (".sqlite", ".xls") or path.name == "ReportGeneratorTags.txt":
            exports.append(relative)
        elif path.suffix in (".log", ".txt") or path.name.startswith("stderr"):
            logs.append(relative)
    return {"native": native, "export": exports, "logs": logs}


def capture(config: CaptureConfig) -> dict[str, Any]:
    if config.tool not in (TOOL_SYSTEMS, TOOL_GRAPHICS):
        raise ProfileError(f"unknown capture tool {config.tool!r}", cause="invalid-invocation")
    if config.workload not in WORKLOADS:
        raise ProfileError(f"unknown profiling workload {config.workload!r}", cause="invalid-invocation")
    executable = config.executable.expanduser().resolve()
    if not executable.is_file():
        raise ProfileError(f"application executable not found: {executable}", cause="invalid-invocation")
    output = config.output.expanduser().resolve()
    if output.exists():
        if not output.is_dir():
            raise ProfileError(f"capture output is not a directory: {output}", cause="invalid-invocation")
        if any(output.iterdir()):
            raise ProfileError(f"capture output directory is not fresh: {output}", cause="output-collision")
    else:
        output.mkdir(parents=True)

    if config.tool == TOOL_SYSTEMS and not config.nsys:
        raise ProfileError("Nsight Systems CLI not configured or discovered", cause="unsupported")
    if config.tool == TOOL_GRAPHICS and not config.ngfx:
        raise ProfileError("Nsight Graphics CLI not configured or discovered", cause="unsupported")

    session = f"ppr-lighting-{uuid.uuid4().hex[:12]}"
    # The application owns the freshness of its artifact directory, so the
    # supervisor's own process logs stay outside it until the run is over.
    supervisor_logs = output.with_name(output.name + ".supervisor")
    shutil.rmtree(supervisor_logs, ignore_errors=True)
    supervisor_logs.mkdir(parents=True)
    # Nsight Graphics requires an existing output directory, which must not
    # precede the application's own fresh-directory check: the tool writes to a
    # staging directory that is moved into the capture afterwards.
    graphics_staging = output.with_name(output.name + ".ngfx-export")
    shutil.rmtree(graphics_staging, ignore_errors=True)
    if config.tool == TOOL_GRAPHICS:
        graphics_staging.mkdir(parents=True)
    working_directory = config.working_directory or executable.parent
    environment = dict(os.environ)
    environment.update(config.environment)
    started = time.monotonic()
    started_utc = _utc_now()

    manifest: dict[str, Any] = {
        "schema_version": SCHEMA_VERSION,
        "capture": {
            "tool": config.tool,
            "workload": config.workload,
            "output": str(output),
            "session": session,
            "started_utc": started_utc,
        },
        "identity": {
            **git_identity(find_repository(executable) or find_repository(Path.cwd()) or output),
            "executable": str(executable),
            "executable_bytes": executable.stat().st_size,
            "executable_sha256": sha256_file(executable),
            "build_preset": build_preset_from_path(executable),
        },
        "environment": {
            "os": platform.platform(),
            "gpu": _gpu_information(),
        },
        "tools": {
            TOOL_SYSTEMS: {
                "path": str(config.nsys) if config.nsys else None,
                "version": _command_version(config.nsys) if config.nsys else None,
            },
            TOOL_GRAPHICS: {
                "path": str(config.ngfx) if config.ngfx else None,
                "version": _command_version(config.ngfx) if config.ngfx else None,
            },
        },
        "collection": {
            "metric_set": config.metric_set,
            "architecture": config.architecture,
            "clocks": "unaltered",
            "requested_frames": GRAPHICS_FRAMES if config.tool == TOOL_GRAPHICS else None,
            "max_duration_ms": GRAPHICS_MAX_DURATION_MS if config.tool == TOOL_GRAPHICS else None,
            "interval_seconds": config.collect_seconds,
        },
        "commands": {},
        "deviations": [],
    }

    app_arguments = _app_arguments(config, output)
    owned: list[_OwnedProcess] = []
    launcher: _OwnedProcess | None = None
    profiler: _OwnedProcess | None = None
    exit_code: int | None = None
    status = "failed"
    failure: str | None = None

    def app_pid() -> int | None:
        ready = _read_json_if_present(output / READY_FILE)
        if isinstance(ready, dict) and isinstance(ready.get("pid"), int):
            return int(ready["pid"])
        return None

    def request_stop() -> None:
        token = None
        ready = _read_json_if_present(output / READY_FILE)
        if isinstance(ready, dict):
            token = ready.get("session_token")
        if token is None:
            final_state = _read_json_if_present(output / FINAL_STATE_FILE)
            if isinstance(final_state, dict):
                token = final_state.get("session_token")
        if isinstance(token, str) and token:
            _write_stop_request(output, token)

    try:
        with CaptureLock():
            if config.tool == TOOL_SYSTEMS:
                launch_argv = [
                    str(config.nsys),
                    "launch",
                    f"--session-new={session}",
                    "--trace=vulkan,osrt",
                    "--vulkan-gpu-workload=individual",
                    str(executable),
                    *app_arguments,
                ]
                manifest["commands"]["launch"] = launch_argv
                launcher = _OwnedProcess(
                    launch_argv,
                    cwd=working_directory,
                    env=environment,
                    stdout=supervisor_logs / "launcher.stdout.log",
                    stderr=supervisor_logs / "launcher.stderr.log",
                )
                owned.append(launcher)
            else:
                ngfx_arguments = [
                    str(config.ngfx),
                    "--activity=GPU Trace Profiler",
                    f"--exe={executable}",
                    f"--dir={working_directory}",
                    f"--args={' '.join(app_arguments)}",
                    "--start-with-ngfx-sdk",
                    f"--limit-to-frames={GRAPHICS_FRAMES}",
                    f"--max-duration-ms={GRAPHICS_MAX_DURATION_MS}",
                    "--set-gpu-clocks=unaltered",
                    "--auto-export",
                    f"--output-dir={graphics_staging}",
                ]
                if config.architecture:
                    ngfx_arguments.append(f"--architecture={config.architecture}")
                if config.metric_set:
                    ngfx_arguments.append(f"--metric-set-name={config.metric_set}")
                manifest["commands"]["launch"] = ngfx_arguments
                launcher = _OwnedProcess(
                    ngfx_arguments,
                    cwd=working_directory,
                    env=environment,
                    stdout=supervisor_logs / "profiler.stdout.log",
                    stderr=supervisor_logs / "profiler.stderr.log",
                )
                owned.append(launcher)
                profiler = launcher

            manifest["commands"]["application"] = [str(executable), *app_arguments]

            total_deadline = started + config.total_timeout
            ready_deadline = min(time.monotonic() + config.ready_timeout, total_deadline)
            ready = _wait_for_readiness(output, ready_deadline, launcher)
            manifest["readiness"] = ready
            if ready is None:
                raise ProfileError(
                    f"application did not reach readiness within {config.ready_timeout:.0f} s",
                    cause="readiness-timeout",
                )

            pid = app_pid()
            manifest["application_pid"] = pid

            if config.tool == TOOL_SYSTEMS:
                start_argv = [
                    str(config.nsys),
                    "start",
                    f"--session={session}",
                    "--sample=process-tree",
                    "--backtrace=fp",
                    f"--output={output / 'systems'}",
                ]
                manifest["commands"]["start"] = start_argv
                start_code = _run_tool(
                    start_argv, log_path=supervisor_logs / "profiler.start.log", timeout=60.0, env=environment
                )
                manifest["collection"]["start_exit_code"] = start_code
                if start_code != 0:
                    raise ProfileError(f"nsys start failed with exit {start_code}", cause="tool-failure")

                collection_deadline = min(time.monotonic() + config.collect_seconds, total_deadline)
                while time.monotonic() < collection_deadline:
                    time.sleep(min(0.05, max(0.0, collection_deadline - time.monotonic())))

                stop_argv = [str(config.nsys), "stop", f"--session={session}"]
                manifest["commands"]["stop"] = stop_argv
                stop_code = _run_tool(
                    stop_argv, log_path=supervisor_logs / "profiler.stop.log", timeout=60.0, env=environment
                )
                manifest["collection"]["stop_exit_code"] = stop_code
                if stop_code != 0:
                    raise ProfileError(f"nsys stop failed with exit {stop_code}", cause="tool-failure")

                # Stop -> export -> quit: the native report is complete after
                # `nsys stop`, so the export succeeds while the application is
                # still alive, and the ordinary quit follows the retained export.
                native = output / "systems.nsys-rep"
                if not native.is_file():
                    raise ProfileError("Nsight Systems native report was not produced", cause="tool-failure")
                export_argv = [
                    str(config.nsys),
                    "export",
                    "--type=sqlite",
                    f"--output={output / 'systems.sqlite'}",
                    str(native),
                ]
                manifest["commands"]["export"] = export_argv
                export_code = _run_tool(
                    export_argv,
                    log_path=supervisor_logs / "profiler.export.log",
                    timeout=config.export_timeout,
                    env=environment,
                )
                manifest["collection"]["export_exit_code"] = export_code
                if export_code != 0 or not (output / "systems.sqlite").is_file():
                    raise ProfileError(f"nsys sqlite export failed with exit {export_code}", cause="export-failure")

                request_stop()
                manifest["commands"]["stop_request"] = str(output / STOP_REQUEST_FILE)
            else:
                # The application starts and finalizes the trace through the
                # installed SDK; the tool bounds frames and duration.
                deadline = min(time.monotonic() + config.collect_seconds + config.export_timeout, total_deadline)
                final_state = _wait_for_json(output / FINAL_STATE_FILE, deadline, profiler)
                if final_state is None:
                    raise ProfileError(
                        "application did not finalize the graphics trace within the collection deadline",
                        cause="finalize-timeout",
                    )

                request_stop()
                manifest["commands"]["stop_request"] = str(output / STOP_REQUEST_FILE)

            # Ordinary bounded wait for the process this capture owns.
            wait_deadline = time.monotonic() + config.shutdown_grace
            if launcher is not None:
                exit_code = launcher.wait(max(0.0, wait_deadline - time.monotonic()))
            if exit_code is None and launcher is not None:
                manifest["deviations"].append("launcher did not exit within the shutdown grace period")
                manifest["cleanup"] = launcher.terminate(config.shutdown_grace)

            final_state = _read_json_if_present(output / FINAL_STATE_FILE)
            manifest["final_state"] = final_state
            final_status = final_state.get("status") if isinstance(final_state, dict) else None
            if final_status in ("incomplete", "unsupported", "failed"):
                status = final_status
            elif final_status in ("complete", "ready"):
                # `complete` is only claimed after the export/finalization below.
                status = "pending-artifacts"
            else:
                status = "incomplete"
                manifest["deviations"].append("final-state record missing or unreadable")

            if config.tool == TOOL_GRAPHICS:
                if not graphics_staging.is_dir() or not any(graphics_staging.iterdir()):
                    raise ProfileError("Nsight Graphics auto-export produced no directory", cause="export-failure")
                shutil.move(str(graphics_staging), str(output / "graphics"))
                traces = sorted((output / "graphics").rglob("*.ngfx-gputrace"))
                if not traces:
                    raise ProfileError("no finalized .ngfx-gputrace file was retained", cause="export-failure")
                # Record the path of the preserved native report, after the move.
                manifest["collection"]["trace_path"] = str(traces[0])
                manifest["collection"]["trace_bytes"] = traces[0].stat().st_size

            if status == "pending-artifacts":
                status = "complete"
    except ProfileError as error:
        failure = str(error)
        manifest["error"] = {"message": str(error), "cause": error.cause, "details": error.details}
        # Best-effort graceful shutdown of only this capture's processes.
        request_stop()
        for process in owned:
            if process.poll() is None:
                manifest.setdefault("cleanup", process.terminate(config.shutdown_grace))
    finally:
        for process in owned:
            try:
                process.close()
            except OSError:
                pass

    if failure is not None:
        status = "failed"

    for log_path in sorted(supervisor_logs.glob("*")):
        with contextlib.suppress(OSError):
            shutil.copy2(log_path, output / log_path.name)
    shutil.rmtree(supervisor_logs, ignore_errors=True)

    manifest["capture"]["finished_utc"] = _utc_now()
    manifest["capture"]["duration_seconds"] = round(time.monotonic() - started, 3)
    manifest["capture"]["exit_code"] = exit_code
    manifest["capture"]["status"] = status
    manifest["artifacts"] = _artifact_inventory(output)
    manifest["collection"]["application_pid"] = manifest.get("application_pid")
    manifest["collection"]["total_timeout_seconds"] = config.total_timeout

    _write_json(output / MANIFEST_FILE, manifest)

    if failure is not None:
        raise ProfileError(failure, cause=manifest.get("error", {}).get("cause", "failed"), details=manifest)
    if status != "complete":
        raise ProfileError(
            f"capture {status}: {manifest.get('final_state', {}).get('reason', 'no reason recorded')}",
            cause=status,
            details=manifest,
        )
    return manifest


# ----------------------------------------------------------------------
# vendor adapters
# ----------------------------------------------------------------------


def _parse_number(text: str, number_format: str, location: str) -> float:
    stripped = text.strip()
    if stripped == "":
        raise ProfileError(f"empty numeric field at {location}", cause="parse-failure")
    if number_format == "point":
        if "," in stripped:
            raise ProfileError(
                f"ambiguous number {stripped!r} at {location}: export locale is declared as decimal point",
                cause="locale-mismatch",
            )
        try:
            return float(stripped)
        except ValueError as error:
            raise ProfileError(f"invalid number {stripped!r} at {location}", cause="parse-failure") from error
    if number_format == "comma":
        if "." in stripped and "," in stripped:
            if stripped.rfind(",") > stripped.rfind("."):
                normalized = stripped.replace(".", "").replace(",", ".")
            else:
                normalized = stripped.replace(",", "")
        elif "," in stripped:
            normalized = stripped.replace(",", ".")
        else:
            normalized = stripped
        try:
            return float(normalized)
        except ValueError as error:
            raise ProfileError(f"invalid number {stripped!r} at {location}", cause="parse-failure") from error
    raise ProfileError(f"unknown number format {number_format!r}", cause="invalid-invocation")


def _read_tsv_rows(path: Path, number_format: str, numeric_from: int = 1) -> list[list[str]]:
    rows: list[list[str]] = []
    text = path.read_text(encoding="utf-8", errors="strict")
    for index, line in enumerate(text.splitlines()):
        if line == "":
            continue
        fields = line.split("\t")
        if len(fields) <= numeric_from:
            raise ProfileError(f"{path}:{index + 1} has no numeric columns", cause="parse-failure")
        for column in range(numeric_from, len(fields)):
            _parse_number(fields[column], number_format, f"{path.name}:{index + 1}:{column + 1}")
        rows.append(fields)
    return rows


def _metric_unit(tag: str) -> tuple[str, str]:
    """Splits an observed vendor aggregation suffix into (unit, aggregation).

    Rates stay rates and percentage-of-peak values keep their peak reference;
    a summed per-second counter is not a count.
    """

    lower = tag.lower()
    if lower.endswith(".sum.per_second"):
        return "per second", "rate"
    if lower.endswith(".avg.per_second"):
        return "per second", "mean"
    if lower.endswith(".sum.per_cycle_elapsed"):
        return "per cycle elapsed", "rate"
    if lower.endswith(".avg.per_cycle_elapsed"):
        return "per cycle elapsed", "mean"
    if lower.endswith(".avg.pct_of_peak_sustained_elapsed"):
        return "% of peak sustained elapsed", "mean"
    if lower.endswith(".avg.pct_of_peak_sustained_active"):
        return "% of peak sustained active", "mean"
    if lower.endswith(".sum.pct_of_peak_sustained_elapsed"):
        return "% of peak sustained elapsed", "sum"
    if lower.endswith(".avg.peak_sustained"):
        return "peak sustained", "mean"
    if lower.endswith(".sum.peak_sustained"):
        return "peak sustained", "sum"
    if lower.endswith(".sum"):
        return "count", "sum"
    if lower.endswith(".avg"):
        return "value", "mean"
    if lower.endswith(".max"):
        return "value", "max"
    if lower.endswith(".min"):
        return "value", "min"
    return "value", "mean"


def _metric_family(tag: str) -> str | None:
    """Maps an exact vendor tag family to a normalized metric family."""

    lower = tag.lower()
    if lower == "gpu frame time":
        return "gpu_duration"
    # DRAM/DRAM-controller traffic only; an L1TEX unit throughput is neither
    # memory throughput nor a cache hit rate.
    if "dramc__" in lower or "dram__" in lower or "gpu__dram_" in lower or "lts__throughput" in lower:
        return "memory_throughput"
    if "hit_rate" in lower:
        return "cache_hit_rate"
    if "__warps_active" in lower or "__maximum_warps_per_active_cycle_pct" in lower:
        return "sm_occupancy"
    return None


class GraphicsAdapter:
    """Pinned adapter for the observed 2026.4 GPU Trace tab-separated export."""

    def __init__(self, root: Path, number_format: str = "point") -> None:
        self.root = root
        self.number_format = number_format

    def _clock_directory(self) -> Path:
        candidates = [path for path in self.root.iterdir() if path.is_dir()] if self.root.is_dir() else []
        for candidate in sorted(candidates):
            if (candidate / "REPRO_INFO.xls").is_file():
                return candidate
        raise ProfileError(
            f"no Nsight Graphics export directory with REPRO_INFO.xls under {self.root}", cause="unknown-schema"
        )

    def _repro_info(self, directory: Path) -> dict[str, str]:
        info: dict[str, str] = {}
        path = directory / "REPRO_INFO.xls"
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            if "\t" not in line:
                continue
            key, _, value = line.partition("\t")
            info[key.strip()] = value.strip()
        return info

    def analyze(self) -> dict[str, Any]:
        directory = self._clock_directory()
        repro = self._repro_info(directory)
        for required in ("Device Name", "Driver Version", "Metric Set"):
            if required not in repro:
                raise ProfileError(
                    f"{directory / 'REPRO_INFO.xls'} lacks the expected {required!r} record", cause="unknown-schema"
                )

        frame_path = directory / "FRAME.xls"
        marker_path = directory / "D3DPERF_EVENTS.xls"
        frame_metrics_path = directory / "GPUTRACE_FRAME.xls"
        if not frame_path.is_file() or not frame_metrics_path.is_file():
            raise ProfileError(
                f"{directory} lacks FRAME.xls or GPUTRACE_FRAME.xls", cause="unknown-schema"
            )

        frame_rows = _read_tsv_rows(frame_path, self.number_format)
        if len(frame_rows) != 1 or frame_rows[0][0].strip() != "GPU frame time":
            raise ProfileError(
                f"{frame_path} does not start with the observed 'GPU frame time' row", cause="unknown-schema"
            )
        frame_values = [_parse_number(value, self.number_format, f"FRAME.xls:{index + 2}") for index, value in enumerate(frame_rows[0][1:])]
        observed_frames = len(frame_values)

        ranges: list[dict[str, Any]] = []
        markers: list[dict[str, Any]] = []
        marker_names: list[str] = []
        if marker_path.is_file():
            # Rows nest through leading indentation; parents are recorded per
            # frame so no parent/child pair is merged across frames.
            stack: list[tuple[int, list[int]]] = []
            for line_index, line in enumerate(marker_path.read_text(encoding="utf-8").splitlines()):
                if not line.strip():
                    continue
                label, *values = line.split("\t")
                if label.strip() == "event_text":
                    continue
                depth = (len(label) - len(label.lstrip(" "))) // 2
                name = label.strip()
                while stack and stack[-1][0] >= depth:
                    stack.pop()
                parents = stack[-1][1] if stack else []
                row_ids: list[int] = []
                for frame_index, value_text in enumerate(values):
                    value = _parse_number(value_text, self.number_format, f"D3DPERF_EVENTS.xls:{line_index + 1}")
                    if value <= 0.0:
                        continue
                    parent_id = parents[frame_index] if frame_index < len(parents) else None
                    ranges.append(
                        {
                            "id": len(ranges),
                            "parent_id": parent_id,
                            "stable_name": name,
                            "frame_id": frame_index,
                            "queue": "graphics",
                            "start_ns": None,
                            "duration_ns": int(round(value * 1_000_000.0)),
                            "inclusive": True,
                            "provenance": "Nsight Graphics D3DPERF marker duration per frame (milliseconds exported)",
                            "shader": None,
                            "pipeline": None,
                        }
                    )
                    while len(row_ids) <= frame_index:
                        row_ids.append(-1)
                    row_ids[frame_index] = len(ranges) - 1
                if name not in marker_names:
                    marker_names.append(name)
                stack.append((depth, row_ids))

        metrics: list[dict[str, Any]] = []
        raw_location = str(frame_metrics_path.relative_to(self.root.parent))
        for row_index, row in enumerate(_read_tsv_rows(frame_metrics_path, self.number_format)):
            tag = row[0].strip()
            unit, aggregation = _metric_unit(tag)
            name = tag.split(".", 1)[1] if tag.startswith("FE_A.") else tag
            for frame_index, value_text in enumerate(row[1:]):
                metrics.append(
                    {
                        "range_id": None,
                        "frame_id": frame_index,
                        "vendor_id": tag,
                        "name": name,
                        "unit": unit,
                        "value": _parse_number(value_text, self.number_format, f"GPUTRACE_FRAME.xls:{row_index + 1}"),
                        "availability": "observed",
                        "reason": None,
                        "aggregation": aggregation,
                        "raw_artifact": raw_location,
                        "raw_location": f"row {row_index + 1}, frame {frame_index}",
                    }
                )

        # Frame-level GPU duration is a first-class metric, never mixed with CPU
        # API time.
        for frame_index, value_ms in enumerate(frame_values):
            metrics.append(
                {
                    "range_id": None,
                    "frame_id": frame_index,
                    "vendor_id": "GPU frame time",
                    "name": "gpu.frame_time",
                    "unit": "ms",
                    "value": value_ms,
                    "availability": "observed",
                    "reason": None,
                    "aggregation": "mean",
                    "raw_artifact": str(frame_path.relative_to(self.root.parent)),
                    "raw_location": f"frame {frame_index}",
                }
            )

        observed_families = {
            family
            for entry in metrics
            if (family := _metric_family(str(entry.get("vendor_id") or ""))) is not None
        }
        metrics.extend(
            _unavailable_metrics(
                metrics,
                observed_families,
                "the selected metric set does not export {family}; no value is inferred",
            )
        )

        # Production stage markers, observed names. A complete frame carries the
        # scene stage (A), resolve/post (B) and present (C) ranges.
        stage_names = ("scene.stage", "Post.AgX", "Present.Scene")
        frames_with_stage: dict[int, set[str]] = {}
        for entry in ranges:
            frames_with_stage.setdefault(int(entry["frame_id"]), set()).add(entry["stable_name"])
        complete_abc = [
            frame for frame in range(observed_frames) if all(name in frames_with_stage.get(frame, set()) for name in stage_names)
        ]

        return {
            "vendor": {
                "product": "NVIDIA Nsight Graphics",
                "version": repro.get("Product Version"),
                "export_format": "gpu-trace-tab-separated",
                "clock_state": directory.name,
                "repro": repro,
            },
            "declared_metric_set": repro.get("Metric Set"),
            "clocks": repro.get("GPU Clocks", "unaltered"),
            "frames": observed_frames,
            "frames_abc_complete": complete_abc,
            "stage_marker_names": list(stage_names),
            "ranges": ranges,
            "metrics": metrics,
            "markers": marker_names,
            "number_format": self.number_format,
        }


def _unavailable_metrics(
    metrics: Iterable[dict[str, Any]], observed_families: set[str], reason: str
) -> list[dict[str, Any]]:
    """Adds explicit null entries for normalized families the source cannot provide."""

    entries: list[dict[str, Any]] = []
    for family in DESIRED_METRIC_FAMILIES:
        if family in observed_families:
            continue
        entries.append(
            {
                "range_id": None,
                "frame_id": None,
                "vendor_id": None,
                "name": f"unavailable.{family}",
                "unit": None,
                "value": None,
                "availability": "unavailable",
                "reason": reason.format(family=family),
                "aggregation": "none",
                "raw_artifact": None,
                "raw_location": None,
            }
        )
    return entries


class SystemsAdapter:
    """Pinned adapter for the observed Nsight Systems 2026.5 SQLite export."""

    def __init__(self, path: Path) -> None:
        self.path = path

    def _connect(self) -> sqlite3.Connection:
        connection = sqlite3.connect(f"file:{self.path}?mode=ro", uri=True)
        connection.row_factory = sqlite3.Row
        return connection

    def _metadata(self, connection: sqlite3.Connection) -> dict[str, str]:
        try:
            rows = connection.execute("SELECT name, value FROM META_DATA_EXPORT").fetchall()
        except sqlite3.DatabaseError as error:
            raise ProfileError(f"{self.path} is not an Nsight Systems SQLite export: {error}", cause="unknown-schema") from error
        return {row["name"]: row["value"] for row in rows}

    def _strings(self, connection: sqlite3.Connection) -> dict[int, str]:
        return {row["id"]: row["value"] for row in connection.execute("SELECT id, value FROM StringIds")}

    def analyze(self) -> dict[str, Any]:
        connection = self._connect()
        try:
            metadata = self._metadata(connection)
            version = metadata.get("EXPORT_SCHEMA_VERSION", "")
            if version != SYSTEMS_EXPORT_SCHEMA:
                raise ProfileError(
                    f"Nsight Systems SQLite export schema {version or 'unknown'} is not the pinned "
                    f"{SYSTEMS_EXPORT_SCHEMA}; update the adapter against the observed export",
                    cause="unknown-schema",
                    details={"schema": version},
                )
            strings = self._strings(connection)

            cpu_ranges: list[dict[str, Any]] = []
            cpu_totals: dict[str, dict[str, float]] = {}
            presents: list[int] = []
            present_threads: dict[int, int] = {}
            for row in connection.execute(
                "SELECT start, end, globalTid, nameId FROM VULKAN_API ORDER BY start"
            ):
                name = strings.get(row["nameId"], f"api-{row['nameId']}")
                duration = int(row["end"]) - int(row["start"])
                if duration <= 0:
                    continue
                entry = cpu_totals.setdefault(name, {"total_ns": 0.0, "count": 0.0, "max_ns": 0.0})
                entry["total_ns"] += float(duration)
                entry["count"] += 1.0
                entry["max_ns"] = max(entry["max_ns"], float(duration))
                if name == "vkQueuePresentKHR":
                    presents.append(int(row["end"]))
                    present_threads[int(row["globalTid"])] = present_threads.get(int(row["globalTid"]), 0) + 1
                if name in ("vkQueueSubmit", "vkQueuePresentKHR", "vkWaitForFences", "vkQueueWaitIdle"):
                    cpu_ranges.append(
                        {
                            "id": len(cpu_ranges),
                            "parent_id": None,
                            "stable_name": name,
                            "frame_id": None,
                            "queue": "cpu",
                            "start_ns": int(row["start"]),
                            "duration_ns": duration,
                            "inclusive": False,
                            "provenance": "nsys VULKAN_API CPU API-call duration (not GPU execution)",
                            "shader": None,
                            "pipeline": None,
                        }
                    )

            gpu_intervals: list[tuple[int, int]] = []
            gpu_ranges: list[dict[str, Any]] = []
            for row in connection.execute(
                "SELECT start, end, globalTid, nameId FROM VULKAN_WORKLOAD ORDER BY start"
            ):
                start = int(row["start"])
                end = int(row["end"])
                if end <= start:
                    continue
                name = strings.get(row["nameId"], "vkQueueSubmit")
                gpu_intervals.append((start, end))
                gpu_ranges.append(
                    {
                        "id": len(cpu_ranges) + len(gpu_ranges),
                        "parent_id": None,
                        "stable_name": name,
                        "frame_id": None,
                        "queue": "gpu",
                        "start_ns": start,
                        "duration_ns": end - start,
                        "inclusive": False,
                        "provenance": (
                            "nsys VULKAN_WORKLOAD queue-workload window; not a per-pass GPU execution range"
                        ),
                        "shader": None,
                        "pipeline": None,
                    }
                )

            busy_ns = _interval_union_ns(gpu_intervals)
            metrics: list[dict[str, Any]] = [
                {
                    "range_id": None,
                    "frame_id": None,
                    "vendor_id": "gpu_workload_union",
                    "name": "gpu.workload_busy",
                    "unit": "ns",
                    "value": float(busy_ns),
                    "availability": "observed",
                    "reason": None,
                    "aggregation": "union",
                    "raw_artifact": self.path.name,
                    "raw_location": "VULKAN_WORKLOAD union of queue windows",
                },
                {
                    "range_id": None,
                    "frame_id": None,
                    "vendor_id": "gpu_workload_count",
                    "name": "gpu.workload_count",
                    "unit": "count",
                    "value": float(len(gpu_intervals)),
                    "availability": "observed",
                    "reason": None,
                    "aggregation": "count",
                    "raw_artifact": self.path.name,
                    "raw_location": "VULKAN_WORKLOAD",
                },
                {
                    "range_id": None,
                    "frame_id": None,
                    "vendor_id": "present_count",
                    "name": "frame.present_count",
                    "unit": "count",
                    "value": float(len(presents)),
                    "availability": "observed",
                    "reason": None,
                    "aggregation": "count",
                    "raw_artifact": self.path.name,
                    "raw_location": "VULKAN_API vkQueuePresentKHR",
                },
            ]
            for name, entry in sorted(cpu_totals.items()):
                metrics.append(
                    {
                        "range_id": None,
                        "frame_id": None,
                        "vendor_id": f"cpu_api.{name}",
                        "name": f"cpu.api.{name}.total",
                        "unit": "ns",
                        "value": entry["total_ns"],
                        "availability": "observed",
                        "reason": None,
                        "aggregation": "sum",
                        "raw_artifact": self.path.name,
                        "raw_location": "VULKAN_API",
                    }
                )
                metrics.append(
                    {
                        "range_id": None,
                        "frame_id": None,
                        "vendor_id": f"cpu_api.{name}",
                        "name": f"cpu.api.{name}.max",
                        "unit": "ns",
                        "value": entry["max_ns"],
                        "availability": "observed",
                        "reason": None,
                        "aggregation": "max",
                        "raw_artifact": self.path.name,
                        "raw_location": "VULKAN_API",
                    }
                )

            markers: list[str] = []
            has_nvtx = connection.execute(
                "SELECT name FROM sqlite_master WHERE type='table' AND name='NVTX_EVENTS'"
            ).fetchone()
            if has_nvtx:
                markers = [
                    row["value"]
                    for row in connection.execute(
                        "SELECT DISTINCT s.value FROM NVTX_EVENTS n JOIN StringIds s ON s.id = n.textId "
                        "WHERE s.value IS NOT NULL ORDER BY s.value"
                    )
                ]

            metrics.extend(
                _unavailable_metrics(
                    metrics,
                    set(),
                    "Systems records API/workload windows on this host, not GPU {family}; no value is inferred",
                )
            )

            return {
                "vendor": {
                    "product": metadata.get("EXPORT_PRODUCT_NAME"),
                    "version": metadata.get("EXPORT_PRODUCT_VERSION"),
                    "export_format": "nsys-sqlite",
                    "export_schema": version,
                    "platform": metadata.get("EXPORT_PLATFORM"),
                },
                "declared_metric_set": None,
                "clocks": "unaltered",
                "frames": len(presents),
                "frames_abc_complete": None,
                "stage_marker_names": [],
                "ranges": cpu_ranges + gpu_ranges,
                "metrics": metrics,
                "markers": markers,
                "number_format": "point",
            }
        finally:
            connection.close()


def _interval_union_ns(intervals: Iterable[tuple[int, int]]) -> int:
    """Union length, so nested/overlapping ranges are never double-counted."""
    ordered = sorted(intervals)
    if not ordered:
        return 0
    total = 0
    current_start, current_end = ordered[0]
    for start, end in ordered[1:]:
        if start > current_end:
            total += current_end - current_start
            current_start, current_end = start, end
        else:
            current_end = max(current_end, end)
    total += current_end - current_start
    return total


# ----------------------------------------------------------------------
# analysis
# ----------------------------------------------------------------------


def _analysis_identity(input_directory: Path) -> tuple[dict[str, Any], dict[str, Any], dict[str, Any]]:
    manifest = _read_json_if_present(input_directory / MANIFEST_FILE) or {}
    ready = _read_json_if_present(input_directory / READY_FILE) or {}
    final_state = _read_json_if_present(input_directory / FINAL_STATE_FILE) or {}
    return manifest, ready, final_state


def analyze(input_directory: Path, output_directory: Path, number_format: str | None = None) -> dict[str, Any]:
    input_directory = input_directory.expanduser().resolve()
    output_directory = output_directory.expanduser().resolve()
    if not input_directory.is_dir():
        raise ProfileError(f"analysis input directory not found: {input_directory}", cause="invalid-invocation")

    manifest, ready, final_state = _analysis_identity(input_directory)
    tool = manifest.get("capture", {}).get("tool") or ("graphics" if (input_directory / "graphics").is_dir() else "systems")
    declared_format = (
        number_format
        or manifest.get("vendor", {}).get("number_format")
        or "point"
    )
    if declared_format not in ("point", "comma"):
        raise ProfileError(f"unsupported number format {declared_format!r}", cause="invalid-invocation")

    if tool == TOOL_GRAPHICS:
        graphics_root = input_directory / "graphics"
        if not graphics_root.is_dir():
            raise ProfileError(f"graphics export directory missing under {input_directory}", cause="missing-export")
        vendor = GraphicsAdapter(graphics_root, declared_format).analyze()
    else:
        sqlite_path = input_directory / "systems.sqlite"
        if not sqlite_path.is_file():
            raise ProfileError(f"systems SQLite export missing: {sqlite_path}", cause="missing-export")
        vendor = SystemsAdapter(sqlite_path).analyze()

    observed_markers = list(vendor["markers"])
    workload = (manifest.get("capture") or {}).get("workload")
    final_status = final_state.get("status") if isinstance(final_state, dict) else None

    markers_applicable = tool == TOOL_GRAPHICS
    optional_markers = list(FROZEN_OPTIONAL_MARKERS) if workload == "frozen" else []
    if markers_applicable:
        missing_markers = [
            marker
            for marker in REQUIRED_GPU_MARKERS
            if marker not in observed_markers and marker not in optional_markers
        ]
        missing_optional_markers = [marker for marker in optional_markers if marker not in observed_markers]
    else:
        missing_markers = []
        missing_optional_markers = []

    settings = ready.get("settings") if isinstance(ready, dict) else None
    camera = ready.get("camera") if isinstance(ready, dict) else None
    extents = ready.get("extents") if isinstance(ready, dict) else None

    identity = {
        "source_commit": (manifest.get("identity") or {}).get("source_commit"),
        "dirty_diff_hash": (manifest.get("identity") or {}).get("dirty_diff_hash"),
        "binary_sha256": (manifest.get("identity") or {}).get("executable_sha256"),
        "binary_bytes": (manifest.get("identity") or {}).get("executable_bytes"),
        "build_preset": (manifest.get("identity") or {}).get("build_preset"),
        "tool_versions": {
            TOOL_SYSTEMS: ((manifest.get("tools") or {}).get(TOOL_SYSTEMS) or {}).get("version"),
            TOOL_GRAPHICS: ((manifest.get("tools") or {}).get(TOOL_GRAPHICS) or {}).get("version"),
        },
        "os": (manifest.get("environment") or {}).get("os"),
        "backend": ((manifest.get("environment") or {}).get("gpu") or {}).get("backend")
        or "vulkan",
        "gpu": ((manifest.get("environment") or {}).get("gpu") or {}).get("name"),
        "driver": ((manifest.get("environment") or {}).get("gpu") or {}).get("driver_version"),
        "architecture": ((manifest.get("environment") or {}).get("gpu") or {}).get("architecture"),
        "map": "lighting_lab",
        "map_size": 1024,
        "seed": 42,
        "checkpoint": None,
        "camera": camera,
        "extents": extents,
        "settings": settings,
        "observed": {
            "world_id": ready.get("world_id") if isinstance(ready, dict) else None,
            "publication_id": ready.get("publication_id") if isinstance(ready, dict) else None,
            "tick": ready.get("tick") if isinstance(ready, dict) else None,
            "accepted_scene_frames": ready.get("accepted_scene_frames") if isinstance(ready, dict) else None,
            "active_presentation_seconds": (
                ready.get("active_presentation_seconds") if isinstance(ready, dict) else None
            ),
            "simulation_frame_range": [
                (final_state or {}).get("ready_tick"),
                (final_state or {}).get("stop_tick"),
            ],
            "lighting_frame_range": [
                (final_state or {}).get("lighting_frame_start"),
                (final_state or {}).get("lighting_frame_end"),
            ],
            "screen_frame_range": [
                (final_state or {}).get("screen_frame_start"),
                (final_state or {}).get("screen_frame_end"),
            ],
        },
        "number_format": vendor.get("number_format", declared_format),
        "tooling": (manifest.get("identity") or {}).get("tooling"),
        "sdk": None,
    }

    collection = {
        "tool": tool,
        "mode": (manifest.get("capture") or {}).get("workload"),
        "clocks": vendor.get("clocks"),
        "metric_set": vendor.get("declared_metric_set")
        or ((manifest.get("collection") or {}).get("metric_set")),
        "replay_identity": None,
        "interval_seconds": (manifest.get("collection") or {}).get("interval_seconds"),
        "requested_frames": (manifest.get("collection") or {}).get("requested_frames"),
        "observed_complete_frames": vendor.get("frames"),
        "observed_complete_abc_frames": vendor.get("frames_abc_complete"),
        "stage_marker_names": vendor.get("stage_marker_names", []),
        "overhead": {
            "note": (
                "Capture-time instrumentation and export cost are not normal frame timing; compare matched captures "
                "only and report distributions with sample counts."
            )
        },
        "artifacts": manifest.get("artifacts", {}),
        "vendor": vendor.get("vendor"),
    }

    metrics = sorted(
        vendor["metrics"],
        key=lambda entry: (
            str(entry["name"]),
            -1 if entry.get("frame_id") is None else int(entry["frame_id"]),
            str(entry.get("vendor_id")),
        ),
    )
    ranges = sorted(vendor["ranges"], key=lambda entry: (entry["id"],))

    frames_present = isinstance(vendor.get("frames"), int) and vendor["frames"] > 0
    abc_frames = vendor.get("frames_abc_complete")
    abc_present = bool(abc_frames) if markers_applicable else None
    shutdown_clean = final_status == "complete"

    if markers_applicable:
        checks = {
            "gpu_markers_applicable": True,
            "gpu_markers_present": not missing_markers,
            "complete_abc_frame_present": abc_present,
            "frames_present": frames_present,
            "shutdown_clean": shutdown_clean,
        }
        complete = checks["gpu_markers_present"] and checks["complete_abc_frame_present"] and frames_present and shutdown_clean
    else:
        # Systems cannot observe GPU pass ranges. Its own evidence must be
        # sufficient: a validated schema, a nonempty API/workload trace and a
        # clean application shutdown.
        checks = {
            "gpu_markers_applicable": False,
            "vendor_schema_validated": True,
            "frames_present": frames_present,
            "shutdown_clean": shutdown_clean,
        }
        complete = frames_present and shutdown_clean

    validation = {
        "vendor_schema": "ok",
        "required_ranges_present": (not missing_markers) if markers_applicable else None,
        "required_markers_applicable": markers_applicable,
        "required_markers": list(REQUIRED_GPU_MARKERS) if markers_applicable else [],
        "optional_markers": optional_markers,
        "observed_markers": observed_markers,
        "missing_markers": missing_markers,
        "missing_optional_markers": missing_optional_markers,
        "checks": checks,
        "complete_frames": vendor.get("frames"),
        "complete_abc_frames": abc_frames,
        "abc_frames_present": abc_present,
        "missing_counters": [entry["name"] for entry in metrics if entry["availability"] != "observed"],
        "counter_summary": {
            "distinct_metrics": len({str(entry["name"]) for entry in metrics if entry["availability"] == "observed"}),
            "observed_samples": sum(1 for entry in metrics if entry["availability"] == "observed"),
            "unavailable_families": sum(1 for entry in metrics if entry["availability"] != "observed"),
        },
        "parser_failures": [],
        "shutdown": {
            "status": final_status,
            "clean": shutdown_clean,
            "reason": (final_state or {}).get("reason"),
        },
        "overall_status": "failed" if final_status == "failed" else ("complete" if complete else "incomplete"),
    }

    analysis = {
        "schema_version": SCHEMA_VERSION,
        "identity": identity,
        "collection": collection,
        "ranges": ranges,
        "metrics": metrics,
        "validation": validation,
        "annotations": [
            {
                "source": "vendor",
                "text": (
                    "Vendor metric names, units and aggregation suffixes are retained verbatim from the exported "
                    "capture; no thresholds or optimization instructions are inherited."
                ),
            }
        ],
    }

    output_directory.mkdir(parents=True, exist_ok=True)
    _write_json(output_directory / ANALYSIS_JSON, analysis)
    _write_yaml(output_directory / ANALYSIS_YAML, analysis)
    return analysis


# ----------------------------------------------------------------------
# comparison
# ----------------------------------------------------------------------


def _distribution(values: Sequence[float]) -> dict[str, Any]:
    if not values:
        return {"count": 0, "min": None, "median": None, "max": None, "mean": None}
    return {
        "count": len(values),
        "min": min(values),
        "median": statistics.median(values),
        "max": max(values),
        "mean": statistics.fmean(values),
    }


IDENTITY_FIELDS = (
    ("identity", "source_commit"),
    ("identity", "dirty_diff_hash"),
    ("identity", "binary_sha256"),
    ("identity", "build_preset"),
    ("identity", "map"),
    ("identity", "map_size"),
    ("identity", "seed"),
    ("identity", "backend"),
    ("identity", "gpu"),
    ("identity", "driver"),
    ("collection", "tool"),
    ("collection", "mode"),
    ("collection", "clocks"),
    ("collection", "metric_set"),
    ("collection", "replay_identity"),
)


def _identity_differences(baseline: dict[str, Any], candidate: dict[str, Any]) -> list[dict[str, Any]]:
    differences: list[dict[str, Any]] = []
    for section, key in IDENTITY_FIELDS:
        left = (baseline.get(section) or {}).get(key)
        right = (candidate.get(section) or {}).get(key)
        if left != right:
            differences.append({"field": f"{section}.{key}", "baseline": left, "candidate": right})
    for section, key in (("identity", "camera"), ("identity", "settings")):
        left = (baseline.get(section) or {}).get(key)
        right = (candidate.get(section) or {}).get(key)
        if left != right:
            differences.append({"field": f"{section}.{key}", "baseline": left, "candidate": right})
    return differences


def _metric_distributions(analysis: dict[str, Any]) -> dict[str, dict[str, Any]]:
    groups: dict[str, list[float]] = {}
    units: dict[str, str] = {}
    for entry in analysis.get("metrics", []):
        if entry.get("availability") != "observed" or entry.get("value") is None:
            continue
        name = str(entry["name"])
        groups.setdefault(name, []).append(float(entry["value"]))
        units.setdefault(name, str(entry.get("unit")))
    return {
        name: {"unit": units[name], **_distribution(values)}
        for name, values in sorted(groups.items())
    }


def compare(baseline_directory: Path, candidate_directory: Path, output_directory: Path) -> dict[str, Any]:
    def load(directory: Path) -> dict[str, Any]:
        directory = directory.expanduser().resolve()
        path = directory / ANALYSIS_JSON
        if not path.is_file():
            raise ProfileError(f"analysis.json missing in {directory}; run analyze first", cause="missing-analysis")
        value = _json_load(path)
        if not isinstance(value, dict) or value.get("schema_version") != SCHEMA_VERSION:
            raise ProfileError(f"{path} is not schema_version {SCHEMA_VERSION}", cause="unknown-schema")
        return value

    baseline = load(baseline_directory)
    candidate = load(candidate_directory)
    differences = _identity_differences(baseline, candidate)

    baseline_metrics = _metric_distributions(baseline)
    candidate_metrics = _metric_distributions(candidate)
    shared = sorted(set(baseline_metrics) & set(candidate_metrics))
    distributed = {
        name: {
            "unit": baseline_metrics[name]["unit"],
            "baseline": baseline_metrics[name],
            "candidate": candidate_metrics[name],
        }
        for name in shared
    }

    comparison = {
        "schema_version": SCHEMA_VERSION,
        "comparable": not differences,
        "conclusion": (
            "like-for-like"
            if not differences
            else "not like-for-like: identities differ; descriptive comparison only"
        ),
        "identity_differences": differences,
        "baseline": {
            "identity": baseline.get("identity"),
            "collection": {
                key: (baseline.get("collection") or {}).get(key)
                for key in ("tool", "mode", "clocks", "metric_set", "observed_complete_frames")
            },
            "validation": baseline.get("validation"),
        },
        "candidate": {
            "identity": candidate.get("identity"),
            "collection": {
                key: (candidate.get("collection") or {}).get(key)
                for key in ("tool", "mode", "clocks", "metric_set", "observed_complete_frames")
            },
            "validation": candidate.get("validation"),
        },
        "metrics": distributed,
        "profiler_overhead": {
            "baseline": (baseline.get("collection") or {}).get("overhead"),
            "candidate": (candidate.get("collection") or {}).get("overhead"),
        },
    }

    output_directory = output_directory.expanduser().resolve()
    output_directory.mkdir(parents=True, exist_ok=True)
    _write_json(output_directory / COMPARISON_JSON, comparison)
    _write_yaml(output_directory / COMPARISON_YAML, comparison)
    return comparison


# ----------------------------------------------------------------------
# command line
# ----------------------------------------------------------------------


def _default_architecture(discovered: dict[str, Any]) -> str | None:
    return (discovered.get("gpu") or {}).get("architecture")


def _default_metric_set(discovered: dict[str, Any]) -> str | None:
    graphics = discovered.get("graphics") or {}
    return graphics.get("selected_metric_set")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Bounded lighting profiling with Nsight exports.")
    subparsers = parser.add_subparsers(dest="command", required=True)

    discover_parser = subparsers.add_parser("discover", help="record installed tools, GPU and SDK support")
    discover_parser.add_argument("--json", type=Path, default=None, help="write the discovery record here")
    discover_parser.add_argument("--nsys", default=None)
    discover_parser.add_argument("--ngfx", default=None)

    capture_parser = subparsers.add_parser("capture", help="run one bounded capture and retain its artifacts")
    capture_parser.add_argument("--tool", choices=(TOOL_SYSTEMS, TOOL_GRAPHICS), required=True)
    capture_parser.add_argument("--exe", type=Path, required=True)
    capture_parser.add_argument("--output", type=Path, required=True)
    capture_parser.add_argument("--workload", choices=WORKLOADS, default="frozen")
    capture_parser.add_argument("--nsys", default=None)
    capture_parser.add_argument("--ngfx", default=None)
    capture_parser.add_argument("--architecture", default=None)
    capture_parser.add_argument("--metric-set", default=None)

    analyze_parser = subparsers.add_parser("analyze", help="normalize retained vendor exports offline")
    analyze_parser.add_argument("--input", type=Path, required=True)
    analyze_parser.add_argument("--output", type=Path, required=True)
    analyze_parser.add_argument("--number-format", choices=("point", "comma"), default=None)

    compare_parser = subparsers.add_parser("compare", help="compare two analyzed captures")
    compare_parser.add_argument("--baseline", type=Path, required=True)
    compare_parser.add_argument("--candidate", type=Path, required=True)
    compare_parser.add_argument("--output", type=Path, required=True)

    try:
        arguments = parser.parse_args(argv)
    except SystemExit as exit_request:
        # argparse owns usage diagnostics; keep main()'s int contract.
        return int(exit_request.code or 0)

    try:
        if arguments.command == "discover":
            record = discover(arguments.nsys, arguments.ngfx)
            if arguments.json:
                _write_json(arguments.json, record)
            print(json.dumps(record, indent=2))
            ok = all(
                (record["tools"][tool].get("path") is not None)
                for tool in (TOOL_SYSTEMS, TOOL_GRAPHICS)
            )
            return 0 if ok else 3

        if arguments.command == "capture":
            record = discover(arguments.nsys, arguments.ngfx)
            config = CaptureConfig(
                tool=arguments.tool,
                executable=arguments.exe,
                output=arguments.output,
                workload=arguments.workload,
                nsys=Path(record["tools"][TOOL_SYSTEMS]["path"]) if record["tools"][TOOL_SYSTEMS]["path"] else None,
                ngfx=Path(record["tools"][TOOL_GRAPHICS]["path"]) if record["tools"][TOOL_GRAPHICS]["path"] else None,
                architecture=arguments.architecture or _default_architecture(record),
                metric_set=arguments.metric_set or _default_metric_set(record),
            )
            manifest = capture(config)
            print(
                json.dumps(
                    {
                        "status": manifest["capture"]["status"],
                        "output": manifest["capture"]["output"],
                        "manifest": str(Path(manifest["capture"]["output"]) / MANIFEST_FILE),
                    },
                    indent=2,
                )
            )
            return 0

        if arguments.command == "analyze":
            analysis = analyze(arguments.input, arguments.output, arguments.number_format)
            print(
                json.dumps(
                    {
                        "schema_version": analysis["schema_version"],
                        "status": analysis["validation"]["overall_status"],
                        "missing_markers": analysis["validation"]["missing_markers"],
                        "metrics": len(analysis["metrics"]),
                        "output": str(arguments.output),
                    },
                    indent=2,
                )
            )
            return 0 if analysis["validation"]["overall_status"] == "complete" else 3

        comparison = compare(arguments.baseline, arguments.candidate, arguments.output)
        print(json.dumps({"comparable": comparison["comparable"], "conclusion": comparison["conclusion"]}, indent=2))
        return 0 if comparison["comparable"] else 3
    except ProfileError as error:
        print(f"profile_lighting: {error}", file=sys.stderr)
        if error.details is not None:
            print(json.dumps(error.details, indent=2), file=sys.stderr)
        return 1 if error.cause == "failed" else 3


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
