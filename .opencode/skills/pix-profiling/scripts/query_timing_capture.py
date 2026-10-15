"""Read named PIX CPU markers through the installed PIX SQLite extension.

The original capture remains authoritative. This adapter deliberately does not
decode sampled stacks or join every event to PixCpuExecutionTimes.
"""

import argparse
from collections import Counter, defaultdict
import hashlib
import json
import os
from pathlib import Path
import platform
import sqlite3
import struct
import sys
import tempfile


def select_process(rows, pid, instance=None):
    candidates = [dict(row) for row in rows if row["pid"] == pid]
    if instance is not None:
        candidates = [row for row in candidates if row["instance"] == instance]
    if len(candidates) != 1:
        raise ValueError(
            f"Select exactly one captured process instance for OS PID {pid}; "
            f"matching instances: {candidates}"
        )
    return candidates[0]


def union_duration(intervals):
    total, previous_end = 0, None
    for start, end in sorted(intervals):
        if previous_end is None or start >= previous_end:
            total += end - start
        elif end > previous_end:
            total += end - previous_end
        previous_end = end if previous_end is None else max(previous_end, end)
    return total


def is_integer(value):
    return isinstance(value, int) and not isinstance(value, bool)


def analyse_intervals(rows, window_start, window_end):
    if not all(map(is_integer, (window_start, window_end))) or window_end <= window_start:
        raise ValueError("Capture window must contain ordered integer nanosecond bounds")

    counts = Counter(rows_read=0, invalid=0, zero=0, outside=0, clipped=0, included=0)
    invalid_reasons = Counter()
    incomplete_threads = defaultdict(Counter)
    unattributed_invalid = Counter()
    events = []
    for raw in rows:
        counts["rows_read"] += 1
        event = dict(raw)
        begin, end = event.get("begin_ns"), event.get("end_ns")
        reason = None
        if not all(map(is_integer, (begin, end))):
            reason = "missing_or_noninteger_bounds"
        elif end < begin:
            reason = "inverted_interval"
        elif end > begin and (end <= window_start or begin >= window_end):
            counts["outside"] += 1
            continue
        elif not is_integer(event.get("level")) or event["level"] < 0:
            reason = "missing_or_invalid_level"
        elif not is_integer(event.get("thread_instance")):
            reason = "missing_or_noninteger_thread_instance"
        elif not isinstance(event.get("name"), str) or not event["name"]:
            reason = "unresolved_name"

        if reason:
            counts["invalid"] += 1
            invalid_reasons[reason] += 1
            if is_integer(event.get("thread_instance")):
                incomplete_threads[event["thread_instance"]][reason] += 1
            else:
                unattributed_invalid[reason] += 1
            continue
        if end == begin:
            counts["zero"] += 1
            continue

        event["source_begin_ns"], event["source_end_ns"] = begin, end
        event["begin_ns"], event["end_ns"] = max(begin, window_start), min(end, window_end)
        event["clipped"] = (begin, end) != (event["begin_ns"], event["end_ns"])
        counts["clipped"] += int(event["clipped"])
        counts["included"] += 1
        events.append(event)

    events.sort(key=lambda item: (item["thread_instance"], item["source_begin_ns"], item["level"], -item["source_end_ns"]))
    grouped = defaultdict(list)
    for index, event in enumerate(events):
        event["index"] = index
        grouped[event["thread_instance"]].append(event)

    children = defaultdict(list)
    problems = {}
    for thread, items in grouped.items():
        active, seen = [], set()
        issues = Counter(incomplete_threads[thread])
        if unattributed_invalid:
            issues["unattributed_invalid_rows"] += sum(unattributed_invalid.values())
        root_level = min(item["level"] for item in items)
        for event in items:
            while active and active[-1]["source_end_ns"] <= event["source_begin_ns"]:
                active.pop()

            identity = (event["source_begin_ns"], event["source_end_ns"], event["level"])
            duplicate = identity in seen
            seen.add(identity)
            if duplicate:
                issues["duplicate"] += 1
            elif active and (event["level"] <= active[-1]["level"] or event["source_end_ns"] > active[-1]["source_end_ns"]):
                issues["crossing_or_overlapping_siblings"] += 1

            while active and active[-1]["level"] >= event["level"]:
                active.pop()
            expected_level = active[-1]["level"] + 1 if active else root_level
            if event["level"] != expected_level:
                issues["depth_gap_or_unmatched_parent"] += 1

            if active and event["level"] == expected_level and event["source_end_ns"] <= active[-1]["source_end_ns"]:
                children[active[-1]["index"]].append((event["begin_ns"], event["end_ns"]))
            active.append(event)

        problems[thread] = dict(issues)

    aggregates = {}
    for event in events:
        inclusive = event["end_ns"] - event["begin_ns"]
        reliable = not problems[event["thread_instance"]]
        exclusive = inclusive - union_duration(children[event["index"]]) if reliable else None
        event["exclusive_wall_ns"] = exclusive
        event["exclusive_reliability"] = "RELIABLE" if reliable else "UNRELIABLE"
        aggregate = aggregates.setdefault(event["name"], {
            "name": event["name"], "interval_count": 0, "clipped_count": 0,
            "inclusive_wall_ns": 0, "exclusive_wall_ns": 0, "exclusive_reliability": "RELIABLE",
        })
        aggregate["interval_count"] += 1
        aggregate["clipped_count"] += int(event["clipped"])
        aggregate["inclusive_wall_ns"] += inclusive
        if exclusive is None:
            aggregate["exclusive_wall_ns"] = None
            aggregate["exclusive_reliability"] = "UNRELIABLE"
        elif aggregate["exclusive_wall_ns"] is not None:
            aggregate["exclusive_wall_ns"] += exclusive

    return {
        "interval_counts": dict(counts), "invalid_reasons": dict(invalid_reasons),
        "incomplete_thread_rows": {str(key): dict(value) for key, value in incomplete_threads.items() if value},
        "unattributed_invalid_rows": dict(unattributed_invalid),
        "thread_topology_problems": {str(key): value for key, value in problems.items() if value},
        "scope_aggregates": sorted(aggregates.values(), key=lambda item: (-item["inclusive_wall_ns"], item["name"])),
    }, events


def file_identity(path):
    digest = hashlib.sha256()
    before = path.stat()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    after = path.stat()
    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        raise ValueError(f"File changed while hashing: {path}")
    return {"path": str(path), "sha256": digest.hexdigest(), "bytes": after.st_size, "mtime_ns": after.st_mtime_ns}


def validate_storage_dll(path):
    if sys.platform != "win32" or struct.calcsize("P") != 8:
        raise ValueError("PIX storage queries require a 64-bit Windows Python/SQLite runtime")
    with path.open("rb") as source:
        header = source.read(64)
        if len(header) < 64 or header[:2] != b"MZ":
            raise ValueError("Storage extension is not a PE DLL")
        source.seek(struct.unpack_from("<I", header, 60)[0])
        pe = source.read(26)
    expected = {"amd64": 0x8664, "x86_64": 0x8664, "arm64": 0xAA64, "aarch64": 0xAA64}.get(platform.machine().lower())
    if (len(pe) < 26 or pe[:4] != b"PE\0\0"
            or struct.unpack_from("<H", pe, 4)[0] != expected
            or struct.unpack_from("<H", pe, 24)[0] != 0x20B):
        raise ValueError("Storage DLL architecture must match the 64-bit Python process")


def columns(connection, table):
    # Table names are internal constants, never supplied by the command line.
    return {row[1].lower() for row in connection.execute(f'PRAGMA table_info("{table}")')}


def read_capture(capture, storage_dll, pid, instance=None):
    validate_storage_dll(storage_dll)
    before = capture.stat()
    connection = sqlite3.connect(capture.as_uri() + "?mode=ro", uri=True)
    try:
        if not hasattr(connection, "enable_load_extension"):
            raise ValueError("This SQLite runtime does not support extension loading")
        connection.enable_load_extension(True)
        try:
            connection.execute("SELECT load_extension(?, ?)", (str(storage_dll), "sqlite3_batchexpand_init"))
        finally:
            connection.enable_load_extension(False)
        connection.execute("PRAGMA query_only=ON")
        connection.row_factory = sqlite3.Row
        required = {
            "CaptureFacts": {"id", "value"},
            "Processes": {"id", "processid", "imagenameid", "starttimestamp"},
            "Threads": {"id", "procthreadid", "processrowid"},
            "Strings": {"id", "value"}, "PixEventInfo": {"id", "nameid"},
            "PixCpuExecution": {"eventid", "threadrowid", "begintimestamp", "endtimestamp", "level"},
        }
        schema = {}
        for table, names in required.items():
            schema[table] = columns(connection, table)
            if not names <= schema[table]:
                raise ValueError(f"Required PIX schema missing in {table}: {sorted(names - schema[table])}")

        process_rows = connection.execute(
            "SELECT p.Id instance,p.ProcessId pid,p.StartTimestamp start_ns,s.Value image "
            "FROM Processes p LEFT JOIN Strings s ON s.Id=p.ImageNameId WHERE p.ProcessId=?", (pid,),
        ).fetchall()
        process = select_process(process_rows, pid, instance)
        bounds = dict(connection.execute("SELECT Id,Value FROM CaptureFacts WHERE Id IN (2,24)"))
        start, stop = bounds.get(2), bounds.get(24)
        if not all(map(is_integer, (start, stop))) or stop <= start:
            raise ValueError("Validated CaptureFacts start/end nanoseconds (Id 2/24) are absent or invalid")

        thread_columns = schema["Threads"]
        thread_name = "tn.Value" if "threadnameid" in thread_columns else "NULL"
        thread_join = "LEFT JOIN Strings tn ON tn.Id=t.ThreadNameId" if "threadnameid" in thread_columns else ""
        color = "cpu.Color" if "color" in schema["PixCpuExecution"] else "NULL"
        rows = connection.execute(
            f"SELECT t.Id thread_instance,t.ProcThreadId pix_proc_thread_id,{thread_name} thread_name,n.Value name,"
            f"cpu.BeginTimestamp begin_ns,cpu.EndTimestamp end_ns,cpu.Level level,{color} color "
            "FROM Threads t JOIN PixCpuExecution cpu ON cpu.ThreadRowId=t.Id "
            "LEFT JOIN PixEventInfo pei ON pei.Id=cpu.EventId LEFT JOIN Strings n ON n.Id=pei.NameId "
            f"{thread_join} WHERE t.ProcessRowId=?", (process["instance"],),
        ).fetchall()
        availability = {table: bool(columns(connection, table)) for table in ("CpuSample", "Stacks", "StackEvents", "PixCpuExecutionTimes")}
    finally:
        connection.close()

    after = capture.stat()
    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        raise ValueError("Capture changed during read-only querying")
    capture_identity = file_identity(capture)
    if (capture_identity["bytes"], capture_identity["mtime_ns"]) != (after.st_size, after.st_mtime_ns):
        raise ValueError("Capture changed between querying and provenance hashing")
    report, events = analyse_intervals(rows, start, stop)
    report.update({
        "format_version": 1, "capture": capture_identity, "storage_extension": file_identity(storage_dll),
        "selected_process": process, "window_start_ns": start, "window_end_ns": stop,
        "window_source": "CaptureFacts Id 2/24", "source_time_unit": "nanoseconds",
        "runtime": {"python": platform.python_version(), "sqlite": sqlite3.sqlite_version,
                    "machine": platform.machine(), "pointer_bits": 64},
        "available_optional_tables": availability,
        "coverage": {
            "named_cpu_marker_intervals": True, "sampled_callstacks_exported": False,
            "gpu_events_exported": False, "cpu_execution_or_stall_queried": False,
            "thread_identity": "Threads.Id; ProcThreadId is retained as an opaque PIX identifier",
            "timestamp_sentinel_policy": "None assumed; integer negative/zero edges are accepted",
            "exclusive_reliability": "Invalid/unresolved rows, duplicates, crossings, or depth gaps invalidate thread exclusive wall time; unattributed invalid rows invalidate all threads",
        },
        "limitations": [
            "Rows require selected-process Threads linkage; unresolved names remain in invalid/incomplete accounting rather than disappearing in metadata joins.",
            "Counts describe intervals intersecting the window; clipped intervals are not complete invocations.",
            "Exclusive WALL subtracts direct-child unions relative to observed valid named-marker topology; it is not reconstructed whole-call time, CPU Execution or Stall.",
            "Inclusive and exclusive sums across nested scopes or threads are not elapsed capture budgets.",
            "Root depth is the minimum observed depth per captured thread; missing outer ancestry is not reconstructed.",
            "The public SQL article does not establish a CpuSample-to-Stacks mapping; original stack data is preserved in the capture.",
        ],
    })
    return report, events


def chrome_trace(report, events):
    pid = report["selected_process"]["pid"]
    trace = [{"name": "process_name", "ph": "M", "pid": pid, "tid": 0,
              "args": {"name": report["selected_process"].get("image") or str(pid)}}]
    first_by_thread = {}
    for event in events:
        first_by_thread.setdefault(event["thread_instance"], event)
    for thread, event in sorted(first_by_thread.items()):
        trace.append({"name": "thread_name", "ph": "M", "pid": pid, "tid": thread,
                      "args": {"name": f"{event.get('thread_name') or 'Thread'} (PIX ProcThreadId {event.get('pix_proc_thread_id')})"}})
    for event in events:
        trace.append({"name": event["name"], "cat": "PIX CPU marker", "ph": "X", "pid": pid,
                      "tid": event["thread_instance"], "ts": (event["begin_ns"] - report["window_start_ns"]) / 1000,
                      "dur": (event["end_ns"] - event["begin_ns"]) / 1000,
                      "args": {"pix_proc_thread_id": str(event["pix_proc_thread_id"]) if event.get("pix_proc_thread_id") is not None else None,
                               "level": event["level"], "color": event.get("color"),
                               "clipped": event["clipped"], "exclusive_reliability": event["exclusive_reliability"]}})
    return {"traceEvents": trace, "displayTimeUnit": "ms", "metadata": {
        "timestamp_duration_unit": "microseconds", "origin_ns": report["window_start_ns"],
        "window_end_ns": report["window_end_ns"], "selected_process": report["selected_process"],
        "capture": report["capture"], "storage_extension": report["storage_extension"],
        "coverage": report["coverage"], "limitations": report["limitations"],
    }}


def atomic_write_json(path, value):
    descriptor, temporary = tempfile.mkstemp(prefix=".pix-query-", suffix=".tmp", dir=path.parent)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as output:
            json.dump(value, output, indent=2, allow_nan=False)
            output.write("\n")
            output.flush()
            os.fsync(output.fileno())
        # Same-directory hard-link publication is atomic and fails if the destination exists.
        os.link(temporary, path)
    finally:
        Path(temporary).unlink(missing_ok=True)


def write_outputs(directory, report, events, export_trace=False):
    directory.mkdir(parents=True, exist_ok=False)
    if export_trace:
        atomic_write_json(directory / "markers.trace.json", chrome_trace(report, events))
    atomic_write_json(directory / "analysis.json", report)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", type=Path, required=True)
    parser.add_argument("--storage-dll", type=Path, required=True, help="Explicit trusted installed PIX storage extension DLL")
    parser.add_argument("--pid", type=int, required=True, help="Target OS PID, not Processes.Id")
    parser.add_argument("--process-instance", type=int, help="Captured Processes.Id; required if the PID is ambiguous")
    parser.add_argument("--output-dir", type=Path, required=True, help="New per-capture directory; existing directories are rejected")
    parser.add_argument("--chrome-trace", action="store_true", help="Also export positive named-marker intervals as Chrome JSON")
    args = parser.parse_args(argv)
    try:
        if args.output_dir.resolve().exists():
            raise FileExistsError(f"Output directory already exists: {args.output_dir.resolve()}")
        report, events = read_capture(args.capture.resolve(strict=True), args.storage_dll.resolve(strict=True), args.pid, args.process_instance)
        write_outputs(args.output_dir.resolve(), report, events, args.chrome_trace)
    except (OSError, ValueError, sqlite3.Error) as error:
        parser.exit(1, f"PIX timing query failed: {error}\n")
    print(args.output_dir.resolve() / "analysis.json")


if __name__ == "__main__":
    main()
