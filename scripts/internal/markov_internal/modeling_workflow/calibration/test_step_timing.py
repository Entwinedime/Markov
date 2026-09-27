"""Explicit request and rank coverage for base CPU measurements."""

import copy
import unittest

from .step_timing import bind_forward_steps, compare_forward_cpu, observe_forward_cpu
from .cpu_scopes import synchronization_scopes


class StepBindingTests(unittest.TestCase):
    def inputs(self):
        report = {
            "requests": [
                {
                    "logical_request_id": "a",
                    "measure": True,
                    "kind": "request",
                    "start_time_ms": 1.0,
                    "end_time_ms": 2.0,
                }
            ],
            "formal_window": {"formal_begin_ms": 1.0, "formal_end_ms": 3.0},
        }
        events = [
            dict(
                name="step[EXTEND bs=1]",
                identity={"request_ids": ["a"], "tp_rank": rank},
                start_ns=1200000,
                end_ns=2100000,
                thread_cpu_ns=50000,
                returned=True,
                pid=10 + rank,
                tid=10 + rank,
            )
            for rank in (0, 1)
        ]
        return report, events

    def test_tail_preserved_and_ranks_not_inferred(self):
        report, events = self.inputs()
        rows = bind_forward_steps(report, events, (0, 1))
        self.assertEqual(rows[0]["end_ns"], events[0]["end_ns"])
        self.assertEqual(rows[0]["thread_cpu_ns"], events[0]["thread_cpu_ns"])
        with self.assertRaises(ValueError):
            bind_forward_steps(report, events[:1], (0, 1))

    def test_repeated_phase_ordinals_are_per_request_rank_and_time(self):
        report, events = self.inputs()
        later = copy.deepcopy(events)
        for row in later:
            row.update(start_ns=2200000, end_ns=2500000)
        rows = bind_forward_steps(report, list(reversed(events + later)), (0, 1))
        for rank in (0, 1):
            self.assertEqual([r["forward_ordinal"] for r in rows if r["identity"]["tp_rank"] == rank], [0, 1])

    def test_no_time_guess_or_partial_identity(self):
        for case in ("missing", "batch", "unknown", "rank", "returned"):
            report, events = self.inputs()
            if case == "missing":
                events[0].pop("identity")
            elif case == "batch":
                events[0]["identity"]["request_ids"] = ["a", "b"]
            elif case == "unknown":
                events[0]["identity"]["request_ids"] = ["other"]
            elif case == "rank":
                events[0]["identity"]["tp_rank"] = 3
            else:
                events[0]["returned"] = False
            with self.subTest(case=case), self.assertRaises(ValueError):
                bind_forward_steps(report, events, (0, 1))

    def test_request_coverage_and_serial_scope(self):
        report, events = self.inputs()
        request = copy.deepcopy(report["requests"][0])
        request.update(logical_request_id="b", start_time_ms=2.2, end_time_ms=2.8)
        report["requests"].append(request)
        with self.assertRaises(ValueError):
            bind_forward_steps(report, events, (0, 1))
        request["start_time_ms"] = 1.5
        with self.assertRaises(ValueError):
            bind_forward_steps(report, events, (0, 1))


def measurement(requests=1, ranks=1):
    return {
        "rows": [
            dict(
                request_id=f"r{request}",
                rank=rank,
                phase=phase,
                sync_count=2,
                outside_sync_cpu_us=10.0,
            )
            for request in range(requests)
            for rank in range(ranks)
            for phase in ("prefill", "decode")
        ]
    }


class ForwardCpuComparisonTests(unittest.TestCase):
    def test_arbitrary_identity_sets_and_signed_deltas(self):
        for requests, ranks in ((1, 1), (3, 4), (14, 2)):
            with self.subTest(requests=requests, ranks=ranks):
                light = measurement(requests, ranks)
                full = copy.deepcopy(light)
                full["rows"][0]["outside_sync_cpu_us"] = 8.0
                full["rows"].reverse()
                result = compare_forward_cpu(light, full)
                self.assertEqual(len(result["rows"]), requests * ranks * 2)
                self.assertEqual(result["rows"][0]["delta"]["outside_sync_cpu_us"], -2.0)

    def test_missing_and_changed_call_structure(self):
        light = measurement()
        for mutation in ("missing", "sync"):
            full = copy.deepcopy(light)
            if mutation == "missing":
                full["rows"].pop()
            else:
                full["rows"][0]["sync_count"] += 1
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                compare_forward_cpu(light, full)

    def test_repeated_decode_retains_distinct_budgets(self):
        light = measurement()
        light["rows"].append(dict(light["rows"][-1], forward_ordinal=1))
        full = copy.deepcopy(light)
        full["rows"][-1]["outside_sync_cpu_us"] += 3
        full["rows"].reverse()
        rows = compare_forward_cpu(light, full)["rows"]
        self.assertEqual(
            [(r["forward_ordinal"], r["delta"]["outside_sync_cpu_us"]) for r in rows if r["phase"] == "decode"],
            [(0, 0), (1, 3)],
        )


class ForwardCpuObservationTests(unittest.TestCase):
    def inputs(self):
        step = dict(
            request_id="r1",
            identity={"tp_rank": 3},
            name="decode forward",
            pid=10,
            tid=12,
            start_ns=100000,
            end_ns=200000,
            thread_cpu_ns=60000,
        )
        event = dict(
            ph="X",
            name="AscendCL@aclrtSynchronizeStream",
            pid="10",
            tid="12",
            ts=120.0,
            dur=30.0,
            args={"thread_cpu_ns": 20000},
        )
        return {"rows": [step]}, [event]

    def test_explicit_rank_and_same_thread_cpu(self):
        document, events = self.inputs()
        result = observe_forward_cpu(document, synchronization_scopes(events))
        row = result["rows"][0]
        self.assertEqual(row["rank"], 3)
        self.assertEqual(row["outside_sync_cpu_us"], 40.0)
        self.assertEqual(row["sync_count"], 1)

    def test_missing_wrong_thread_boundary_overlap_and_cpu(self):
        for case in ("missing", "thread", "boundary", "overlap", "cpu", "excess"):
            document, events = self.inputs()
            if case == "missing":
                events.clear()
            elif case == "thread":
                events[0]["tid"] = 99
            elif case == "boundary":
                events[0]["ts"] = 190.0
            elif case == "overlap":
                events.append(copy.deepcopy(events[0]))
            elif case == "cpu":
                events[0]["args"].clear()
            elif case == "excess":
                document["rows"][0]["thread_cpu_ns"] = 10000
            with self.subTest(case=case), self.assertRaises(ValueError):
                observe_forward_cpu(document, synchronization_scopes(events))
