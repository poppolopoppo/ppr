#!/usr/bin/env python3
"""Parse compilation profiling data and generate structured reports.

Input sources:
  1. /Bt+ text output (from CL=/Bt+ builds) - per-file front/back/total
  2. vcperf timetrace JSON (Chrome Event Format) - per-file with categories
  3. Raw build duration (Measure-Command or ninja timing)

Output: JSON report + human-readable summary to stdout.
"""

import argparse
import json
import os
import re
import sys
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

# Force UTF-8 output on Windows
if sys.platform == "win32":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")  # type: ignore[attr-defined]

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

# Format: time(C:\...\c1xx.dll)=5.52699s < ts - ts > BB [filepath]
# Each compilation produces two consecutive lines: c1xx (front) then c2 (back).
RE_BTPLUS = re.compile(
    r"^time\([^)]+\)=([\d.]+)s\s*<\s*\d+\s*-\s*\d+\s*>\s*BB\s*\[(.+)\]"
)

STD_MODULE_PREFIXES = ("std:", "std.compat:", "std.",)
STD_FILE_PATTERNS = re.compile(
    r"(std\s+(?:module|partition)|/usr/include/c\+\+|C:|/builds/|vcpkg)", re.IGNORECASE
)

# ---------------------------------------------------------------------------
# Parsers
# ---------------------------------------------------------------------------


def _btplus_normalize(path: str) -> str:
    """Normalize a /Bt+ file path to a short project-relative form."""
    path = path.replace("\\", "/")
    msvc_marker = "Microsoft Visual Studio/18/Insiders/VC/Tools/MSVC/"
    if msvc_marker in path:
        return "std:" + path.rsplit("/", 1)[-1]
    if "Windows Kits" in path:
        return "sdk:" + "/".join(path.split("/")[-3:])
    if path.startswith("E:/Code/ppr/"):
        return path[len("E:/Code/ppr/"):]
    return path


def parse_btplus(text: str) -> dict[str, Any]:
    """Parse /Bt+ compiler timing output into structured data.

    MSVC /Bt+ format (two consecutive lines per compilation):
      time(C:/path/to/c1xx.dll)=<front>s < ts - ts > BB [file]
      time(C:/path/to/c2.dll)  =<back>s  < ts - ts > BB [file]
    """
    raw: list[tuple[str, float, str]] = []  # (phase, seconds, filepath)

    for line in text.splitlines():
        m = RE_BTPLUS.match(line)
        if not m:
            continue

        seconds = float(m.group(1))
        filepath = m.group(2)
        line_lower = line.lower()

        # Classify c1xx = front-end, c2 = back-end
        if "c1xx" in line_lower:
            phase = "front"
        elif "c2" in line_lower and "c1xx" not in line_lower:
            phase = "back"
        else:
            continue  # unknown DLL, skip

        raw.append((phase, seconds, filepath))

    # Aggregate by file (front + back may arrive out of order with parallel builds)
    files: dict[str, dict] = {}
    for phase, seconds, filepath in raw:
        name = _btplus_normalize(filepath)
        if name not in files:
            files[name] = {"front": 0.0, "back": 0.0, "total": 0.0}
        ms = seconds * 1000.0
        files[name][phase] += ms
        files[name]["total"] += ms

    return {
        "source": "btplus",
        "files": files,
        "totals": {},
    }


def parse_timetrace(json_path: str) -> dict[str, Any]:
    """Parse vcperf timetrace JSON (Chrome Event Format)."""
    with open(json_path, encoding="utf-8") as f:
        data = json.load(f)

    events = data.get("traceEvents", data if isinstance(data, list) else [])

    # Group by pid, sort by ts
    by_pid: dict[int, list] = {}
    for evt in events:
        if not isinstance(evt, dict):
            continue
        pid = evt.get("pid", 0)
        by_pid.setdefault(pid, []).append(evt)

    cl_invocations: list[dict] = []

    for pid, pid_evts in by_pid.items():
        pid_evts.sort(key=lambda e: e.get("ts", 0))
        stack: list[dict] = []
        current_cl: dict | None = None

        for evt in pid_evts:
            ph = evt.get("ph", "")
            ts = evt.get("ts", 0)
            name = evt.get("name", "")
            dur = evt.get("dur", 0)

            if ph == "B":
                entry = {"name": name, "ts": ts, "args": evt.get("args", {})}
                stack.append(entry)

                if "CL Invocation" in name:
                    args = entry["args"]
                    fname = args.get("File Input", "")
                    if fname:
                        fname = fname.replace("\\", "/")
                        # Shorten long VS paths
                        short = fname
                        msvc_marker = (
                            "Microsoft Visual Studio/18/Insiders/VC/Tools/MSVC/"
                        )
                        if msvc_marker in fname:
                            short = "std:" + fname.rsplit("/", 1)[-1]
                        elif "Windows Kits" in fname:
                            short = "sdk:" + "/".join(fname.split("/")[-3:])
                        elif fname.startswith("E:/Code/ppr/"):
                            short = fname[len("E:/Code/ppr/"):]

                        current_cl = {
                            "file": fname,
                            "file_short": short,
                            "start": ts,
                            "phases": {},
                        }

            elif ph == "E":
                if stack:
                    begin = stack.pop()
                    elapsed = ts - begin["ts"]

                    bname = begin["name"]
                    if "CL Invocation" in bname and current_cl:
                        current_cl["duration"] = elapsed
                        current_cl["end"] = ts
                        cl_invocations.append(current_cl)
                        current_cl = None
                    elif current_cl is not None:
                        # Nest sub-phase timing: skip empty names
                        if bname:
                            current_cl["phases"].setdefault(bname, 0)
                            current_cl["phases"][bname] += elapsed

            elif ph == "X" and current_cl is not None:
                current_cl["phases"].setdefault(name, 0)
                current_cl["phases"][name] += dur

    # Extract per-file times
    files: dict[str, dict] = {}
    for inv in cl_invocations:
        dur_us = inv.get("duration", 0)
        dur_ms = round(dur_us / 1000.0, 1)
        fname: str = inv.get("file_short", inv.get("file", ""))
        phases = inv.get("phases", {})

        # Classify sub-phases into front/back
        front = 0.0
        back = 0.0
        for pname, pdur in phases.items():
            pdur_ms = pdur / 1000.0
            pl = pname.lower()
            if any(x in pl for x in ("front", "c1", "parse")):
                front += pdur_ms
            elif any(x in pl for x in ("back", "c2", "codegen", "emit")):
                back += pdur_ms
            elif "codegen" in pl:
                back += pdur_ms
            else:
                front += pdur_ms  # default to front-end

        if not fname:
            continue

        fname_norm = fname.replace("\\", "/")

        # Skip std modules and SDK files
        if STD_FILE_PATTERNS.search(fname_norm):
            continue
        if any(fname_norm.startswith(p) for p in STD_MODULE_PREFIXES):
            continue

        files[fname_norm] = {
            "total": round(dur_ms, 1),
            "front": round(front, 1),
            "back": round(back, 1),
        }

    return {"source": "timetrace", "files": files, "totals": {}}


# ---------------------------------------------------------------------------
# Classification
# ---------------------------------------------------------------------------


def classify_file(name: str) -> str:
    """Classify a file into a category."""
    name_lower = name.lower()

    # Std modules
    if any(name_lower.startswith(p) for p in STD_MODULE_PREFIXES):
        return "std_modules"
    if STD_FILE_PATTERNS.search(name_lower):
        return "std_modules"

    # Dependency scanning
    if "dependency" in name_lower or "scan" in name_lower or "dyndep" in name_lower:
        return "dependency_scanning"

    # Module partition interfaces
    if name_lower.endswith(".cppm") or name_lower.endswith(".ixx"):
        # Exclude test module partitions
        if "test" in name_lower:
            return "test_partitions"
        return "partition_interfaces"

    # Test files
    if "test" in name_lower and name_lower.endswith(".cpp"):
        return "test_implementations"

    # Implementation files
    if name_lower.endswith(".cpp"):
        return "implementation_files"

    # Object files (accept .obj too)
    if name_lower.endswith(".obj"):
        return "linking"

    return "other"


# ---------------------------------------------------------------------------
# Report generation
# ---------------------------------------------------------------------------


def generate_report(data: dict[str, Any], label: str = "") -> dict[str, Any]:
    """Generate a complete analysis report from parsed data."""
    files = data.get("files", {})
    source = data.get("source", "unknown")

    # Classify and aggregate
    categories: dict[str, dict] = defaultdict(lambda: {"count": 0, "total_ms": 0.0})
    per_file: list[dict] = []

    for name, info in files.items():
        cat = classify_file(name)
        total = info.get("total", 0)
        front = info.get("front", 0)
        back = info.get("back", 0)

        categories[cat]["count"] += 1
        categories[cat]["total_ms"] += total

        per_file.append({
            "name": name,
            "total": total,
            "front": front,
            "back": back,
            "category": cat,
        })

    # Sort by total descending
    per_file.sort(key=lambda x: x["total"], reverse=True)

    # Compute totals
    total_all = sum(c["total_ms"] for c in categories.values())
    total_front = sum(f["front"] for f in per_file)
    total_back = sum(f["back"] for f in per_file)

    # Top 10 slowest project files (exclude std)
    project_files = [f for f in per_file if f["category"] not in ("std_modules", "dependency_scanning")]
    top_slowest = project_files[:20]

    report = {
        "label": label or "unnamed",
        "source": source,
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "total_ms": round(total_all, 1),
        "files_total": len(files),
        "front_ms": round(total_front, 1),
        "back_ms": round(total_back, 1),
        "categories": dict(categories),
        "top_slowest": [
            {
                "name": f["name"],
                "total_ms": f["total"],
                "front_ms": f["front"],
                "back_ms": f["back"],
                "pct": round(f["total"] / total_all * 100, 1) if total_all > 0 else 0,
            }
            for f in top_slowest
        ],
        "per_file": per_file,
    }

    return report


# ---------------------------------------------------------------------------
# Baseline management
# ---------------------------------------------------------------------------

BASELINE_DIR = Path(__file__).resolve().parent.parent.parent / "profile"


def save_baseline(label: str, report: dict[str, Any]) -> Path:
    """Save report as a named baseline."""
    BASELINE_DIR.mkdir(parents=True, exist_ok=True)
    path = BASELINE_DIR / "baselines.json"

    baselines: dict = {}
    if path.exists():
        with open(path, encoding="utf-8") as f:
            baselines = json.load(f)

    baselines[label] = {
        "label": label,
        "timestamp": report["timestamp"],
        "total_ms": report["total_ms"],
        "files_total": report["files_total"],
        "categories": report["categories"],
        "top_slowest": report["top_slowest"],
    }

    with open(path, "w", encoding="utf-8") as f:
        json.dump(baselines, f, indent=2)

    # Also save full run
    runs_dir = BASELINE_DIR / "runs"
    runs_dir.mkdir(parents=True, exist_ok=True)
    run_path = runs_dir / f"{label}_{datetime.now():%Y%m%d_%H%M%S}.json"
    with open(run_path, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2)

    return path


def load_baseline(label: str) -> dict | None:
    """Load a previously saved baseline by label."""
    path = BASELINE_DIR / "baselines.json"
    if not path.exists():
        return None
    with open(path, encoding="utf-8") as f:
        baselines = json.load(f)
    return baselines.get(label)


def list_baselines() -> list[str]:
    """List all saved baseline labels."""
    path = BASELINE_DIR / "baselines.json"
    if not path.exists():
        return []
    with open(path, encoding="utf-8") as f:
        return list(json.load(f).keys())


# ---------------------------------------------------------------------------
# Output formatting
# ---------------------------------------------------------------------------


def format_report(report: dict[str, Any]) -> str:
    """Format report as human-readable text."""
    lines: list[str] = []
    lines.append("=" * 72)
    lines.append(f"  Compilation Profile: {report['label']}")
    lines.append(f"  Source: {report['source']}  |  Files: {report['files_total']}")
    lines.append(f"  Timestamp: {report['timestamp']}")
    lines.append(f"  Total: {report['total_ms']:.1f} ms ({report['total_ms'] / 1000:.2f} s)")
    lines.append(f"  Front-end: {report['front_ms']:.1f} ms  |  Back-end: {report['back_ms']:.1f} ms")
    lines.append("=" * 72)
    lines.append("")

    # Category breakdown
    lines.append("── Category Breakdown ──")
    lines.append(f"  {'Category':<30} {'Files':>6} {'Total (ms)':>12} {'%':>8}")
    lines.append("  " + "-" * 56)
    for cat in sorted(report["categories"], key=lambda c: report["categories"][c]["total_ms"], reverse=True):
        info = report["categories"][cat]
        pct = info["total_ms"] / report["total_ms"] * 100 if report["total_ms"] > 0 else 0
        lines.append(f"  {cat:<30} {info['count']:>6} {info['total_ms']:>10.1f}  {pct:>6.1f}")
    lines.append("")

    # Top slowest
    lines.append("── Top 20 Slowest ──")
    lines.append(f"  {'File':<65} {'Total':>8} {'Front':>8} {'Back':>8} {'%':>6}")
    lines.append("  " + "-" * 95)
    for i, f in enumerate(report["top_slowest"][:20], 1):
        name = f["name"][:64]
        lines.append(
            f"  {i:>2}. {name:<62} {f['total_ms']:>7.1f} {f['front_ms']:>7.1f} {f['back_ms']:>7.1f} {f['pct']:>5.1f}"
        )

    return "\n".join(lines)


def format_comparison(current: dict[str, Any], baseline: dict[str, Any]) -> str:
    """Format before/after comparison."""
    lines: list[str] = []
    lines.append("=" * 72)
    lines.append("  Comparison: Current vs Baseline")
    lines.append(f"  Current:  {current['label']}  ({current['total_ms']:.0f} ms)")
    lines.append(f"  Baseline: {baseline.get('label', '(unnamed)')}  ({baseline['total_ms']:.0f} ms)")
    delta = current["total_ms"] - baseline["total_ms"]
    delta_pct = delta / baseline["total_ms"] * 100 if baseline["total_ms"] > 0 else 0
    arrow = "+" if delta > 0 else "-" if delta < 0 else "="
    lines.append(f"  Delta: {delta:+.0f} ms ({delta_pct:+.1f}%) {arrow}")
    lines.append("=" * 72)
    lines.append("")

    # Category comparison
    all_cats = set(current["categories"]) | set(baseline["categories"])
    lines.append("── Category Comparison ──")
    lines.append(f"  {'Category':<30} {'Current':>10} {'Baseline':>10} {'Delta':>10}")
    lines.append("  " + "-" * 60)
    for cat in sorted(all_cats, key=lambda c: current["categories"].get(c, {}).get("total_ms", 0), reverse=True):
        cur = current["categories"].get(cat, {}).get("total_ms", 0)
        base = baseline["categories"].get(cat, {}).get("total_ms", 0)
        d = cur - base
        lines.append(f"  {cat:<30} {cur:>9.1f} {base:>9.1f} {d:>+9.1f}")

    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------


def main():
    parser = argparse.ArgumentParser(description="Analyze compilation profiling data")
    parser.add_argument("--btplus", help="Path to /Bt+ build output text file")
    parser.add_argument("--timetrace", help="Path to vcperf timetrace JSON file")
    parser.add_argument("--label", help="Label for this run", default="")
    parser.add_argument("--save", help="Save report as baseline with this label")
    parser.add_argument("--compare", help="Compare with a saved baseline label")
    parser.add_argument("--list", action="store_true", help="List saved baselines")
    parser.add_argument("--json", action="store_true", help="Output raw JSON instead of formatted text")

    args = parser.parse_args()

    if args.list:
        baselines = list_baselines()
        if baselines:
            print("Saved baselines:")
            for b in baselines:
                print(f"  {b}")
        else:
            print("No baselines saved.")
        return

    # Parse input
    data = None
    source_name = ""
    if args.btplus:
        with open(args.btplus, encoding="utf-8") as f:
            data = parse_btplus(f.read())
        source_name = Path(args.btplus).stem
    elif args.timetrace:
        data = parse_timetrace(args.timetrace)
        source_name = Path(args.timetrace).stem
    else:
        print("Error: specify --btplus or --timetrace", file=sys.stderr)
        sys.exit(1)

    label = args.label or source_name
    report = generate_report(data, label=label)

    # Save if requested
    if args.save:
        path = save_baseline(args.save, report)
        print(f"Saved baseline '{args.save}' to {path}")

    # Compare if requested
    comparison = None
    if args.compare:
        baseline = load_baseline(args.compare)
        if baseline:
            comparison = format_comparison(report, baseline)
        else:
            print(f"No baseline found: '{args.compare}'")
            print("Available baselines:", ", ".join(list_baselines()))

    # Output
    if args.json:
        print(json.dumps(report, indent=2))
    else:
        print(format_report(report))
        if comparison:
            print()
            print(comparison)


if __name__ == "__main__":
    main()
