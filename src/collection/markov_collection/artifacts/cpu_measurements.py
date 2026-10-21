"""Read independent CPU activities without treating nested envelopes as costs."""

from __future__ import annotations

import json
import statistics
from collections import Counter, defaultdict
from os.path import relpath
from pathlib import Path
from typing import Any

from markov_collection.artifacts.cpu_service_samples import CpuServiceSamples
from markov_collection.artifacts.json_file import read_json, write_json
from markov_collection.artifacts.sampling_domain import SamplingDomain
from markov_collection.instrumentation.cpu_sampling import CpuBoundaryReader


class CpuMeasurements:
    def __init__(
        self,
        directory: Path,
        scopes: dict[tuple[int, int], dict[str, Any]],
        domain: SamplingDomain,
        boundaries: CpuBoundaryReader,
        requested: list[dict[str, Any]],
    ) -> None:
        self.directory = directory
        self.paths = sorted(directory.glob("*.jsonl"))
        self.scopes = scopes
        self.domain = domain
        self.boundaries = boundaries
        self.requested = requested

    def publish(self, output: Path, completions: list[dict[str, Any]]) -> dict[str, Any]:
        calibration_paths = sorted(self.directory.glob("*.clock.json"))
        calibrations = [read_json(path) for path in calibration_paths]
        read_costs = {
            (calibration["pid"], thread["tid"]): thread["read_cpu_ns"]
            for calibration in calibrations
            for thread in calibration["threads"]
        }
        activities: dict[tuple[int, int], list[dict[str, Any]]] = defaultdict(list)
        kinds: Counter[str] = Counter()
        operators: Counter[str] = Counter()
        gaps = []
        pending = set()
        unowned = {}
        for path in self.paths:
            with path.open(encoding="utf-8") as stream:
                for number, line in enumerate(stream, start=1):
                    record = json.loads(line)
                    begin, end = record["boundaries"]
                    identity = record["pid"], record["scope_id"]
                    candidate = record["kind"] == "scope" and record["name"] == "schedule_pending"
                    if candidate:
                        pending.add(identity)
                    scope = self.scopes.get(identity)
                    if scope is None:
                        unowned.setdefault(
                            identity, {"path": str(path), "line": number, "reason": "completed_cpu_scope_missing"}
                        )
                        continue
                    reason = None
                    if not scope["completed"]:
                        reason = "completed_cpu_scope_missing"
                    elif scope["metadata"] is None:
                        reason = "cpu_scope_work_not_observed"
                    elif scope["step"] == "schedule" and scope.get("input_supported") is not True:
                        reason = "cpu_schedule_input_work_not_observed"
                    elif scope.get("work_contexts") is None:
                        reason = "cpu_work_context_not_observed"
                    elif candidate and scope["step"] != "schedule":
                        reason = "cpu_pending_scope_commit_mismatch"
                    elif end["wall_ns"] < begin["wall_ns"] or end["thread_cpu_ns"] < begin["thread_cpu_ns"]:
                        reason = "cpu_activity_clock_order_invalid"

                    if reason is not None:
                        gaps.append({"path": str(path), "line": number, "reason": reason})
                        continue
                    if scope["control"]:
                        continue
                    if self.domain.stage(scope["metadata"]) != "measurement":
                        continue
                    if candidate:
                        # Only the committed host record assigns the candidate
                        # its serving role; uncommitted polls have no work.
                        record["name"] = scope["step"]

                    activities[record["pid"], record["tid"]].append(record)
                    kinds[record["kind"]] += 1
                    if record["kind"] == "operator":
                        operators[record["name"]] += 1
        gaps.extend(location for identity, location in unowned.items() if identity not in pending)

        # Exclusive segments partition each recorded lane once. CPU inside a
        # native wait is retained for diagnosis, never charged as ordinary work.
        totals: Counter[str] = Counter()
        segments = 0
        prefixes = {}
        for lane, records in activities.items():
            prefixes[lane] = defaultdict(list)
            service_cpu = 0
            service_clock_cpu = 0
            scope_cpu: Counter[int] = Counter()
            worker = not any(r["kind"] == "scope" for r in records)
            boundaries = []
            for index, record in enumerate(records):
                begin, end = record["boundaries"]
                boundaries.append((begin["wall_ns"], 1, index, begin["thread_cpu_ns"], begin["observer_cycles"]))
                boundaries.append((end["wall_ns"], 0, index, end["thread_cpu_ns"], end["observer_cycles"]))

            active = set()
            previous_cpu = None
            previous_wall = None
            previous_cycles = None
            for wall, edge, index, cpu, cycles in sorted(boundaries):
                if active and previous_cpu is not None:
                    delta = cpu - previous_cpu
                    if delta < 0:
                        gaps.append({"reason": "cpu_lane_clock_order_invalid", "wall_ns": wall})
                    else:
                        roles = {records[item]["kind"] for item in active}
                        kind = "wait" if "wait" in roles else "transfer" if "transfer" in roles else "service"
                        totals[f"{kind}_thread_cpu_ns"] += delta
                        totals[f"{kind}_wall_ns"] += wall - previous_wall
                        segments += 1
                        if kind == "service":
                            # Self-exclusion leaves one full timer read per
                            # completed observer cycle. Its cost is measured on
                            # this lane, never inferred from serving residuals.
                            clock_cpu = (cycles - previous_cycles) * read_costs[lane]
                            service_clock_cpu += clock_cpu
                            service_cpu += delta - clock_cpu
                            identities = {records[item]["scope_id"] for item in active} if worker else set()
                            if len(identities) == 1:
                                identity = next(iter(identities))
                                scope_cpu[identity] += delta - clock_cpu
                                prefixes[lane][identity].append((wall, scope_cpu[identity]))
                            elif worker and identities:
                                gaps.append({"reason": "cpu_service_scope_conflict", "wall_ns": wall})

                prefixes[lane][None].append((wall, service_cpu))

                if edge:
                    active.add(index)
                else:
                    active.remove(index)
                previous_cpu = cpu
                previous_wall = wall
                previous_cycles = cycles

            totals["service_clock_read_cpu_ns"] += service_clock_cpu
            totals["calibrated_service_thread_cpu_ns"] += service_cpu

        result = {
            "kind": "independent_cpu_activity_summary",
            "unit": "ns",
            "activities": dict(kinds),
            "operator_names": dict(sorted(operators.items())),
            "lanes": len(activities),
            "exclusive_segments": segments,
            "uncommitted_schedule_scopes": len(pending - self.scopes.keys()),
            "totals": dict(totals),
            "clock_calibrations": [relpath(path, output.parent) for path in calibration_paths],
            "gaps": gaps,
            "cost_readiness": "needs_data" if self.requested else "not_requested",
            "cost_gap": "cpu_requested_service_intervals_not_observed" if self.requested else None,
            "perturbation": self._perturbation(self.scopes, completions),
        }
        if self.requested and not gaps:
            units_path = output.with_name("cpu_service_units.json.gz")
            units = CpuServiceSamples(
                self.scopes, activities, self.boundaries.read(activities), prefixes, self.requested
            ).publish(units_path)
            result["service_units"] = units_path.name
            result["service_coverage"] = units
            result["gaps"].extend(
                {"reason": reason, "count": count} for reason, count in units["work_context_gaps"].items()
            )
            incomplete = units["missing_features"] or units["work_context_gaps"]
            result["cost_readiness"] = "needs_data" if incomplete else "sampled_service_intervals"
            result["cost_gap"] = "cpu_requested_service_intervals_not_observed" if incomplete else None
        write_json(output, result)
        return result

    def consume_journals(self) -> None:
        # Published service units retain the measured boundaries and source
        # coordinates. Calibration files remain beside these consumed journals.
        for path in self.paths:
            path.unlink()

    def _perturbation(self, scopes: dict, completions: list[dict[str, Any]]) -> dict[str, Any]:
        work: dict[str, Counter] = {"active": Counter(), "inactive": Counter()}
        for scope in scopes.values():
            metadata = scope["metadata"]
            if not scope["completed"] or metadata is None:
                continue
            control = scope["control"]
            if not control and self.domain.stage(metadata) != "measurement":
                continue

            finished = set(metadata.get("finished_request_ids", []))
            members = tuple(
                (
                    request["input_tokens"],
                    request["context_tokens"],
                    request["prompt_tokens"],
                    request["generated_tokens"],
                    request["id"] in finished,
                )
                for request in metadata["requests"]
            )
            group = "inactive" if control else "active"
            scheduling = json.dumps(scope.get("scheduling"), sort_keys=True, separators=(",", ":"))
            contexts = json.dumps(scope.get("work_contexts"), sort_keys=True, separators=(",", ":"))
            work[group][scope["rank"], scope["step"], metadata["phase"], members, scheduling, contexts] += 1

        pairs = []
        for row in completions:
            active = row["window"]
            inactive = row["inactive_recorder_window"]
            native_active = self._native_range(scopes, row.get("request_ids", []))
            native_inactive = self._native_range(scopes, row.get("inactive_request_ids", []))
            pairs.append(
                {
                    "point": row["grid"],
                    "repetition": row["repetition"],
                    "active_ns": active["ended_ns"] - active["started_ns"],
                    "inactive_ns": inactive["ended_ns"] - inactive["started_ns"],
                    "native_active_ns": native_active,
                    "native_inactive_ns": native_inactive,
                }
            )
        return {
            "control": "inactive scopes except pending receive before cohort classification; global callback installed",
            "control_scopes_recording_at_entry": sum(
                scope["control"] and scope["cpu_recording"] for scope in scopes.values()
            ),
            "native_window": "first Schedule entry to final native Publish return on all ranks",
            "actual_batch_work_matches": bool(work["active"]) and work["active"] == work["inactive"],
            "active_scopes": sum(work["active"].values()),
            "inactive_scopes": sum(work["inactive"].values()),
            "pairs": pairs,
            "median_active_to_inactive_ratio": statistics.median(
                row["active_ns"] / row["inactive_ns"] for row in pairs
            ),
            "native_window_observation": (
                "complete"
                if all(row["native_active_ns"] is not None and row["native_inactive_ns"] is not None for row in pairs)
                else "native_scope_timestamps_not_observed"
            ),
        }

    def _native_range(self, scopes: dict, request_ids: list[str]) -> int | None:
        if not request_ids:
            return None

        expected = set(request_ids)
        entries: dict[int, list[int]] = {rank: [] for rank in self.domain.ranks}
        finished: dict[int, set[str]] = {rank: set() for rank in self.domain.ranks}
        publications = []
        for scope in scopes.values():
            metadata = scope["metadata"]
            if not scope["completed"] or metadata is None:
                continue
            members = {request["id"] for request in metadata["requests"]}
            if not members or not members <= expected:
                continue
            # Native scopes are direct observations. Missing timestamps remain
            # unknown; a driver completion window cannot substitute for them.
            if scope["step"] == "schedule":
                entry = scope.get("entered_ns")
                if entry is None:
                    return None
                entries[scope["rank"]].append(entry)
            elif scope["step"] == "publish":
                returned = scope.get("native_return_ns")
                if returned is None:
                    return None
                publications.append(returned)
                finished[scope["rank"]].update(metadata["finished_request_ids"])

        if any(not values for values in entries.values()) or any(
            not expected <= values for values in finished.values()
        ):
            return None
        return max(publications) - min(min(values) for values in entries.values())
