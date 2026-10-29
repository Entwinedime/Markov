"""Collect a bounded serial HTTP workload inside its declared inference container."""

import argparse
from collections import Counter
import os
from pathlib import Path
import signal
import subprocess
import sys
import time
import urllib.error
import urllib.request
import json

from markov.artifacts.request import read_json
from markov.artifacts.writing import write_json


def server_arguments(deployment, configuration):
    reserved = {"model_path", "dtype", "tp_size", "host", "port", "skip_server_warmup", "base_gpu_id", "gpu_id_step"}
    if reserved.intersection(configuration):
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


def capture_environment():
    return {
        "SGLANG_NPU_PROFILER_SERIAL_EXPORT": "1",
        "SGLANG_NPU_PROFILER_LEVEL": "2",
        "TRITON_ENABLE_TASKQUEUE": "true",
        "MARKOV_INPUT_FACTS": "/capture/probe",
        "SGLANG_ENABLE_HEALTH_ENDPOINT_GENERATION": "0",
    }


class Capture:
    def __init__(self, path: Path):
        self.path = path
        self.directory = path.parent
        self.plan = read_json(path)
        self.deadline = time.monotonic() + self.plan["budget"]["wall_sec"]
        self.client = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        self.rows = []

    def remaining(self):
        value = self.deadline - time.monotonic()
        if value <= 0:
            raise TimeoutError("Capture wall-time budget reached")
        return value

    def http(self, route: str, body=None):
        data = None if body is None else json.dumps(body).encode()
        request = urllib.request.Request(
            "http://127.0.0.1:30000" + route, data=data, headers={"Content-Type": "application/json"}
        )
        begin = time.time_ns()
        with self.client.open(request, timeout=self.remaining()) as response:
            payload = response.read()
        end = time.time_ns()
        return payload, begin, end

    def prepare_inputs(self):
        requests = self.plan["workload"]["requests"]
        tokenizer = None
        if any(row["prompt"] is not None for row in requests):
            from transformers import AutoTokenizer

            tokenizer = AutoTokenizer.from_pretrained(
                self.plan["server"]["model_path"],
                local_files_only=True,
                trust_remote_code=self.plan["server"].get("trust_remote_code", False),
            )
        for row in requests:
            if row["input_ids"] is None:
                row["input_ids"] = tokenizer.encode(row["prompt"])
        if sum(len(row["input_ids"]) for row in requests) > self.plan["budget"]["input_tokens"]:
            raise ValueError("The tokenized workload exceeds the remaining input-token budget")
        return requests

    def run(self):
        requests = self.prepare_inputs()
        (self.directory / "probe").mkdir()
        (self.directory / "trace").mkdir()
        command = [sys.executable, "-m", "markov.adapters.sglang.capture", "server", "--input", str(self.path)]
        write_json(
            self.directory / "config.json",
            {"deployment": self.plan["deployment"], "server": {"command": command, "arguments": self.plan["server"]}},
        )
        profiling = False
        with (self.directory / "server.log").open("w") as log:
            server = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            try:
                while True:
                    if server.poll() is not None:
                        raise RuntimeError(f"Inference server exited with {server.returncode}; see server.log")
                    try:
                        self.http("/model_info")
                        break
                    except (urllib.error.URLError, TimeoutError):
                        self.remaining()
                        time.sleep(min(0.5, self.remaining()))
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
                for ordinal, row in enumerate(requests):
                    self.request(ordinal, row)
                self.http("/stop_profile", {})
                profiling = False
            finally:
                # Export finishes before shutdown on success; failed captures
                # retain their partial files without publishing a ready manifest.
                if server.poll() is None:
                    server.terminate()
                    try:
                        server.wait(timeout=15)
                    except subprocess.TimeoutExpired:
                        os.killpg(server.pid, signal.SIGKILL)
                        server.wait(timeout=15)
        if profiling:
            raise RuntimeError("Profiler did not finish exporting")
        self.publish_manifest()

    def request(self, ordinal, item):
        identity = f"request_{ordinal}"
        body = {
            "rid": identity,
            "input_ids": item["input_ids"],
            "return_logprob": item["return_logprob"],
            "sampling_params": {
                "temperature": 0,
                "max_new_tokens": item["max_new_tokens"],
                "ignore_eos": item["ignore_eos"],
            },
        }
        row = {
            "kind": "request",
            "logical_request_id": identity,
            "measure": ordinal >= self.plan["workload"]["formal_begin_index"],
            "input_tokens": len(item["input_ids"]),
            "max_new_tokens": item["max_new_tokens"],
            "status": "running",
        }
        self.rows.append(row)
        write_json(self.directory / "workload_report.json", {"requests": self.rows})
        try:
            payload, begin, end = self.http("/generate", body)
            response = json.loads(payload)
        except Exception as error:
            row.update(status="error", error=str(error))
            write_json(self.directory / "workload_report.json", {"requests": self.rows})
            raise
        row.update(
            status="ok",
            start_time_ms=self.milliseconds(begin),
            end_time_ms=self.milliseconds(end),
            generated_tokens=response["meta_info"]["completion_tokens"],
            response=response,
        )
        write_json(self.directory / "workload_report.json", {"requests": self.rows})

    @staticmethod
    def milliseconds(ns):
        return f"{ns // 1_000_000}.{ns % 1_000_000:06d}"

    def publish_manifest(self):
        from markov.adapters.ascend.clock import trace_clock

        traces = sorted((self.directory / "trace").rglob("trace_view.json"))
        probes = sorted((self.directory / "probe").glob("*.jsonl"))
        ranks = self.plan["deployment"]["tensor_parallel"]
        if len(traces) != ranks or len(probes) != ranks:
            raise RuntimeError(f"Expected {ranks} worker traces and probes; received {len(traces)} and {len(probes)}")
        required = {
            "runtime.phase.batch_inputs",
            "runtime.phase.batch_input_start",
            "runtime.hicache.extend_allocator",
            "runtime.hicache.extend_allocation",
            "runtime.phase.forward_input_boundary",
            "runtime.phase.attention_metadata",
        }
        ports = {name: Counter() for name in ("runtime.request.socket_received", "runtime.response.scheduler_send")}
        expected = {row["logical_request_id"] for row in self.rows}
        for path in probes:
            roles = set()
            identities = set()
            with path.open() as stream:
                for line in stream:
                    event = json.loads(line)
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
        files = [{"path": str(path.relative_to(self.directory)), "host_clock": trace_clock(path)} for path in traces]
        write_json(
            self.directory / "profile_manifest.json",
            {
                "framework": "sglang",
                "run_dir": str(self.directory),
                "config_path": "config.json",
                "status": "completed",
                "profiling_ready": True,
                "dry_run": False,
                "scope": "Raw CPU/device trace and input preparation; remaining semantic and native-hook coverage requires qualification",
                "trace": {"torch_trace_files": files, "ld_preload_trace_files": []},
                "sidecar": {
                    "python_probe_files": [
                        {"path": str(path.relative_to(self.directory)), "format": "jsonl"} for path in probes
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
