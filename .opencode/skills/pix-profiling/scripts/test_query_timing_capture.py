"""Pure stdlib tests; no PIX DLL, capture, UI, or external package is needed."""

from pathlib import Path
import tempfile
import unittest

import query_timing_capture as query


def event(name, begin, end, level=0, thread=1, pix_proc_thread_id=3899830305680):
    return {"name": name, "begin_ns": begin, "end_ns": end, "level": level,
            "thread_instance": thread, "pix_proc_thread_id": pix_proc_thread_id,
            "thread_name": "worker", "color": 0}


class IntervalTests(unittest.TestCase):
    def test_nested_recursion_subtracts_only_direct_children(self):
        report, events = query.analyse_intervals([
            event("recursive", 0, 100), event("recursive", 10, 80, 1),
            event("leaf", 20, 30, 2), event("other", 80, 90, 1),
        ], 0, 100)
        by_name = {item["name"]: item for item in report["scope_aggregates"]}
        self.assertEqual([item["exclusive_wall_ns"] for item in events], [20, 60, 10, 10])
        self.assertEqual(by_name["recursive"]["inclusive_wall_ns"], 170)
        self.assertEqual(by_name["recursive"]["exclusive_wall_ns"], 80)

    def test_direct_child_union_does_not_double_count_descendants(self):
        self.assertEqual(query.union_duration([(0, 8), (2, 4), (6, 10), (10, 12)]), 12)

    def test_captured_thread_instances_isolate_reused_pix_identifier(self):
        report, events = query.analyse_intervals([
            event("first", 0, 100, thread=1), event("child", 10, 20, 1, thread=1),
            event("second", 0, 100, thread=2),
        ], 0, 100)
        self.assertFalse(report["thread_topology_problems"])
        self.assertEqual({item["name"]: item["exclusive_wall_ns"] for item in events},
                         {"first": 90, "child": 10, "second": 100})

    def test_crossings_duplicates_and_depth_gaps_invalidate_exclusive(self):
        cases = [
            [event("parent", 0, 50), event("cross", 20, 70, 1)],
            [event("same", 0, 50), event("same", 0, 50)],
            [event("parent", 0, 100), event("gap", 20, 30, 2)],
            [event("root", 0, 10), event("unmatched", 20, 30, 1)],
        ]
        for rows in cases:
            with self.subTest(rows=rows):
                report, events = query.analyse_intervals(rows, 0, 100)
                self.assertTrue(report["thread_topology_problems"])
                self.assertTrue(all(item["exclusive_wall_ns"] is None for item in events))
                self.assertTrue(all(item["exclusive_reliability"] == "UNRELIABLE"
                                    for item in report["scope_aggregates"]))

        # Window clipping must not conceal a crossing in the original source intervals.
        _, clipped = query.analyse_intervals([event("parent", 0, 60), event("cross", 20, 80, 1)], 0, 50)
        self.assertTrue(all(item["exclusive_wall_ns"] is None for item in clipped))

    def test_negative_and_zero_edges_clip_without_becoming_invalid(self):
        report, events = query.analyse_intervals([
            event("left", -20, 20), event("at_zero", 0, 10, 1),
            event("right", 80, 120), event("negative_zero", -1, -1),
            event("zero", 0, 0), event("outside", -20, -10),
            event("inverted", 40, 30, thread=2), event("missing", None, 30, thread=2),
        ], 0, 100)
        self.assertEqual(report["interval_counts"], {
            "rows_read": 8, "invalid": 2, "zero": 2, "outside": 1, "clipped": 2, "included": 3,
        })
        self.assertEqual([(item["begin_ns"], item["end_ns"]) for item in events], [(0, 20), (0, 10), (80, 100)])
        self.assertEqual([item["exclusive_wall_ns"] for item in events], [10, 10, 20])

    def test_invalid_or_unresolved_children_invalidate_only_their_thread(self):
        children = [event("child", 20, None, 1), event(None, 20, 80, 1),
                    event("", 20, 80, 1), event(123, 20, 80, 1)]
        for child in children:
            with self.subTest(child=child):
                report, events = query.analyse_intervals([
                    event("parent", 0, 100), child, event("unaffected", 0, 100, thread=2),
                ], 0, 100)
                by_name = {item["name"]: item for item in report["scope_aggregates"]}
                self.assertEqual(report["interval_counts"]["invalid"], 1)
                self.assertIn("1", report["incomplete_thread_rows"])
                self.assertEqual(by_name["parent"]["inclusive_wall_ns"], 100)
                self.assertIsNone(by_name["parent"]["exclusive_wall_ns"])
                self.assertEqual(by_name["parent"]["exclusive_reliability"], "UNRELIABLE")
                self.assertEqual(by_name["unaffected"]["exclusive_wall_ns"], 100)
                self.assertEqual([item["exclusive_wall_ns"] for item in events], [None, 100])

    def test_unknown_thread_incomplete_row_invalidates_all_threads(self):
        report, events = query.analyse_intervals([
            event("first", 0, 100), event("second", 0, 100, thread=2),
            event("unknown", 20, None, thread=None),
        ], 0, 100)
        self.assertTrue(report["unattributed_invalid_rows"])
        self.assertTrue(all(item["exclusive_wall_ns"] is None for item in events))
        self.assertTrue(all(item["exclusive_wall_ns"] is None for item in report["scope_aggregates"]))

    def test_provably_outside_unresolved_row_does_not_invalidate_window(self):
        report, events = query.analyse_intervals([
            event("parent", 0, 100), event(None, -20, -10, thread=None),
        ], 0, 100)
        self.assertEqual(report["interval_counts"]["outside"], 1)
        self.assertFalse(report["unattributed_invalid_rows"])
        self.assertEqual(events[0]["exclusive_wall_ns"], 100)

    def test_chrome_units_are_microseconds_and_only_positive_intervals_export(self):
        report, events = query.analyse_intervals([event("work", 1000, 3500), event("zero", 1000, 1000)], 1000, 4000)
        report.update({"selected_process": {"pid": 123, "image": "target.exe"},
                       "window_start_ns": 1000, "window_end_ns": 4000,
                       "capture": {"path": "fixture.wpix"}, "storage_extension": {"path": "PixStorage.dll"},
                       "coverage": {}, "limitations": []})
        trace = query.chrome_trace(report, events)
        markers = [item for item in trace["traceEvents"] if item["ph"] == "X"]
        self.assertEqual(len(markers), 1)
        self.assertEqual((markers[0]["ts"], markers[0]["dur"]), (0.0, 2.5))
        self.assertEqual(markers[0]["tid"], 1)
        self.assertEqual(markers[0]["args"]["pix_proc_thread_id"], "3899830305680")
        thread_labels = [item["args"]["name"] for item in trace["traceEvents"] if item["name"] == "thread_name"]
        self.assertEqual(thread_labels, ["worker (PIX ProcThreadId 3899830305680)"])
        self.assertEqual(trace["metadata"]["timestamp_duration_unit"], "microseconds")


class SelectionAndOutputTests(unittest.TestCase):
    def test_pid_reuse_requires_explicit_captured_instance(self):
        rows = [{"pid": 12, "instance": 1}, {"pid": 12, "instance": 2}, {"pid": 99, "instance": 3}]
        with self.assertRaises(ValueError):
            query.select_process(rows, 12)
        self.assertEqual(query.select_process(rows, 12, 2)["instance"], 2)
        self.assertEqual(query.select_process(rows, 99)["instance"], 3)
        with self.assertRaises(ValueError):
            query.select_process(rows, 12, 3)

    def test_atomic_publication_never_clobbers_an_existing_file(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "analysis.json"
            query.atomic_write_json(path, {"first": 1})
            before = path.read_bytes()
            with self.assertRaises(FileExistsError):
                query.atomic_write_json(path, {"second": 2})
            self.assertEqual(path.read_bytes(), before)
            self.assertEqual(list(path.parent.glob(".pix-query-*.tmp")), [])

    def test_existing_output_directory_is_rejected_before_writes(self):
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaises(FileExistsError):
                query.write_outputs(Path(temporary), {}, [])
            self.assertEqual(list(Path(temporary).iterdir()), [])


if __name__ == "__main__":
    unittest.main()
