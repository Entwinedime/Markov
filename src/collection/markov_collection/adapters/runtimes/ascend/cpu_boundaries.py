"""Bind native CPU observations to the same entries as Ascend source extraction."""

from __future__ import annotations

from bisect import bisect_right
from collections import defaultdict
from typing import Any

from markov_collection.instrumentation.cpu_sampling import CpuAnchor, CpuBoundaryReader


class AscendCpuBoundaries(CpuBoundaryReader):
    def read(self, lanes: dict[tuple[int, int], list[dict[str, Any]]]) -> dict[tuple[int, int], list[CpuAnchor]]:
        result = {}
        queues = {}
        for lane, records in lanes.items():
            role = "control" if any(r["kind"] == "scope" for r in records) else "worker"
            roots: dict[int, list[dict[str, Any]]] = defaultdict(list)
            functions = sorted(
                (r for r in records if r["kind"] == "operator"),
                key=lambda r: (r["boundaries"][0]["wall_ns"], -r["boundaries"][1]["wall_ns"]),
            )
            for record in functions:
                begin, end = record["boundaries"]
                scope = roots[record["scope_id"]]
                if scope and end["wall_ns"] <= scope[-1]["boundaries"][1]["wall_ns"]:
                    continue
                scope.append(record)
            starts = {scope: [r["boundaries"][0]["wall_ns"] for r in values] for scope, values in roots.items()}
            transfers: dict[int, list[dict[str, Any]]] = defaultdict(list)
            for record in records:
                if record["kind"] == "transfer":
                    transfers[record["scope_id"]].append(record)
            # Queue identity links the worker to its actual host operation.
            # Position is relative to that operation, never a global native ID.
            enqueues: dict[int, list[dict[str, Any]]] = defaultdict(list)
            for record in records:
                if record["kind"] != "enqueue":
                    continue
                begin, end = record["boundaries"]
                scope = record["scope_id"]
                index = bisect_right(starts.get(scope, []), begin["wall_ns"]) - 1
                if index < 0:
                    continue
                root = roots[scope][index]
                if end["wall_ns"] <= root["boundaries"][1]["wall_ns"]:
                    enqueues[id(root)].append(record)

            anchors = []
            for record in records:
                kind = record["kind"]
                if kind in {"enqueue", "worker"}:
                    continue
                named_kind = {"operator": "function", "submission": "runtime", "query": "runtime"}.get(kind, kind)
                # A completed host driver copy identifies the blocking fallback
                # inside rtMemcpyAsync. Its outer SDK entry launches no device work;
                # the driver boundary and retained wait determine readiness.
                begin, end = record["boundaries"]
                blocking_copy = (
                    kind == "submission"
                    and record["name"] == "rtMemcpyAsync"
                    and any(
                        begin["wall_ns"] <= copy["boundaries"][0]["wall_ns"]
                        and copy["boundaries"][1]["wall_ns"] <= end["wall_ns"]
                        for copy in transfers[record["scope_id"]]
                    )
                )
                anchors.extend(
                    self._ends(
                        record,
                        role,
                        named_kind,
                        record["name"],
                        cut_begin=kind in {"scope", "submission", "query", "wait", "transfer"} and not blocking_copy,
                        cut_end=kind in {"scope", "wait", "transfer"},
                    )
                )
                # CANN reports the completed stream/device wait without the
                # SDK timeout suffix. Both names address this one observed call.
                if kind == "wait" and record["name"].endswith("WithTimeout"):
                    anchors.extend(self._ends(record, role, named_kind, record["name"].removesuffix("WithTimeout")))
            result[lane] = anchors

            for root in (r for values in roots.values() for r in values):
                queued = sorted(enqueues[id(root)], key=lambda r: r["boundaries"][0]["wall_ns"])
                for index, record in enumerate(queued):
                    anchors.extend(
                        self._ends(record, role, "enqueue", root["name"], index, len(queued), cut_begin=True)
                    )
                    queues[record["pid"], record["correlation_id"]] = root["name"], index, len(queued)

        for lane, records in lanes.items():
            for record in records:
                if record["kind"] != "worker":
                    continue
                parent = queues.get((record["pid"], record["correlation_id"]))
                if parent is None:
                    continue
                name, index, count = parent
                result[lane].extend(
                    self._ends(record, "worker", "worker", name, index, count, cut_begin=True, cut_end=True)
                )
            result[lane].sort(key=lambda a: a.wall_ns)
        return result

    @staticmethod
    def _ends(
        record: dict[str, Any],
        role: str,
        kind: str,
        name: str,
        ordinal: int = 0,
        count: int = 1,
        *,
        cut_begin: bool = False,
        cut_end: bool = False,
    ) -> list[CpuAnchor]:
        return [
            CpuAnchor(
                (role, kind, edge, name, ordinal, count),
                record["scope_id"],
                stamp["wall_ns"],
                stamp["thread_cpu_ns"],
                cut_begin if edge == "begin" else cut_end,
            )
            for edge, stamp in zip(("begin", "end"), record["boundaries"], strict=True)
        ]
