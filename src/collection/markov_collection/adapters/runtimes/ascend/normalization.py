"""Normalize explicitly declared Ascend observations; retain gaps instead of inventing dependencies."""

from __future__ import annotations

import argparse
import csv
import json
import re
import sqlite3
from bisect import bisect_left, bisect_right
from collections import Counter, defaultdict
from contextlib import closing
from decimal import Decimal
from pathlib import Path
from typing import Any

from markov_collection.adapters.runtimes.ascend.event_history import EventHistory
from markov_collection.adapters.runtimes.ascend.torch_npu.allocator import WorkspaceAllocation
from markov_collection.artifacts.json_file import write_json


def timestamp_ns(value: str | int | float) -> int:
    # Vendor timestamps are decimal microseconds near the Unix epoch. Converting
    # them through a binary float would lose sub-microsecond identity matches.
    return int(Decimal(str(value).strip()) * 1000)


class ObservationNormalizer:
    def __init__(self, capture: Path) -> None:
        self.directory = capture.parent
        with capture.open(encoding="utf-8") as stream:
            self.capture = json.load(stream)

        self.calls: dict[str, dict[str, Any]] = {}
        self.intervals: list[dict[str, Any]] = []
        self.flows: list[dict[str, Any]] = []
        self.native_api: list[dict[str, Any]] = []
        self.supplementary_work: list[dict[str, Any]] = []
        self.gaps: list[dict[str, Any]] = []
        self.communicators: dict[tuple[int, str], dict[str, Any]] = {}
        self.trace_clock_offsets: dict[int, int] = {}
        self.devices: list[dict[str, Any]] = []
        self.profile_controls: list[dict[str, Any]] = []

    def _task_samples(
        self, collectives: dict[tuple[int, int], list[dict[str, Any]]], unresolved: Counter
    ) -> list[dict[str, Any]]:
        hardware: dict[tuple[int, int, int, int], list[dict[str, Any]]] = defaultdict(list)
        for interval in self.intervals:
            if interval["process"] != "Ascend Hardware":
                continue

            args = interval["arguments"]
            key = (interval["rank"], args["Physic Stream Id"], args["Task Id"], args["Subtask Id"])
            hardware[key].append(interval)

        samples = []
        for detail in self.intervals:
            args = detail["arguments"]
            if detail["process"] != "Communication" or "context id" not in args:
                continue

            rank = detail["rank"]
            key = (rank, args["stream id"], args["task id"], args["context id"])
            matches = hardware[key]
            if len(matches) != 1 or args["src rank"] != rank:
                unresolved["collective_physical_task_identity_ambiguous"] += 1
                continue

            physical = matches[0]
            owners = collectives.get((rank, physical["arguments"]["connection_id"]), [])
            if not owners:
                owner = physical["owner_call"]
                if owner is None or self.calls[owner]["step"] != "setup":
                    unresolved["collective_task_owner_missing"] += 1
                continue  # Only a known setup scope can be excluded without a gap.
            if len(owners) != 1:
                unresolved["collective_task_owner_ambiguous"] += 1
                continue

            collective = owners[0]

            # Keep the physical task boundary and classification. Waits and
            # markers retain their physical classification, not a collective envelope cost.
            samples.append(
                {
                    "rank": rank,
                    "stage": collective["stage"],
                    **{key: collective[key] for key in ("grid", "repetition", "step") if key in collective},
                    "kind": physical["kind"],
                    "operation": detail["name"],
                    "bytes": args["size(Byte)"],
                    "data_type": args["data type"],
                    "transport": args["transport type"],
                    "source_rank": args["src rank"],
                    "destination_rank": args["dst rank"],
                    "stream": args["stream id"],
                    "task": args["task id"],
                    "context": args["context id"],
                    "device_start_ns": physical["start_ns"],
                    "device_end_ns": physical["end_ns"],
                    "observed_wall_ns": physical["observed_wall_ns"],
                    "source": physical["source"],
                    "detail_source": detail["source"],
                }
            )
        return samples

    def _read_capture(self) -> None:
        for reference in self.capture["observations"]:
            with (self.directory / reference).open(encoding="utf-8") as stream:
                for line in stream:
                    call = json.loads(line)
                    identity = f"{call['rank']}:{call['call_id']}"
                    self.calls[identity] = call
                    if call["status"] != "completed":
                        self.gaps.append({"kind": "observation", "reason": "native_step_failed", "call": identity})

        self._read_communicators()
        for trace in self.capture["traces"]:
            self._read_trace(trace)

        self._read_native_api()
        self._read_profile_controls()

        # Propagate the innermost observed scope through queues and task identities
        # before projecting onto serving ownership. Regions remain views of the
        # same work; collapsing them earlier loses model-internal attribution.
        for records in (self.intervals, self.native_api):
            for record in records:
                scope = record["owner_call"]
                record["scope_call"] = scope
                if scope is not None:
                    record["owner_call"] = self._semantic_owner(scope)

        self.unowned = Counter(
            interval["kind"]
            for interval in self.intervals
            if interval["owner_call"] is None and "profiler_control" not in interval
        )
        if self.unowned:
            self.gaps.append(
                {"kind": "observation", "reason": "unassigned_native_intervals", "counts": dict(self.unowned)}
            )

    def _read_profile_controls(self) -> None:
        for reference in self.capture["profile_controls"]:
            with (self.directory / reference).open(encoding="utf-8") as stream:
                for index, record in enumerate(json.load(stream)):
                    record["source"] = {"path": reference, "index": index}
                    self.profile_controls.append(record)

        # Attribution requires the observed control call on the same native
        # thread and full containment, never just an unowned tail interval.
        controls: dict[tuple[int, int], list[dict[str, Any]]] = defaultdict(list)
        for record in self.profile_controls:
            if not record["completed"]:
                self.gaps.append(
                    {"kind": "observation", "reason": "profiler_control_failed", "source": record["source"]}
                )
                continue
            controls[(record["rank"], record["tid"])].append(record)

        for records in (self.intervals, self.native_api):
            for record in records:
                is_native = "api" in record
                if not is_native and record["process"] != "CANN":
                    continue
                offset = 0 if is_native else self.trace_clock_offsets[record["rank"]]
                for control in controls[(record["rank"], record["tid"])]:
                    if not (
                        control["start_ns"] + offset <= record["start_ns"]
                        and record["end_ns"] <= control["end_ns"] + offset
                    ):
                        continue
                    if record["owner_call"] is not None:
                        self.gaps.append(
                            {
                                "kind": "observation",
                                "reason": "profiler_control_overlaps_serving",
                                "source": control["source"],
                            }
                        )
                        break
                    record["profiler_control"] = {"operation": control["operation"], "source": control["source"]}
                    break

    def _read_communicators(self) -> None:
        workers = {(call["rank"], call["pid"]) for call in self.calls.values()}
        records: dict[tuple[int, str], dict[str, list[dict[str, Any]]]] = defaultdict(lambda: defaultdict(list))
        worker_facts: dict[int, dict[str, list[dict[str, Any]]]] = defaultdict(lambda: defaultdict(list))
        transports: dict[tuple[int, str], list[dict[str, Any]]] = defaultdict(list)
        patterns = {
            "options": r"comm config of \[([^]]+)\]: bufferSize\[(\d+)\], deterministic\[(\d+)\]",
            "expansion": r"comm config info of \[([^]]+)\]: .*opExpansionMode\[(\d+)\]",
            "topology": (
                r"\[Communicator Key Info\]identifier\[([^]]+)\] rankSize\[(\d+)\] "
                r"serverNum\[(\d+)\] moduleNum\[(\d+)\]"
            ),
        }
        for reference in self.capture["communication_logs"]:
            worker = (reference["rank"], reference["pid"])
            if worker not in workers:
                raise ValueError(f"communication log has no observed worker: {worker}")

            with (self.directory / reference["path"]).open(encoding="utf-8") as stream:
                for line_number, line in enumerate(stream, start=1):
                    if not any(f"{component}({reference['pid']}," in line for component in ("HCCL", "HCCP")):
                        continue
                    source = {"path": reference["path"], "line": line_number}
                    for kind, pattern in (
                        ("device_type", r"chip name is ([^,]+),"),
                        ("hcomm_version", r"hcomm version is ([^,]+),"),
                        ("env_aicpu_unfold", r"\[HCCL_ENV\].*aicpuUnfold is \[(\d+)\]"),
                        ("env_aiv_mode", r"\[HCCL_ENV\].*aivMode is \[(\d+)\]"),
                    ):
                        match = re.search(pattern, line)
                        if match:
                            value = int(match[1]) if kind.startswith("env_") else match[1]
                            worker_facts[reference["rank"]][kind].append({"value": value, "source": source})
                    transport = re.search(
                        r"communicator\[(AllReduce|AllGather)_([^]]+)\], local rank\[(\d+)\], "
                        r"remote rank\[(\d+)\], transporttype\[([^]]+)\]",
                        line,
                    )
                    if transport and int(transport[3]) == reference["rank"]:
                        transports[(reference["rank"], transport[2])].append(
                            {
                                "operation": transport[1],
                                "peer": int(transport[4]),
                                "transport": transport[5],
                                "source": source,
                            }
                        )

                    for kind, pattern in patterns.items():
                        match = re.search(pattern, line)
                        if match:
                            records[(reference["rank"], match[1])][kind].append(
                                {
                                    "values": [int(value) for value in match.groups()[1:]],
                                    "source": {"path": reference["path"], "line": line_number},
                                }
                            )

        for key, parts in records.items():
            # Reinitialization needs generation identity; never pick one record
            # merely because its values match or it appears last in the log.
            if any(len(parts[kind]) != 1 for kind in patterns):
                self.gaps.append(
                    {
                        "kind": "observation",
                        "reason": "communicator_configuration_incomplete_or_ambiguous",
                        "rank": key[0],
                        "group_name": key[1],
                    }
                )
                continue

            buffer_bytes, deterministic = parts["options"][0]["values"]
            rank_count, server_count, module_count = parts["topology"][0]["values"]
            self.communicators[key] = {
                "buffer_bytes": buffer_bytes,
                "deterministic": deterministic,
                "op_expansion_mode": parts["expansion"][0]["values"][0],
                "rank_count": rank_count,
                "server_count": server_count,
                "module_count": module_count,
                "source": {kind: parts[kind][0]["source"] for kind in patterns},
                "transports": transports[key],
            }
            for kind in ("device_type", "hcomm_version", "env_aicpu_unfold", "env_aiv_mode"):
                observations = worker_facts[key[0]][kind]
                values = {record["value"] for record in observations}
                self.communicators[key][kind] = next(iter(values)) if len(values) == 1 else None
                self.communicators[key]["source"][kind] = [record["source"] for record in observations]

    def _read_native_api(self) -> None:
        ranks = {call["pid"]: call["rank"] for call in self.calls.values()}
        failures = Counter()
        for reference in self.capture["native_api"]:
            with (self.directory / reference).open(encoding="utf-8") as stream:
                for line_number, line in enumerate(stream, start=1):
                    record = json.loads(line)
                    if record["end_ns"] < record["start_ns"] or record["thread_cpu_ns"] < 0:
                        raise ValueError(f"invalid native API clock interval at {reference}:{line_number}")

                    for copy in record["driver_copies"]:
                        if (
                            not (record["start_ns"] <= copy["start_ns"] <= copy["end_ns"] <= record["end_ns"])
                            or copy["thread_cpu_ns"] < 0
                        ):
                            raise ValueError(f"driver copy lies outside its parent API at {reference}:{line_number}")

                    # Scope identity is thread-local; timestamps alone cannot establish ownership.
                    record["rank"] = ranks[record["pid"]]
                    scope = record["scope_call_id"]
                    record["owner_call"] = None
                    if scope is not None:
                        identity = f"{record['rank']}:{scope}"
                        call = self.calls[identity]
                        if (call["pid"], call["tid"]) != (record["pid"], record["tid"]):
                            raise ValueError(f"native scope thread disagrees at {reference}:{line_number}")
                        record["owner_call"] = identity

                    record["source"] = {"native_api": reference, "line": line_number}
                    self.native_api.append(record)
                    if record["result"] != 0:
                        failures[record["api"]] += 1

        self.native_api.sort(key=lambda record: (record["pid"], record["call_id"]))
        self._associate_native_api()

        if not self.native_api:
            self.gaps.append({"kind": "observation", "reason": "native_api_not_observed"})
        if failures:
            self.gaps.append({"kind": "observation", "reason": "native_api_failures", "counts": dict(failures)})

    def _associate_native_api(self) -> None:
        # The SDK reports the task submitted by this native thread. Hardware
        # identity and its connection ID establish the association; profiler
        # envelopes do not necessarily include the whole intercepted API.
        hardware: dict[tuple[int, int, int], list[int]] = defaultdict(list)
        cpu: dict[tuple[int, int], list[int]] = defaultdict(list)
        for interval in self.intervals:
            arguments = interval["arguments"]
            if interval["process"] == "Ascend Hardware":
                hardware[(interval["rank"], arguments["Physic Stream Id"], arguments["Task Id"])].append(interval["id"])
            elif interval["process"] == "CANN":
                cpu[(interval["rank"], arguments["connection_id"])].append(interval["id"])

        expected = {"rtEventRecord": "device_marker", "rtStreamWaitEvent": "device_wait", "rtMemcpyAsync": "dma"}
        matches: dict[tuple[int, int, int], list[int]] = defaultdict(list)
        unresolved = Counter()
        for index, record in enumerate(self.native_api):
            offset = self.trace_clock_offsets[record["rank"]]
            record["trace_interval"] = {
                "begin_ns": record["start_ns"] + offset,
                "end_ns": record["end_ns"] + offset,
            }
            record["source_interval"] = None
            record["hardware_intervals"] = []
            record["copy_execution"] = None
            record["copy_interval"] = None
            if record["api"] not in expected or record["result"] != 0:
                continue

            if record["api"] == "rtMemcpyAsync" and record["driver_copies"]:
                copies = record["driver_copies"]
                addresses = record["copy_addresses"]
                if len(copies) != 1:
                    unresolved["compound_driver_copy_not_qualified"] += 1
                    continue
                copy = copies[0]
                if (
                    copy["result"] != 0
                    or copy["bytes"] != record["transfer_bytes"]
                    or addresses is None
                    or (copy["source"], copy["destination"]) != (addresses["source"], addresses["destination"])
                ):
                    unresolved["driver_copy_payload_not_qualified"] += 1
                    continue

                # This complete synchronous copy did not submit a DMA task.
                # The runtime's last task may still identify preceding compute.
                record["copy_execution"] = "synchronous_driver"
                # Use the exporter's recorded clock origin for parent and child.
                # Native realtime samples remain provenance, not trace coordinates.
                record["copy_interval"] = {
                    "begin_ns": record["trace_interval"]["begin_ns"] + copy["start_ns"] - record["start_ns"],
                    "end_ns": record["trace_interval"]["begin_ns"] + copy["end_ns"] - record["start_ns"],
                }
                continue
            if record["submitted_task_id"] is None:
                unresolved["task_identity_not_returned"] += 1
                continue

            key = (record["rank"], record["submitted_stream_id"], record["submitted_task_id"])
            physical = hardware[key]
            if record["stream_id"] != key[1] or not physical:
                unresolved[record["api"]] += 1
                continue
            if any(self.intervals[identity]["kind"] != expected[record["api"]] for identity in physical):
                unresolved["submitted_task_kind_disagrees"] += 1
                continue
            if record["api"] in ("rtEventRecord", "rtStreamWaitEvent"):
                name = "EVENT_RECORD" if record["api"] == "rtEventRecord" else "EVENT_WAIT"
                if len(physical) != 1 or self.intervals[physical[0]]["name"] != name:
                    unresolved["submitted_event_task_disagrees"] += 1
                    continue

            matches[key].append(index)

        for key, indices in matches.items():
            if len(indices) != 1:
                unresolved["submitted_task_identity_not_unique"] += len(indices)
                continue

            index = indices[0]
            record = self.native_api[index]
            physical = hardware[key]
            connections = {self.intervals[identity]["arguments"]["connection_id"] for identity in physical}
            if len(connections) != 1:
                unresolved["submitted_task_connection_not_unique"] += 1
                continue

            sources = cpu[(record["rank"], next(iter(connections)))]
            if len(sources) != 1:
                unresolved["submitted_task_cpu_identity_not_unique"] += 1
                continue

            expected_names = {
                "rtEventRecord": ("AscendCL@aclrtRecordEvent",),
                "rtStreamWaitEvent": ("AscendCL@aclrtStreamWaitEvent",),
                # ACLNN operators can submit DMA internally. Keep the observed
                # enclosing API; do not relabel it as a standalone memcpy call.
                "rtMemcpyAsync": ("AscendCL@aclrtMemcpyAsync", "AscendCL@aclnnInplaceCopy", "AscendCL@aclnnCat"),
            }
            if self.intervals[sources[0]]["name"] not in expected_names[record["api"]]:
                unresolved["submitted_task_cpu_api_disagrees"] += 1
                continue

            if self.intervals[sources[0]]["tid"] != record["tid"]:
                unresolved["submitted_task_cpu_thread_disagrees"] += 1
                continue

            source_owner = self.intervals[sources[0]]["owner_call"]
            if record["owner_call"] is not None and source_owner is not None and record["owner_call"] != source_owner:
                unresolved["submitted_task_scope_disagrees"] += 1
                continue

            # Commit the association only after hardware, API and scope identities agree.
            record["source_interval"] = sources[0]
            if record["owner_call"] is None:
                record["owner_call"] = source_owner
            record["hardware_intervals"] = physical
            if record["api"] == "rtMemcpyAsync":
                record["copy_execution"] = "asynchronous_device"
            for identity in physical:
                self.intervals[identity]["native_api"] = index
                if self.intervals[identity]["owner_call"] is None:
                    self.intervals[identity]["owner_call"] = record["owner_call"]

        if unresolved:
            self.gaps.append(
                {"kind": "observation", "reason": "native_api_task_association_unresolved", "counts": dict(unresolved)}
            )

        self._associate_cpu_api_calls()
        self._associate_event_destruction()

    def _associate_event_destruction(self) -> None:
        observed: dict[tuple[int, int], list[dict[str, Any]]] = defaultdict(list)
        for interval in self.intervals:
            if interval["process"] == "CANN" and interval["name"] == "Runtime@EventDestroy":
                observed[(interval["rank"], interval["tid"])].append(interval)

        starts = {}
        for thread, intervals in observed.items():
            intervals.sort(key=lambda interval: interval["start_ns"])
            starts[thread] = [interval["start_ns"] for interval in intervals]

        for record in self.native_api:
            if record["api"] != "rtEventDestroy":
                continue

            # The intercepted runtime call encloses its profiler interval. Native
            # recording can start before profiling, so ordinal pairing is unsafe.
            thread = (record["rank"], record["tid"])
            intervals = observed[thread]
            if not intervals:
                continue

            begin = record["trace_interval"]["begin_ns"]
            end = record["trace_interval"]["end_ns"]
            first = bisect_left(starts[thread], begin)
            last = bisect_right(starts[thread], end)
            if last - first != 1 or intervals[first]["end_ns"] > end:
                continue

            interval = intervals[first]
            if record["owner_call"] not in (None, interval["owner_call"]):
                continue

            # Preserve the execution scope, not the event producer's scope.
            # Destruction submits no hardware task and cannot inherit a last task ID.
            record["source_interval"] = interval["id"]
            record["owner_call"] = interval["owner_call"]

    def _associate_cpu_api_calls(self) -> None:
        # Both device-sync entry points emit the same runtime profiler name.
        # A full per-thread sequence must match; nested or missing calls stay unassociated.
        names = {
            "rtMemcpyAsync": "AscendCL@aclrtMemcpyAsync",
            "rtStreamWaitEvent": "AscendCL@aclrtStreamWaitEvent",
            "rtEventQueryStatus": "AscendCL@aclrtQueryEventStatus",
            "rtDeviceSynchronize": "Runtime@DeviceSynchronize",
            "rtDeviceSynchronizeWithTimeout": "Runtime@DeviceSynchronize",
            "workspace_allocate": "MarkovWorkspaceAllocate",
            "workspace_release": "MarkovWorkspaceRelease",
        }
        native: dict[tuple[int, int, str], list[dict[str, Any]]] = defaultdict(list)
        observed: dict[tuple[int, int, str], list[dict[str, Any]]] = defaultdict(list)
        for record in self.native_api:
            if record["api"] in names:
                native[(record["rank"], record["tid"], names[record["api"]])].append(record)
        for interval in self.intervals:
            if interval["kind"] == "cpu" and interval["name"] in names.values():
                observed[(interval["rank"], interval["tid"], interval["name"])].append(interval)

        for identity, sequence in native.items():
            calls = observed[identity]
            if len(sequence) != len(calls):
                continue
            sequence.sort(key=lambda record: record["start_ns"])
            calls.sort(key=lambda interval: interval["start_ns"])

            # Match the whole same-thread API sequence. Existing hardware links
            # may name an enclosing ACLNN call, which must contain this API.
            qualified = True
            for record, interval in zip(sequence, calls, strict=True):
                if record["owner_call"] not in (None, interval["owner_call"]):
                    qualified = False
                    break
                if record["source_interval"] is not None:
                    source = self.intervals[record["source_interval"]]
                    if not (
                        source["start_ns"] <= interval["start_ns"] <= interval["end_ns"] <= source["end_ns"]
                        and source["tid"] == interval["tid"]
                        and source["owner_call"] == interval["owner_call"]
                    ):
                        qualified = False
                        break
            if not qualified:
                continue

            # CPU attribution is independent of device submission. In particular,
            # a stale last-task ID must not create a hardware edge or clear its gap.
            for record, interval in zip(sequence, calls, strict=True):
                if record["source_interval"] is None:
                    record["source_interval"] = interval["id"]
                    record["owner_call"] = interval["owner_call"]

    def _semantic_owner(self, identity: str) -> str:
        call = self.calls[identity]
        while call["step"] in ("model_forward", "model_region", "component_region"):
            identity = f"{call['rank']}:{call['parent_id']}"
            call = self.calls[identity]

        return identity

    def _read_devices(self, rank: int, references: list[str]) -> None:
        if not references:
            self.gaps.append({"kind": "observation", "reason": "device_geometry_not_observed", "rank": rank})
            return

        for reference in references:
            with (self.directory / reference).open(encoding="utf-8") as stream:
                information = json.load(stream)

            for device in information["DeviceInfo"]:
                cube_cores = int(device["ai_core_num"])
                vector_cores = int(device["aiv_num"])
                if cube_cores <= 0 or vector_cores <= 0:
                    raise ValueError(f"invalid device core counts in {reference}")

                # These are device capacities, not the Block Num of a launch.
                # The vendor device ID is local to this worker's visible devices.
                self.devices.append(
                    {
                        "rank": rank,
                        "pid": int(information["pid"]),
                        "device": int(device["id"]),
                        "cube_cores": cube_cores,
                        "vector_cores": vector_cores,
                        "source": reference,
                    }
                )

    def _read_trace(self, reference: dict[str, Any]) -> None:
        with (self.directory / reference["profiler_info"]).open(encoding="utf-8") as stream:
            information = json.load(stream)
        rank = information["rank_id"]
        self._read_devices(rank, reference["device_info"])
        with (self.directory / reference["clock_start_info"]).open(encoding="utf-8") as stream:
            clock = json.load(stream)
        # Match CANNFileParser.get_localtime_diff / ProfilerConfig.get_local_time.
        # The export origin is not identical to a later realtime/monotonic pair.
        self.trace_clock_offsets[rank] = timestamp_ns(clock["collectionTimeBegin"]) - int(clock["clockMonotonicRaw"])

        with (self.directory / reference["trace"]).open(encoding="utf-8") as stream:
            events = json.load(stream)

        processes = {
            event["pid"]: event["args"]["name"]
            for event in events
            if event["ph"] == "M" and event["name"] == "process_name"
        }

        # Group IDs are database-local string IDs. Resolve their names, then join
        # the trace by native connection and operation identity, never by time.
        groups: dict[tuple[int, str], tuple[str, int]] = {}
        database = (self.directory / reference["database"]).resolve()
        # Traces without collective work may omit the communication database table.
        if "Communication" in processes.values():
            with closing(sqlite3.connect(database.as_uri() + "?mode=ro", uri=True)) as connection:
                for connection_id, name, group, operation_id in connection.execute(
                    "SELECT c.connectionId, n.value, g.value, c.opId FROM COMMUNICATION_OP c "
                    "JOIN STRING_IDS n ON n.id = c.opName JOIN STRING_IDS g ON g.id = c.groupName"
                ):
                    key = (connection_id, name)
                    if key in groups:
                        raise ValueError(f"ambiguous communication identity in {database}: {key}")
                    groups[key] = (group, operation_id)

        for group in sorted({value[0] for value in groups.values()}):
            if (rank, group) not in self.communicators:
                self.gaps.append(
                    {
                        "kind": "observation",
                        "reason": "communicator_configuration_missing",
                        "rank": rank,
                        "group_name": group,
                    }
                )

        kernels: dict[tuple[int, int, int], dict[str, str]] = {}
        with (self.directory / reference["kernel_details"]).open(encoding="utf-8", newline="") as stream:
            for row_index, row in enumerate(csv.DictReader(stream)):
                # Collective summaries intentionally have no physical task ID.
                if row["Stream ID"] == "N/A" or row["Task ID"] == "N/A":
                    self.supplementary_work.append(
                        {"rank": rank, "source": reference["kernel_details"], "row_index": row_index, "values": row}
                    )
                    continue

                key = (int(row["Stream ID"]), int(row["Task ID"]), timestamp_ns(row["Start Time(us)"]))
                kernels[key] = row

        markers: dict[int, list[tuple[int, int, str]]] = defaultdict(list)
        marker_ranges: dict[str, tuple[int, int]] = {}
        for event in events:
            if event["ph"] != "X" or not event["name"].startswith("markov/"):
                continue

            _, step, call_id = event["name"].split("/")
            identity = f"{rank}:{call_id}"
            if self.calls[identity]["step"] != step:
                raise ValueError(f"profiler marker disagrees with observed call {identity}")

            start_ns = timestamp_ns(event["ts"])
            markers[event["tid"]].append((start_ns, start_ns + timestamp_ns(event["dur"]), identity))
            marker_ranges[identity] = (start_ns, start_ns + timestamp_ns(event["dur"]))

        for marker_list in markers.values():
            marker_list.sort(key=lambda marker: (marker[0], self.calls[marker[2]]["call_id"]))

        starts: dict[tuple[int, int, int], list[int]] = defaultdict(list)
        queue_owners: dict[int, str] = {}
        local_intervals: list[dict[str, Any]] = []
        flow_events: dict[tuple[str, str], dict[str, Any]] = defaultdict(dict)
        ignored = Counter()
        for event_index, event in enumerate(events):
            if event["ph"] in ("s", "f"):
                flow_events[(event["cat"], str(event["id"]))][event["ph"]] = event
                continue
            if event["ph"] != "X":
                continue

            process = processes[event["pid"]]
            if process not in ("Python", "CANN", "Ascend Hardware", "Communication"):
                ignored[process] += 1
                continue

            start_ns = timestamp_ns(event["ts"])
            end_ns = start_ns + timestamp_ns(event["dur"])
            arguments = event.get("args", {})
            kind = "cpu"
            if process == "Ascend Hardware":
                task_type = arguments["Task Type"]
                if task_type in ("EVENT_WAIT", "Notify Wait"):
                    kind = "device_wait"
                elif task_type in ("SDMA", "SDMA_SQE", "PCIE_DMA_SQE"):
                    kind = "dma"
                elif task_type in (
                    "EVENT_RECORD",
                    "Notify Record",
                    "Write Value",
                    "PROFILING_ENABLE",
                    "PROFILING_DISABLE",
                ):
                    kind = "device_marker"
                else:
                    kind = "compute"
            elif process == "Communication":
                kind = "communication_detail"

            owner = None
            if kind == "cpu":
                # Native record_function identities define the synchronous call tree.
                # Device ownership is established below through queue/flow identities.
                sequence = markers[event["tid"]]
                position = bisect_right(sequence, start_ns, key=lambda marker: marker[0]) - 1
                identity = sequence[position][2] if position >= 0 else None
                # Locate a recorded stack, then follow its explicit parents.
                # Adding per-layer regions must not scan every prior marker for
                # every CPU interval. Equal starts use native call entry order.
                while identity is not None:
                    bounds = marker_ranges.get(identity)
                    if bounds is not None and bounds[0] <= start_ns and end_ns <= bounds[1]:
                        owner = identity
                        break
                    call = self.calls[identity]
                    identity = None if call["parent_id"] is None else f"{rank}:{call['parent_id']}"

            interval = {
                "id": len(self.intervals),
                "rank": rank,
                "pid": event["pid"],
                "tid": event["tid"],
                "process": process,
                "name": event["name"],
                "kind": kind,
                "start_ns": start_ns,
                "end_ns": end_ns,
                "observed_wall_ns": end_ns - start_ns,
                "owner_call": owner,
                "arguments": arguments,
                "source": {"trace": reference["trace"], "event_index": event_index},
            }
            if process == "Communication" and event["name"].startswith("hcom_"):
                group, operation_id = groups[(arguments["connection_id"], event["name"])]
                arguments["group_name"] = group
                interval["communicator"] = self.communicators.get((rank, group))
                interval["source"]["communication_database"] = reference["database"]
                interval["source"]["communication_op_id"] = operation_id

            if process == "Ascend Hardware":
                kernel = kernels.get((arguments["Physic Stream Id"], arguments["Task Id"], start_ns))
                if kernel is not None:
                    interval["kernel"] = {
                        key: kernel[key]
                        for key in (
                            "Type",
                            "Input Shapes",
                            "Input Data Types",
                            "Input Formats",
                            "Output Shapes",
                            "Output Data Types",
                            "Output Formats",
                            "Block Dim" if "Block Dim" in kernel else "Block Num",
                            "Context ID",
                        )
                    }
            if event.get("cat") == "enqueue" and owner is not None:
                queue_owners[arguments["correlation_id"]] = owner

            starts[(event["pid"], event["tid"], start_ns)].append(interval["id"])
            local_intervals.append(interval)
            self.intervals.append(interval)

        # Carry scope ownership across native queues before resolving device flows.
        dequeues: dict[int, list[tuple[int, int, str]]] = defaultdict(list)
        for interval in local_intervals:
            if interval["name"].startswith("Dequeue@"):
                owner = queue_owners.get(interval["arguments"]["correlation_id"])
                interval["owner_call"] = owner
                if owner is not None:
                    dequeues[interval["tid"]].append((interval["start_ns"], interval["end_ns"], owner))

        for sequence in dequeues.values():
            sequence.sort()
        for interval in local_intervals:
            if interval["owner_call"] is not None:
                continue
            if interval["process"] != "CANN" and interval["name"] not in (
                "MarkovWorkspaceAllocate",
                "MarkovWorkspaceRelease",
            ):
                continue

            sequence = dequeues[interval["tid"]]
            position = bisect_right(sequence, (interval["start_ns"], float("inf"), "")) - 1
            if position >= 0:
                begin, end, identity = sequence[position]
                if begin <= interval["start_ns"] and interval["end_ns"] <= end:
                    interval["owner_call"] = identity

        # Native connection IDs locate the actual launch; queue correlation IDs
        # connect its worker to the originating host enqueue. Vendor torch flows
        # can instead name an unrelated nested record_function probe.
        submissions: dict[int, list[int]] = defaultdict(list)
        enqueues: dict[int, list[int]] = defaultdict(list)
        worker_windows: dict[int, list[dict[str, Any]]] = defaultdict(list)
        runtime_submissions: dict[int, list[dict[str, Any]]] = defaultdict(list)

        # These profiler modes denote actual task submission, not preparation or
        # a high-level operator envelope. Keep backend spelling in this adapter.
        submission_modes = {
            "LaunchKernelV2",
            "KernelLaunch",
            "KernelLaunchWithHandle",
            "KernelLaunchWithHandle_Big",
            "FftsPlusTaskLaunch",
            "EventRecord",
            "StreamWaitEvent",
            "MemcpyAsync",
        }

        for interval in local_intervals:
            arguments = interval["arguments"]
            if interval["process"] == "CANN":
                submissions[arguments["connection_id"]].append(interval["id"])
                if arguments.get("level") == "runtime" and arguments.get("Mode") in submission_modes:
                    runtime_submissions[interval["tid"]].append(interval)
            elif interval["name"].startswith("Enqueue@"):
                enqueues[arguments["correlation_id"]].append(interval["id"])
            elif interval["name"].startswith("Dequeue@"):
                worker_windows[interval["tid"]].append(interval)

        for sequence in worker_windows.values():
            sequence.sort(key=lambda interval: interval["start_ns"])
        for sequence in runtime_submissions.values():
            sequence.sort(key=lambda interval: interval["start_ns"])

        for (category, identity), endpoints in flow_events.items():
            if "s" not in endpoints or "f" not in endpoints:
                self.gaps.append(
                    {
                        "kind": "observation",
                        "reason": "unpaired_native_flow",
                        "rank": rank,
                        "category": category,
                        "identity": identity,
                    }
                )
                continue

            source, target = endpoints["s"], endpoints["f"]
            source_ids = starts[(source["pid"], source["tid"], timestamp_ns(source["ts"]))]
            target_ids = starts[(target["pid"], target["tid"], timestamp_ns(target["ts"]))]
            profiler_source_ids = source_ids
            source_binding = None
            if category == "HostToDevice":
                submission = self._flow_submission(source_ids, target_ids, runtime_submissions)
                if submission is not None:
                    source_ids = [submission]
                    source_binding = "native_flow_runtime_submission"
            if category == "async_npu" and len(target_ids) == 1:
                enqueue = self._flow_enqueue(self.intervals[target_ids[0]], submissions, worker_windows, enqueues)
                if enqueue is not None:
                    host = self.intervals[enqueue]
                    # Containment validates an already identity-linked enqueue;
                    # it never chooses a device producer by temporal proximity.
                    valid = any(
                        self.intervals[index]["pid"] == host["pid"]
                        and self.intervals[index]["tid"] == host["tid"]
                        and self.intervals[index]["start_ns"] <= host["start_ns"]
                        and host["end_ns"] <= self.intervals[index]["end_ns"]
                        for index in source_ids
                    )
                    if not valid:
                        source_ids = [enqueue]
                        source_binding = "native_connection_queue"

            owners = {self.intervals[index]["owner_call"] for index in source_ids} - {None}
            if len(owners) == 1:
                owner = owners.pop()
                for index in target_ids:
                    previous_owner = self.intervals[index]["owner_call"]
                    if previous_owner is None:
                        self.intervals[index]["owner_call"] = owner
                    elif previous_owner != owner:
                        self.gaps.append(
                            {"kind": "observation", "reason": "conflicting_flow_ownership", "interval": index}
                        )

            self.flows.append(
                {
                    "rank": rank,
                    "category": category,
                    "identity": identity,
                    "source_intervals": source_ids,
                    "target_intervals": target_ids,
                    "source_time_ns": timestamp_ns(source["ts"]),
                    "target_time_ns": timestamp_ns(target["ts"]),
                }
            )
            if source_ids != profiler_source_ids:
                self.flows[-1]["profiler_source_intervals"] = profiler_source_ids
                self.flows[-1]["source_binding"] = source_binding
                self.flows[-1]["source_time_ns"] = self.intervals[source_ids[0]]["start_ns"]

        # CANN, hardware tasks and HCCL summaries share explicit connection IDs.
        # The collective root receives its owner through the native flow; propagate
        # that identity to its SDMA/notify sub-tasks without using temporal proximity.
        connection_owners: dict[int, set[str]] = defaultdict(set)
        for interval in local_intervals:
            connection = interval["arguments"].get("connection_id")
            if connection is not None and interval["owner_call"] is not None:
                connection_owners[connection].add(interval["owner_call"])
        for interval in local_intervals:
            connection = interval["arguments"].get("connection_id")
            if interval["owner_call"] is None and connection is not None:
                owners = connection_owners[connection]
                if len(owners) == 1:
                    interval["owner_call"] = next(iter(owners))

        task_owners: dict[tuple[int, int], set[str]] = defaultdict(set)
        for interval in local_intervals:
            if interval["process"] == "Ascend Hardware" and interval["owner_call"] is not None:
                arguments = interval["arguments"]
                task_owners[(arguments["Physic Stream Id"], arguments["Task Id"])].add(interval["owner_call"])
        for interval in local_intervals:
            arguments = interval["arguments"]
            if interval["process"] == "Communication" and "task id" in arguments:
                owners = task_owners[(arguments["stream id"], arguments["task id"])]
                if len(owners) == 1:
                    interval["owner_call"] = next(iter(owners))

        unresolved = Counter(
            flow["category"]
            for flow in self.flows
            if flow["rank"] == rank and (not flow["source_intervals"] or not flow["target_intervals"])
        )
        if unresolved:
            self.gaps.append(
                {
                    "kind": "observation",
                    "reason": "native_flow_endpoints_not_matched",
                    "rank": rank,
                    "counts": dict(unresolved),
                }
            )
        if ignored:
            self.gaps.append(
                {"kind": "scope", "reason": "vendor_analysis_channels_excluded", "rank": rank, "counts": dict(ignored)}
            )

    def _flow_submission(
        self,
        sources: list[int],
        targets: list[int],
        submissions: dict[int, list[dict[str, Any]]],
    ) -> int | None:
        if not sources or not targets:
            return None

        parents = [self.intervals[index] for index in sources]
        first = parents[0]
        if any(parent["process"] != "CANN" or parent["tid"] != first["tid"] for parent in parents):
            return None

        # The native flow establishes device identity first. Containment only
        # refines that known producer; never choose a nearby launch by time.
        connections = {parent["arguments"]["connection_id"] for parent in parents}
        if any(self.intervals[index]["arguments"].get("connection_id") not in connections for index in targets):
            return None

        begin = max(parent["start_ns"] for parent in parents)
        end = min(parent["end_ns"] for parent in parents)
        sequence = submissions.get(first["tid"], [])
        position = bisect_left(sequence, begin, key=lambda interval: interval["start_ns"])
        stop = bisect_left(sequence, end, key=lambda interval: interval["start_ns"])
        matches = []
        for interval in sequence[position:stop]:
            if interval["end_ns"] <= end:
                matches.append(interval["id"])

        return matches[0] if len(matches) == 1 else None

    def _flow_enqueue(
        self,
        target: dict[str, Any],
        submissions: dict[int, list[int]],
        worker_windows: dict[int, list[dict[str, Any]]],
        enqueues: dict[int, list[int]],
    ) -> int | None:
        matches = submissions.get(target["arguments"].get("connection_id"), [])
        if len(matches) != 1:
            return None

        submission = self.intervals[matches[0]]
        sequence = worker_windows.get(submission["tid"], [])
        position = bisect_right(sequence, submission["start_ns"], key=lambda interval: interval["start_ns"]) - 1
        if position < 0:
            return None
        worker = sequence[position]
        if submission["end_ns"] > worker["end_ns"]:
            return None

        hosts = enqueues.get(worker["arguments"]["correlation_id"], [])
        if len(hosts) != 1:
            return None
        return hosts[0]


class ServingNormalizer(ObservationNormalizer):
    def __init__(self, capture: Path, role: str) -> None:
        super().__init__(capture)
        if role not in {"base", "validation"} or self.capture["role"] != role:
            raise ValueError("serving observation role must match the native capture")
        self.role = role

    def _read_event_states(self) -> list[dict[str, Any]]:
        ranks = {call["pid"]: call["rank"] for call in self.calls.values()}
        states = []
        observed = set()
        for reference in self.capture["event_history"]:
            state = EventHistory(self.directory, reference).initial_state()
            state["rank"] = ranks[state["pid"]]
            if state["pid"] in observed:
                raise ValueError(f"duplicate event entry history for worker: {state['pid']}")
            observed.add(state["pid"])
            self.gaps.extend({"kind": "event_state", "rank": state["rank"], **gap} for gap in state["gaps"])
            states.append(state)

        for pid in ranks.keys() - observed:
            self.gaps.append({"kind": "event_state", "reason": "entry_history_not_observed", "rank": ranks[pid]})
        return states

    def _read_workspace_states(self) -> list[dict[str, Any]]:
        states = []
        workers = {(call["rank"], call["pid"]) for call in self.calls.values()}
        observed_workers = set()
        for reference in self.capture["workspace_states"]:
            state = json.loads((self.directory / reference).read_text(encoding="utf-8"))
            worker = (state["rank"], state["pid"])
            if worker not in workers or worker in observed_workers:
                raise ValueError(f"workspace state has an unknown or duplicate worker: {reference}")
            observed_workers.add(worker)
            states.append(state)

            if state["probe_installation"] is None:
                self.gaps.append({"kind": "workspace", "reason": "probe_installation_missing", "source": reference})
                continue
            if state["probe_installation"]["torch_npu_source_commit"] != WorkspaceAllocation.source_commit:
                self.gaps.append(
                    {"kind": "workspace", "reason": "allocator_implementation_not_supported", "source": reference}
                )
                continue
            if state["initial"] is None or state["final"] is None:
                self.gaps.append({"kind": "workspace", "reason": "incomplete_window_state", "source": reference})
                continue

            # Stream handles join the allocator snapshot to native calls. Device
            # stream IDs are a different identity, used later for graph resources.
            current = {
                (entry["device"], entry["stream_handle"]): (entry["capacity_bytes"], entry["address"])
                for entry in state["initial"]
            }
            records = sorted(
                (
                    record
                    for record in self.native_api
                    if (record["rank"], record["pid"]) == worker and "workspace" in record
                ),
                key=lambda record: record["start_ns"],
            )
            for record in records:
                if record["source_interval"] is None or record["scope_call"] is None:
                    self.gaps.append(
                        {"kind": "workspace", "reason": "semantic_scope_unresolved", "source": record["source"]}
                    )

                transition = record["workspace"]
                key = (transition["device"], record["stream_handle"])
                before = (transition["capacity_before_bytes"], transition["address_before"])
                if record["result"] != 0 or transition["policy"] != "cached":
                    self.gaps.append(
                        {"kind": "workspace", "reason": "unqualified_transition", "source": record["source"]}
                    )
                    break
                if current.get(key, (0, 0)) != before:
                    self.gaps.append({"kind": "workspace", "reason": "state_discontinuity", "source": record["source"]})
                    break

                allocation = WorkspaceAllocation.from_observation(transition)
                if not allocation.agrees_with(transition):
                    self.gaps.append(
                        {"kind": "workspace", "reason": "allocator_branch_disagrees", "source": record["source"]}
                    )
                    break

                # Keep the native state and derived branch separate. A required
                # synchronization is not an observed or qualified dependency.
                record["workspace_allocation"] = allocation.describe()
                if transition["action"] == "release":
                    current.pop(key, None)
                else:
                    current[key] = (transition["capacity_after_bytes"], transition["address_after"])
            else:
                final = {
                    (entry["device"], entry["stream_handle"]): (entry["capacity_bytes"], entry["address"])
                    for entry in state["final"]
                }
                if current != final:
                    self.gaps.append({"kind": "workspace", "reason": "final_state_disagrees", "source": reference})

        for rank, pid in sorted(workers - observed_workers):
            self.gaps.append({"kind": "workspace", "reason": "window_state_missing", "rank": rank, "pid": pid})
        return states

    def extract(self) -> Path:
        self._read_capture()
        workspace_states = self._read_workspace_states()
        event_states = self._read_event_states()
        facts_path = self.directory / ("source_facts.json" if self.role == "base" else "observation_facts.json")
        write_json(
            facts_path,
            {
                "calls": self.calls,
                "intervals": self.intervals,
                "flows": self.flows,
                "native_api": self.native_api,
                "supplementary_work": self.supplementary_work,
                "workspace_states": workspace_states,
                "event_states": event_states,
                "profile_controls": self.profile_controls,
                "devices": self.devices,
            },
            compact=True,
        )

        manifest = {
            "role": self.role,
            "framework": self.capture["framework"],
            "input": self.capture["input"],
            "environment": self.capture["environment"],
            "native_capture": "native_capture.json",
            "clock_domains": {
                "native_intervals": (
                    "profiler export epoch coordinates in ns, converted from vendor decimal microseconds"
                ),
                "calls": "monotonic_ns; paired wall_time_ns is retained for clock association",
                "thread_cpu_ns": "per-thread CPU time; not a migrated wall service estimate",
                "native_api": (
                    "monotonic_ns; trace_interval uses recorded CANN export origin, wall_time_ns is raw provenance"
                ),
            },
            "coverage": {
                "calls": len(self.calls),
                "intervals": len(self.intervals),
                "flows": len(self.flows),
                "native_api": len(self.native_api),
                "devices": len(self.devices),
                "unowned": dict(self.unowned),
            },
            "gaps": self.gaps,
        }
        if self.role == "base":
            manifest_path = self.directory / "profile_manifest.json"
            manifest.update(source_facts=facts_path.name, source_dag=None, templates=None, readiness="needs_data")
            manifest["gaps"] = self.gaps + [
                {
                    "kind": "implementation",
                    "reason": "source_dag_and_templates_not_extracted",
                }
            ]
        else:
            # Target observations are evaluation facts, never a template source or cost asset.
            manifest_path = self.directory / "validation_manifest.json"
            manifest.update(
                candidate_id=self.capture["candidate_id"],
                observations=facts_path.name,
                readiness="partial" if self.gaps else "observed",
            )

        write_json(manifest_path, manifest)

        return manifest_path


class DeviceCostNormalizer(ObservationNormalizer):
    def __init__(self, capture: Path) -> None:
        super().__init__(capture)
        if self.capture["role"] != "performance":
            raise ValueError("independent device costs must come from a performance capture")

    @staticmethod
    def kernel_cost_work(interval: dict[str, Any]) -> dict[str, Any] | None:
        kernel = interval.get("kernel")
        if interval["kind"] != "compute" or kernel is None:
            return None

        # Vendor column names stop at this adapter. Capacity, rank and trace
        # identities are not substitutes for the actual launch dimensions.
        return {
            "name": interval["name"],
            "operation": kernel["Type"],
            "input_shapes": kernel["Input Shapes"],
            "output_shapes": kernel["Output Shapes"],
            "input_dtypes": kernel["Input Data Types"],
            "output_dtypes": kernel["Output Data Types"],
            "input_formats": kernel["Input Formats"],
            "output_formats": kernel["Output Formats"],
            "task_type": interval["arguments"]["Task Type"],
            "launch_blocks": int(kernel["Block Dim" if "Block Dim" in kernel else "Block Num"]),
        }

    def extract(self) -> Path:
        from markov_collection.artifacts.sampling_domain import SamplingDomain

        self._read_capture()
        with (self.directory / self.capture["input"]).open(encoding="utf-8") as stream:
            prepared = json.load(stream)

        domain = SamplingDomain(prepared)
        for call in sorted(self.calls.values(), key=lambda call: (call["rank"], call["start_ns"])):
            if "requests" in call["metadata"]:
                domain.observe(call["rank"], call["step"], call["metadata"])

        samples = []
        collectives = []
        collective_owners: dict[tuple[int, int], list[dict[str, Any]]] = defaultdict(list)
        unresolved = Counter()
        for interval in self.intervals:
            arguments = interval["arguments"]
            if interval["process"] != "Communication" or "count" not in arguments or "data_type" not in arguments:
                continue

            owner = interval["owner_call"]
            if owner is None or "requests" not in self.calls[owner]["metadata"]:
                unresolved["collective"] += 1
                continue

            call = self.calls[owner]
            collectives.append(
                {
                    "rank": interval["rank"],
                    "stage": domain.stage(call["metadata"]),
                    "step": call["step"],
                    "metadata": call["metadata"],
                    "operation": interval["name"].split("__", 1)[0],
                    "element_count": arguments["count"],
                    "data_type": arguments["data_type"],
                    "algorithm": arguments["alg_type"],
                    "connection_id": arguments["connection_id"],
                    "observed_wall_ns": interval["observed_wall_ns"],
                    "source": interval["source"],
                }
            )
            collective_owners[(interval["rank"], arguments["connection_id"])].append(collectives[-1])

        for interval in self.intervals:
            if interval["kind"] not in ("compute", "dma"):
                continue

            owner = interval["owner_call"]
            if owner is None or "requests" not in self.calls[owner]["metadata"]:
                unresolved[interval["kind"]] += 1
                continue

            call = self.calls[owner]
            transfer = None
            if "native_api" in interval:
                api = self.native_api[interval["native_api"]]
                if api["api"] == "rtMemcpyAsync":
                    transfer = {
                        "bytes": api["transfer_bytes"],
                        "direction": api["transfer_kind"],
                        "source": api["source"],
                    }

            samples.append(
                {
                    "rank": interval["rank"],
                    "stage": domain.stage(call["metadata"]),
                    "step": call["step"],
                    "metadata": call["metadata"],
                    "kind": interval["kind"],
                    "name": interval["name"],
                    "duration_ns": interval["observed_wall_ns"],
                    "arguments": interval["arguments"],
                    "kernel": interval.get("kernel"),
                    "kernel_work": self.kernel_cost_work(interval),
                    "transfer": transfer,
                    "source": interval["source"],
                }
            )

        task_samples = self._task_samples(collective_owners, unresolved)
        sample_path = self.directory / "device_measurements.json"
        write_json(
            sample_path,
            {
                "samples": samples,
                "collective_descriptors": collectives,
                "task_samples": task_samples,
                "collective_observations": self.supplementary_work,
                "devices": self.devices,
            },
            compact=True,
        )
        if unresolved:
            self.gaps.append({"kind": "cost", "reason": "device_work_context_not_observed", "counts": dict(unresolved)})
        self.gaps.append({"kind": "cost", "reason": "collective_transport_service_not_separated"})
        self.gaps.append({"kind": "cost", "reason": "device_profiler_overhead_not_calibrated"})

        coverage = domain.coverage()
        if coverage["missing_conditions"]:
            self.gaps.append({"kind": "cost", "reason": "warmup_execution_domain_incomplete"})

        manifest_path = self.directory / "performance_manifest.json"
        write_json(
            manifest_path,
            {
                "role": "performance",
                "framework": self.capture["framework"],
                "measurement_kind": "native_device_work",
                "input": self.capture["input"],
                "environment": self.capture["environment"],
                "completions": self.capture["responses"],
                "native_capture": "native_capture.json",
                "device_measurements": sample_path.name,
                "warmup_coverage": coverage,
                "coverage": {
                    "samples": len(samples),
                    "stage_kind": dict(Counter(f"{sample['stage']}/{sample['kind']}" for sample in samples)),
                    "collective_observations": len(self.supplementary_work),
                    "collective_descriptors": len(collectives),
                    "task_samples": len(task_samples),
                    "task_kinds": dict(Counter(sample["kind"] for sample in task_samples)),
                },
                "clock": {"duration_ns": "physical hardware span; compound transport is not service cost"},
                "readiness": "needs_data",
                "gaps": self.gaps,
            },
        )

        return manifest_path


class CollectiveCostNormalizer(ObservationNormalizer):
    def __init__(self, capture: Path) -> None:
        super().__init__(capture)
        if self.capture["role"] != "performance":
            raise ValueError("independent collective measurements require a performance capture")

    def extract(self) -> Path:
        self._read_capture()
        with (self.directory / self.capture["input"]).open(encoding="utf-8") as stream:
            prepared = json.load(stream)

        world_size = prepared["configuration"]["tp_size"]
        samples = []
        grouped: dict[tuple[int, int], list[dict[str, Any]]] = defaultdict(list)
        collectives: dict[tuple[int, int], list[dict[str, Any]]] = defaultdict(list)
        unresolved = Counter()
        for interval in self.intervals:
            args = interval["arguments"]
            if interval["process"] != "Communication" or "count" not in args or "data_type" not in args:
                continue

            owner = interval["owner_call"]
            if owner is None:
                unresolved["collective_context_missing"] += 1
                continue

            call = self.calls[owner]
            metadata = call["metadata"]
            if call["step"] == "setup":
                continue

            operation = interval["name"].split("__", 1)[0]
            if operation != metadata["operation"] or args["count"] != metadata["element_count"]:
                unresolved["collective_operation_decomposition_differs"] += 1
                continue
            if args["data_type"] != metadata["data_type"] or args["alg_type"] != metadata["algorithm"]:
                unresolved["collective_implementation_differs"] += 1

            sample = {
                "rank": interval["rank"],
                "stage": metadata["stage"],
                "grid": metadata["grid"],
                "repetition": metadata["repetition"],
                "operation": operation,
                "element_count": args["count"],
                "data_type": args["data_type"],
                "algorithm": args["alg_type"],
                "device_start_ns": interval["start_ns"],
                "device_end_ns": interval["end_ns"],
                "observed_wall_ns": interval["observed_wall_ns"],
                "host_start_ns": call["start_ns"],
                "host_end_ns": call["end_ns"],
                "host_active_cpu_ns": call["thread_cpu_ns"],
                "source": interval["source"],
            }

            samples.append(sample)
            collectives[(interval["rank"], args["connection_id"])].append(sample)
            grouped[(metadata["grid"], metadata["repetition"])].append(sample)

        # A collective sample requires all ranks from the same grid point and repetition.
        groups = []
        for point in prepared["measurement_grid"]:
            for repetition in range(-1, prepared["repetitions"]):
                records = grouped[(point["id"], repetition)]
                if len(records) != world_size or {record["rank"] for record in records} != set(range(world_size)):
                    unresolved["collective_rank_observation_incomplete"] += 1
                    continue

                starts = [record["device_start_ns"] for record in records]
                ends = [record["device_end_ns"] for record in records]
                groups.append(
                    {
                        "grid": point["id"],
                        "stage": "warmup" if repetition == -1 else "measurement",
                        "repetition": repetition,
                        "operation": point["operation"],
                        "element_count": point["element_count"],
                        "data_type": point["data_type"],
                        "rank_start_skew_ns": max(starts) - min(starts),
                        "group_observed_wall_ns": max(ends) - min(starts),
                        "after_all_ranks_started_ns": max(ends) - max(starts),
                        "ranks": records,
                    }
                )

        measurements = self.directory / "collective_measurements.json"
        task_samples = self._task_samples(collectives, unresolved)
        write_json(
            measurements,
            {"rank_samples": samples, "groups": groups, "task_samples": task_samples, "devices": self.devices},
            compact=True,
        )
        if unresolved:
            self.gaps.append(
                {"kind": "cost", "reason": "collective_measurement_incomplete", "counts": dict(unresolved)}
            )
        self.gaps.append({"kind": "cost", "reason": "collective_admission_skew_and_service_not_separated"})
        self.gaps.append({"kind": "cost", "reason": "device_profiler_overhead_not_calibrated"})

        manifest = self.directory / "performance_manifest.json"
        write_json(
            manifest,
            {
                "role": "performance",
                "framework": self.capture["framework"],
                "measurement_kind": "native_collective_work",
                "input": self.capture["input"],
                "environment": self.capture["environment"],
                "completions": self.capture["responses"],
                "native_capture": "native_capture.json",
                "collective_measurements": measurements.name,
                "coverage": {
                    "rank_samples": len(samples),
                    "groups": len(groups),
                    "stage_groups": dict(Counter(group["stage"] for group in groups)),
                    "task_samples": len(task_samples),
                    "task_kinds": dict(Counter(sample["kind"] for sample in task_samples)),
                },
                "clock": {
                    "device_start_ns/device_end_ns": "wall_epoch_ns; native per-rank collective spans",
                    "host_start_ns/host_end_ns": "native monotonic_ns; host call, not hardware service",
                },
                "readiness": "needs_data",
                "gaps": self.gaps,
            },
        )

        return manifest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", type=Path, required=True)
    parser.add_argument("--role", choices=["base", "performance", "validation"], default="base")
    arguments = parser.parse_args()

    if arguments.role in {"base", "validation"}:
        normalizer = ServingNormalizer(arguments.capture, arguments.role)
    else:
        with (arguments.capture.parent / "prepared_input.json").open(encoding="utf-8") as stream:
            prepared = json.load(stream)
        normalizer = (
            CollectiveCostNormalizer(arguments.capture)
            if prepared["measurement_kind"] == "communication"
            else DeviceCostNormalizer(arguments.capture)
        )

    print(normalizer.extract())


if __name__ == "__main__":
    main()
