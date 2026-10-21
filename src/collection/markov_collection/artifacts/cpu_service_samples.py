"""Measure requested named CPU intervals from independently recorded work."""

from __future__ import annotations

import gzip
import json
from bisect import bisect_left, bisect_right
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any

from markov_collection.instrumentation.cpu_sampling import CpuAnchor


class CpuServiceSamples:
    def __init__(
        self,
        scopes: dict,
        lanes: dict,
        anchors: dict,
        prefixes: dict,
        requested: list[dict[str, Any]],
    ) -> None:
        self.scopes = scopes
        self.lanes = lanes
        self.anchors = anchors
        self.prefixes = prefixes
        self.context_gaps = Counter()
        self.unresolved_scopes = set()
        self.contexts = self._work_ranges()
        self.requested = defaultdict(dict)
        self.expected = set()
        # Target queries already carry the complete named interval. Sampling
        # needs no separate source-template inventory to identify its work.
        for features in requested:
            service = features["service"]
            pair = CpuAnchor.boundary_key(service["begin"]), CpuAnchor.boundary_key(service["end"])
            self.requested[self._scope_key(features)][pair] = service
            self.expected.add(json.dumps(features, sort_keys=True, separators=(",", ":")))

    @staticmethod
    def _scope_key(features: dict[str, Any]) -> tuple:
        return features["step"], features["phase"], features["rank"]

    def _work_ranges(self) -> dict:
        result = defaultdict(list)
        observed = set()
        scopes = set()
        for lane, records in self.lanes.items():
            for record in records:
                identity = lane[0], record["scope_id"]
                scopes.add(identity)
                if record["kind"] != "function" or not record["correlation_id"]:
                    continue
                index = record["correlation_id"] - 1
                work = self.scopes[identity]["work_contexts"]
                context = (*identity, index)
                if index >= len(work) or context in observed:
                    self.context_gaps["cpu_work_context_not_unique"] += 1
                    self.unresolved_scopes.add(identity)
                    continue
                observed.add(context)
                if work[index] is None:
                    self.context_gaps["cpu_work_context_not_observed"] += 1
                    self.unresolved_scopes.add(identity)
                    continue
                begin, end = (boundary["wall_ns"] for boundary in record["boundaries"])
                result[lane].append((begin, end, work[index]))
        for identity in scopes:
            for index in range(len(self.scopes[identity]["work_contexts"])):
                if (*identity, index) not in observed:
                    self.context_gaps["cpu_work_context_boundary_missing"] += 1
                    self.unresolved_scopes.add(identity)

        # Queue identity carries the actual host operation to its worker. The
        # worker need not run inside the host function's clock interval.
        queued = {}
        for lane, records in self.lanes.items():
            for record in records:
                if record["kind"] != "enqueue":
                    continue
                begin, end = (boundary["wall_ns"] for boundary in record["boundaries"])
                matches = [(first, final, work) for first, final, work in result[lane] if first < end and begin < final]
                if not matches:
                    continue
                if len(matches) == 1 and matches[0][0] <= begin and end <= matches[0][1]:
                    queued[lane[0], record["correlation_id"]] = matches[0][2]
                else:
                    self.context_gaps["cpu_queue_work_context_not_unique"] += 1
                    self.unresolved_scopes.add((lane[0], record["scope_id"]))
        for lane, records in self.lanes.items():
            for record in records:
                if record["kind"] == "worker" and (work := queued.get((lane[0], record["correlation_id"]))) is not None:
                    begin, end = (boundary["wall_ns"] for boundary in record["boundaries"])
                    result[lane].append((begin, end, work))
        return result

    def _work_context(self, lane: tuple[int, int], begin: int, end: int) -> tuple[bool, dict | None]:
        overlapping = [
            (first, final, work) for first, final, work in self.contexts[lane] if first < end and begin < final
        ]
        if not overlapping:
            return True, None
        if len(overlapping) == 1:
            first, final, work = overlapping[0]
            if first <= begin and end <= final:
                return True, work
        self.context_gaps["cpu_service_crosses_work_context"] += 1
        return False, None

    @staticmethod
    def _features(scope: dict[str, Any]) -> dict[str, Any]:
        metadata = scope["metadata"]
        requests = metadata["requests"]
        return {
            "step": scope["step"],
            "phase": metadata["phase"],
            "rank": scope["rank"],
            "requests": len(requests),
            "input_tokens": sum(r["input_tokens"] for r in requests),
            "context_tokens": sum(r["context_tokens"] for r in requests),
            "finished_requests": len(metadata["finished_request_ids"]) if scope["step"] == "publish" else 0,
            "scheduling": scope["scheduling"] if scope["step"] == "schedule" else None,
            "publication": metadata["publication"] if scope["step"] == "publish" else None,
        }

    def _owners(self, lane: tuple[int, int], records: list[dict[str, Any]]) -> list[tuple[dict, int, int, int | None]]:
        frames = sorted((r for r in records if r["kind"] == "scope"), key=lambda r: r["boundaries"][0]["wall_ns"])
        if not frames:
            by_scope = defaultdict(list)
            for record in records:
                by_scope[record["scope_id"]].append(record)
            return [
                (
                    self.scopes[lane[0], identity],
                    min(r["boundaries"][0]["wall_ns"] for r in values),
                    max(r["boundaries"][1]["wall_ns"] for r in values),
                    identity,
                )
                for identity, values in by_scope.items()
            ]

        owners = []
        boundaries = {frame["scope_id"]: frame["boundaries"] for frame in frames}
        for frame in frames:
            identity = frame["scope_id"]
            scope = self.scopes[lane[0], identity]
            # The framework supplies ownership; native clocks supply endpoints.
            # Common measurement does not infer meaning from nested step names.
            begin, end = (boundaries[owner][edge]["wall_ns"] for owner, edge in scope["control_interval"])
            owners.append((scope, begin, end, None))
        return owners

    @staticmethod
    def _at(prefix: list[tuple[int, int]], wall_ns: int) -> int:
        index = bisect_right(prefix, (wall_ns, float("inf"))) - 1
        return prefix[index][1] if index >= 0 else 0

    def publish(self, output: Path) -> dict[str, Any]:
        observed = {}
        samples = 0
        below_resolution = {}
        with gzip.open(output, "wt", encoding="utf-8", compresslevel=1) as stream:
            stream.write('{"kind":"cpu_service_samples","unit":"ns","samples":[')
            for lane, records in self.lanes.items():
                anchors = self.anchors[lane]
                times = [a.wall_ns for a in anchors]
                for scope, begin_ns, end_ns, identity in self._owners(lane, records):
                    if (lane[0], scope["scope_id"]) in self.unresolved_scopes:
                        continue
                    work = self._features(scope)
                    required = self.requested.get(self._scope_key(work))
                    if not required:
                        continue
                    grouped = defaultdict(list)
                    cuts = []
                    for anchor in anchors[bisect_left(times, begin_ns) : bisect_right(times, end_ns)]:
                        if identity is None or anchor.scope_id == identity:
                            grouped[anchor.key].append(anchor)
                            if anchor.causal_cut:
                                cuts.append(anchor.wall_ns)
                    cuts.extend(
                        point
                        for first, final, _ in self.contexts[lane]
                        for point in (first, final)
                        if begin_ns <= point <= end_ns
                    )
                    cuts.sort()

                    prefix = self.prefixes[lane][identity]
                    for (first, final), service in required.items():
                        # The request recipe only triggers execution. Feature
                        # values always come from its actual observed batch.
                        ends = grouped[final]
                        end_times = [a.wall_ns for a in ends]
                        for begin in grouped[first]:
                            position = bisect_right(end_times, begin.wall_ns)
                            if position == len(ends):
                                continue
                            end = ends[position]
                            # A later same-name call cannot close this unit
                            # across another operation, submission or wait.
                            if bisect_right(cuts, begin.wall_ns) != bisect_left(cuts, end.wall_ns):
                                continue
                            known, context = self._work_context(lane, begin.wall_ns, end.wall_ns)
                            if not known:
                                continue
                            features = {
                                "service": service,
                                **work,
                                "collective": context.get("collective") if context else None,
                            }
                            if context and "incoming_requests" in context:
                                features["scheduling"] = {
                                    **work["scheduling"],
                                    "incoming_requests": context["incoming_requests"],
                                }
                            feature_key = json.dumps(features, sort_keys=True, separators=(",", ":"))
                            duration = self._at(prefix, end.wall_ns) - self._at(prefix, begin.wall_ns)
                            if duration < 0:
                                # Reject this observation, not other valid
                                # samples of the same requested feature.
                                excluded = below_resolution.setdefault(
                                    feature_key, {"features": features, "samples": 0}
                                )
                                excluded["samples"] += 1
                                continue

                            # Repeated layers and repetitions refer to one work
                            # definition. Clocks and source coordinates stay local.
                            feature_index = observed.setdefault(feature_key, (len(observed), features))[0]
                            if samples:
                                stream.write(",")
                            payload = json.dumps(
                                {
                                    "feature": feature_index,
                                    "duration_ns": duration,
                                    "observed_wall_ns": end.wall_ns - begin.wall_ns,
                                    "boundaries": [
                                        {"wall_ns": a.wall_ns, "thread_cpu_ns": a.thread_cpu_ns} for a in (begin, end)
                                    ],
                                    "source": {"pid": lane[0], "tid": lane[1], "scope_id": scope["scope_id"]},
                                },
                                separators=(",", ":"),
                            )
                            stream.write(payload)
                            samples += 1
            stream.write('],"features":')
            features = [item[1] for item in observed.values()]
            json.dump(features, stream, separators=(",", ":"))
            stream.write("}")

        return {
            "samples": samples,
            "requested_features": len(self.expected),
            "observed_feature_count": len(features),
            "missing_features": len(self.expected - observed.keys()),
            "below_clock_resolution_intervals": sum(item["samples"] for item in below_resolution.values()),
            "below_clock_resolution_features": list(below_resolution.values()),
            "work_context_gaps": dict(self.context_gaps),
        }
