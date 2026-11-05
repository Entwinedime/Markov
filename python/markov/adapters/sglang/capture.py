"""Collect a bounded serial HTTP workload inside its declared inference container."""

import argparse
from collections import Counter
from contextlib import ExitStack
import os
import shutil
from pathlib import Path
import sys
import time
import json

from markov.artifacts.capture import capture_origin
from markov.artifacts.request import read_json
from markov.artifacts.writing import write_json
from markov.instrumentation.http import SerialHttpClient, encode_inputs, inference_server


HICACHE_IDLE_POLL_SEC = 0.1


CAPTURE_OWNED_ARGUMENTS = {
    "model_path",
    "dtype",
    "tp_size",
    "host",
    "port",
    "skip_server_warmup",
    "base_gpu_id",
    "gpu_id_step",
}


def server_arguments(deployment, configuration):
    if CAPTURE_OWNED_ARGUMENTS.intersection(configuration):
        raise ValueError("Model, dtype and TP belong to deployment; capture owns the isolated endpoint and startup")
    if configuration.get("disable_cuda_graph", True) is not True:
        raise ValueError("The restored base capture currently supports eager execution")
    return {
        "disable_cuda_graph": True,
        "attention_backend": "ascend",
        "sampling_backend": "ascend",
        "enable_deterministic_inference": True,
        "max_running_requests": 1,
        "chunked_prefill_size": -1,
        **configuration,
        "model_path": "/model",
        "dtype": deployment.dtype,
        "tp_size": deployment.tensor_parallel,
        "host": "127.0.0.1",
        "port": 30000,
        "skip_server_warmup": True,
    }


def capture_environment(profiling: bool, calibration: bool = False, *, semantic: bool = False) -> dict[str, str]:
    """Keep inference conditions equal while disabling all Markov instruments for normal time."""
    environment = {
        "TRITON_ENABLE_TASKQUEUE": "true",
        "ASCEND_USE_FIA": "1",
        "SGLANG_ENABLE_HEALTH_ENDPOINT_GENERATION": "0",
        "MARKOV_INPUT_FACTS": "",
        "MARKOV_PROBE_DETAIL": "semantic" if semantic else "diagnostic",
        "MARKOV_NATIVE_PROBE": "",
        "MARKOV_REGION_MEASUREMENTS": "/capture/regions" if calibration else "",
        "MARKOV_FILE_MEASUREMENTS": "",
        "LD_PRELOAD": "",
        "SGLANG_HICACHE_FILE_BACKEND_STORAGE_DIR": "/capture/storage",
        "SGLANG_HICACHE_FILE_BACKEND_MAX_SIZE": "0",
        "SGLANG_HICACHE_FILE_BACKEND_MIN_FREE_SPACE": "0",
    }
    if profiling:
        environment.update(
            SGLANG_NPU_PROFILER_SERIAL_EXPORT="1",
            SGLANG_NPU_PROFILER_LEVEL="2",
            MARKOV_INPUT_FACTS="/capture/probe",
            MARKOV_NATIVE_PROBE="/capture/native",
            LD_PRELOAD="/opt/markov/lib/libmarkov_runtime_probe.so",
        )
    elif semantic:
        environment["MARKOV_INPUT_FACTS"] = "/capture/probe"
    return environment


class Capture(SerialHttpClient):
    def __init__(self, path: Path) -> None:
        self.path = path
        self.directory = path.parent
        self.plan = read_json(path)
        super().__init__(
            self.directory,
            self.plan["workload"]["formal_begin_index"],
            time.monotonic() + self.plan["budget"]["wall_sec"],
        )

    def wait_hicache_idle(self) -> None:
        row = {"kind": "barrier", "scope": "hicache_idle", "measure": False, "status": "running"}
        self.rows.append(row)
        write_json(self.directory / "workload_report.json", {"requests": self.rows})
        begin = time.time_ns()
        polls = 0
        try:
            while True:
                payload, _, _ = self.http("/server_info")
                states = json.loads(payload)["internal_states"]
                polls += 1
                if states and all(state["hicache_diagnostic"]["hicache_idle"] for state in states):
                    break
                time.sleep(min(HICACHE_IDLE_POLL_SEC, self.remaining_sec()))
        except Exception as error:
            row.update(status="error", error=str(error))
            write_json(self.directory / "workload_report.json", {"requests": self.rows})
            raise

        row.update(
            status="ok",
            start_time_ms=self.milliseconds(begin),
            end_time_ms=self.milliseconds(time.time_ns()),
            polls=polls,
            pending=[state["hicache_diagnostic"]["pending"] for state in states],
            last_response_bytes=len(payload),
        )
        write_json(self.directory / "workload_report.json", {"requests": self.rows})

    def prepare_inputs(self) -> list[dict]:
        requests = self.plan["workload"]["requests"]
        encode_inputs(
            requests,
            self.plan["server"]["model_path"],
            self.plan["budget"]["input_tokens"],
            self.plan["server"].get("trust_remote_code", False),
        )

        server = self.plan["server"]
        if self.plan["workload"].get("drain_before_formal", False) and not server.get(
            "enable_hierarchical_cache", False
        ):
            raise ValueError("drain_before_formal requires SGLang HiCache")

        if "max_total_tokens" in server and "page_size" in server and not server.get("disable_radix_cache", False):
            # Match Scheduler.init_req_max_new_tokens. Smaller pools can silently
            # reduce the requested output, or leave the request unable to enter.
            page = server["page_size"]
            capacity = server["max_total_tokens"] // page * page
            for index, row in enumerate(requests):
                paged = (len(row["input_ids"]) + page - 1) // page * page
                required = paged + row["max_new_tokens"] + page + 1
                if capacity < required:
                    raise ValueError(
                        f"SGLang request {index} needs at least {required} device tokens; the pool provides {capacity}"
                    )
        return requests

    def run(self) -> None:
        profiling_requested = self.plan["profiling"]
        semantic = self.plan.get("semantic", False)
        if self.plan["calibration"] or self.plan["record_regions"]:
            origin = capture_origin(self.directory)
        for flag, directory in (
            ("MARKOV_REGION_MEASUREMENTS", "regions"),
            ("MARKOV_FILE_MEASUREMENTS", "file_workers"),
        ):
            if self.plan["environment"].get(flag):
                (self.directory / directory).mkdir()
        control = self.directory / "native" / "enabled"
        if profiling_requested:
            control.parent.mkdir()
            control.write_bytes(b"\0")
            (self.directory / "trace").mkdir()
        if profiling_requested or semantic:
            (self.directory / "probe").mkdir()
        requests = self.prepare_inputs()
        command = [sys.executable, "-m", "markov.adapters.sglang.capture", "server", "--input", str(self.path)]
        write_json(
            self.directory / "config.json",
            {
                "deployment": self.plan["deployment"],
                "workload": self.plan["workload"],
                "server": {
                    "command": command,
                    "arguments": self.plan["server"],
                    "environment": self.plan["environment"],
                },
            },
        )
        profiling = False
        with ExitStack() as stack:
            gate = stack.enter_context(control.open("r+b", buffering=0)) if profiling_requested else None
            with inference_server(command, self.directory / "server.log", self):
                try:
                    if profiling_requested:
                        self.http(
                            "/start_profile",
                            {
                                "output_dir": str(self.directory / "trace"),
                                "activities": ["CPU", "GPU"],
                                "with_stack": False,
                                "record_shapes": False,
                            },
                        )
                        profiling = True
                        gate.write(b"\1")
                    for ordinal, row in enumerate(requests):
                        if ordinal == self.formal_begin_index and self.plan["workload"].get(
                            "drain_before_formal", False
                        ):
                            self.wait_hicache_idle()
                        self.request(ordinal, row)
                    if profiling_requested:
                        self.http("/stop_profile", {})
                        profiling = False
                finally:
                    if gate is not None:
                        gate.seek(0)
                        gate.write(b"\0")
        if profiling:
            raise RuntimeError("Profiler did not finish exporting")
        storage = self.directory / "storage"
        temporary_storage_bytes = sum(path.stat().st_size for path in storage.rglob("*") if path.is_file())
        if profiling_requested or semantic:
            self.publish_manifest(temporary_storage_bytes)
        elif self.plan["calibration"] and self.plan["environment"].get("MARKOV_FILE_MEASUREMENTS"):
            files = sorted((self.directory / "file_workers").glob("*.jsonl"))
            if len(files) != self.plan["deployment"]["tensor_parallel"]:
                raise RuntimeError("Independent file observation did not report every TP worker")
            ports = set()
            for path in files:
                with path.open(encoding="utf-8") as stream:
                    ports.update(json.loads(line)["name"] for line in stream)
            write_json(
                self.directory / "file_measurements.json",
                {
                    "kind": "independent_file_worker_measurements",
                    "status": "completed",
                    "capture_origin": origin,
                    "profiling": False,
                    "temporary_storage_bytes": temporary_storage_bytes,
                    "deployment": self.plan["deployment"],
                    "configuration": self.plan["server"],
                    "warmup_batches": 1,
                    "formal_begin_index": self.plan["workload"]["formal_begin_index"],
                    "measurement_ports": sorted(ports),
                    "files": [str(path.relative_to(self.directory)) for path in files],
                },
            )
        elif self.plan["calibration"]:
            files = sorted((self.directory / "regions").glob("*.jsonl"))
            if len(files) != self.plan["deployment"]["tensor_parallel"]:
                raise RuntimeError("Independent region measurement did not report every TP worker")
            phases = set()
            measurement_ports = set()
            input_metadata_observed = True
            input_metadata_stream_observed = True
            load_consumer_observed = True
            for path in files:
                with path.open(encoding="utf-8") as stream:
                    roles = set()
                    for line in stream:
                        event = json.loads(line)
                        roles.add(event["name"])
                        measurement_ports.add(event["name"])
                        if event["name"] == "region.batch":
                            input_metadata_observed = input_metadata_observed and "input_metadata" in event["args"]
                        if event["name"] == "region.stream":
                            metadata = event["args"].get("input_metadata")
                            load_consumer_observed = load_consumer_observed and "load_consumer_active" in event["args"]
                            input_metadata_stream_observed = (
                                input_metadata_stream_observed
                                and metadata is not None
                                and "stream_interval_ns" in metadata
                            )
                        if any(
                            int(rid.removeprefix("request_")) >= self.plan["workload"]["formal_begin_index"]
                            for rid in event["args"]["request_ids"]
                        ):
                            phases.add(event["args"]["phase"])
                if (
                    not {"region.prepare", "region.prepare_stream", "region.batch", "region.stream", "region.result"}
                    <= roles
                ):
                    raise RuntimeError(f"Independent region ports are incomplete: {path}")

            if input_metadata_observed:
                measurement_ports.add("region.batch.input_metadata")
            if input_metadata_stream_observed:
                measurement_ports.add("region.stream.input_metadata")
            if load_consumer_observed:
                measurement_ports.add("region.batch.load_consumer")
            write_json(
                self.directory / "region_measurements.json",
                {
                    "kind": "independent_region_measurements",
                    "status": "completed",
                    "capture_origin": origin,
                    "profiling": False,
                    "temporary_storage_bytes": temporary_storage_bytes,
                    "deployment": self.plan["deployment"],
                    "configuration": self.plan["server"],
                    "formal_begin_index": self.plan["workload"]["formal_begin_index"],
                    "phases": sorted(phases),
                    "measurement_ports": sorted(measurement_ports),
                    "output_counts": [row["generated_tokens"] for row in self.rows if row["measure"]],
                    "files": [str(path.relative_to(self.directory)) for path in files],
                    "limitations": [
                        "Eager serial batch execution with the declared deployment and initialization; not a contention model.",
                        "Stream intervals include host submission starvation and internal communication; they are not exclusive GPU service.",
                        "CPU thread time excludes blocking waits; complete HTTP and frontend residuals are not fitted here.",
                        "Device events and lightweight wrappers have overhead; caller return and stream progress are overlapping responses.",
                    ],
                },
            )
        else:
            observed = any(
                self.plan["environment"].get(flag)
                for flag in ("MARKOV_REGION_MEASUREMENTS", "MARKOV_FILE_MEASUREMENTS", "SGLANG_STEP_TIMING_DIR")
            )
            write_json(
                self.directory / "normal_observation.json",
                {
                    "kind": "instrumented_http_observation" if observed else "normal_http_observation",
                    "status": "completed",
                    "profiling": False,
                    "framework": "sglang",
                    "temporary_storage_bytes": temporary_storage_bytes,
                    "config_path": "config.json",
                    "workload_report": "workload_report.json",
                },
            )

        # Runtime page files belong to this fresh session, not the replay input.
        # Retire them only after successful export; failed captures keep evidence.
        if storage.is_dir():
            shutil.rmtree(storage)

    def publish_manifest(self, temporary_storage_bytes: int) -> None:
        from markov.adapters.ascend.clock import trace_clock

        traces = sorted((self.directory / "trace").rglob("trace_view.json"))
        probes = sorted((self.directory / "probe").glob("*.jsonl"))
        native = sorted((self.directory / "native").glob("*.jsonl"))
        ranks = self.plan["deployment"]["tensor_parallel"]
        diagnostic = self.plan["profiling"]
        if len(probes) != ranks or (diagnostic and (len(traces) != ranks or len(native) != ranks)):
            raise RuntimeError(
                f"Capture channels do not match {ranks} ranks with profiler={diagnostic}: "
                f"{len(traces)} traces, {len(probes)} probes, {len(native)} native observations"
            )
        required = {
            "runtime.phase.batch_inputs",
            "runtime.phase.batch_input_start",
            "runtime.hicache.extend_allocator",
            "runtime.hicache.extend_allocation",
            "runtime.phase.forward_input_boundary",
            "runtime.phase.attention_metadata",
            "runtime.request.batch_run",
            "runtime.response.copy_to_cpu",
            "runtime.response.process_result",
        }
        ports = {name: Counter() for name in ("runtime.request.socket_received", "runtime.response.scheduler_send")}
        expected = {row["logical_request_id"] for row in self.rows if row["kind"] == "request"}
        anchors = {}
        for path in probes:
            roles = set()
            identities = set()
            with path.open() as stream:
                for line in stream:
                    event = json.loads(line)
                    if event["name"] == "runtime.clock_anchor":
                        rank = event["args"]["rank"]
                        if rank in anchors:
                            raise RuntimeError(f"Duplicate runtime clock anchor for rank {rank}")
                        anchors[rank] = event["args"]
                    if event["args"].get("status", "returned") == "returned":
                        roles.add(event["name"])
                        identities.update(event["args"].get("request_ids", []))
                        if event["name"] in ports:
                            ports[event["name"]].update(event["args"]["request_ids"])
            if required - roles:
                raise RuntimeError(f"Input observations are incomplete in {path.name}: {sorted(required - roles)}")
            if identities != expected:
                raise RuntimeError(f"Observed requests do not match the captured workload in {path.name}")
        for name, counts in ports.items():
            if counts != Counter({identity: 1 for identity in expected}):
                raise RuntimeError(f"Request transport ports are incomplete or ambiguous: {name}: {dict(counts)}")
        for path in native:
            roles = set()
            with path.open() as stream:
                for line in stream:
                    event = json.loads(line)
                    if event["args"]["return_code"] == 0:
                        roles.add(event["name"])
            if not {"AscendCL@aclrtRecordEvent", "AscendCL@aclrtStreamWaitEvent"} <= roles:
                raise RuntimeError(f"Native event observations are incomplete in {path.name}")
        files = [
            {"path": str(path.relative_to(self.directory)), "host_clock": trace_clock(path, anchors)} for path in traces
        ]
        region_files = sorted((self.directory / "regions").glob("*.jsonl")) if self.plan["record_regions"] else []
        write_json(
            self.directory / "profile_manifest.json",
            {
                "framework": "sglang",
                "temporary_storage_bytes": temporary_storage_bytes,
                "run_dir": str(self.directory),
                "config_path": "config.json",
                "status": "completed",
                "profiling_ready": diagnostic,
                "dry_run": False,
                "scope": "Raw trace, stream/events and eager compute; semantic, dependency and cost coverage require qualification"
                if diagnostic
                else "Semantic base observations and client windows; no native trace or detailed source replay qualification",
                "trace": {
                    "torch_trace_files": files,
                    "ld_preload_trace_files": [
                        {"path": str(path.relative_to(self.directory)), "format": "jsonl"} for path in native
                    ],
                },
                "sidecar": {
                    "python_probe_files": [
                        {"path": str(path.relative_to(self.directory)), "format": "jsonl"}
                        for path in probes + region_files
                    ]
                },
                "bench": {"workload_report_files": [{"path": "workload_report.json"}]},
            },
        )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("collect", "server"))
    parser.add_argument("--input", type=Path, required=True)
    args = parser.parse_args()
    if args.action == "collect":
        Capture(args.input).run()
        return

    from sglang.launch_server import run_server
    from sglang.srt.plugins import load_plugins
    from sglang.srt.server_args import ServerArgs
    from sglang.srt.utils import kill_process_tree

    load_plugins()
    try:
        run_server(ServerArgs(**read_json(args.input)["server"]))
    finally:
        kill_process_tree(os.getpid(), include_parent=False)


if __name__ == "__main__":
    main()
