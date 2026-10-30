"""Capture a bounded dense KTransformers workload through the common HTTP contract."""

from __future__ import annotations

import argparse
from collections import Counter
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
from pathlib import Path
import sys
import time
import traceback

from markov.artifacts.request import Deployment, positive_integer, read_json, reject_unknown
from markov.artifacts.writing import write_json
from markov.instrumentation.events import EventWriter
from markov.instrumentation.http import SerialHttpClient, encode_inputs, inference_server


def server_arguments(deployment: Deployment, configuration: dict) -> dict:
    reject_unknown(
        configuration,
        {"attention_backend", "cpu_threads", "max_context_tokens", "prefill_chunk_tokens"},
        "KTransformers configuration",
    )
    if deployment.tensor_parallel != 1 or deployment.dtype not in ("float16", "bfloat16"):
        raise ValueError("The dense KTransformers adapter currently supports one NPU and float16/bfloat16")
    attention = configuration.get("attention_backend", "eager")
    if attention not in ("eager", "sdpa"):
        raise ValueError("KTransformers attention_backend must be eager or sdpa")
    chunk = configuration.get("prefill_chunk_tokens")
    return {
        "model_path": "/model",
        "dtype": deployment.dtype,
        "attention_backend": attention,
        "cpu_threads": positive_integer(configuration.get("cpu_threads", 2), "cpu_threads"),
        "max_context_tokens": positive_integer(configuration["max_context_tokens"], "max_context_tokens"),
        "prefill_chunk_tokens": None if chunk is None else positive_integer(chunk, "prefill_chunk_tokens"),
    }


def capture_environment(profiling: bool, calibration: bool = False) -> dict[str, str]:
    return {"LD_PRELOAD": "", "TORCH_DEVICE_BACKEND_AUTOLOAD": "0", "TOKENIZERS_PARALLELISM": "false"}


class InferenceServer(HTTPServer):
    def __init__(self, path: Path) -> None:
        from markov.adapters.ktransformers.inference import Inference

        self.plan = read_json(path)
        self.directory = path.parent
        self.engine = Inference(self.plan["server"])
        self.profiler = None
        self.anchor = None
        self.engine_info = {
            "framework": "ktransformers",
            "model_type": self.engine.model.config.model_type,
            "layers": self.engine.model.config.num_hidden_layers,
            "native_linears": self.engine.native_linears,
            "linear_implementation": "KTransformersLinear:KLinearTorch",
            "execution": "synchronous eager phases with fresh per-request KV",
        }
        if self.plan["calibration"]:
            self.engine.writer = EventWriter(self.directory / "regions")
        super().__init__(("127.0.0.1", 30000), InferenceHandler)

    def start_profile(self) -> None:
        import torch_npu
        from markov.adapters.ascend.clock import sample_clock

        self.engine.writer = EventWriter(self.directory / "probe")
        self.anchor = sample_clock(rank=-1)
        self.profiler = torch_npu.profiler.profile(
            activities=[torch_npu.profiler.ProfilerActivity.CPU, torch_npu.profiler.ProfilerActivity.NPU],
            record_shapes=False,
            profile_memory=False,
            with_stack=False,
            schedule=torch_npu.profiler.schedule(
                wait=0, warmup=0, active=len(self.plan["workload"]["requests"]), repeat=1
            ),
            on_trace_ready=torch_npu.profiler.tensorboard_trace_handler(str(self.directory / "trace")),
            experimental_config=torch_npu.profiler._ExperimentalConfig(
                profiler_level=torch_npu.profiler.ProfilerLevel.Level1
            ),
        )
        self.profiler.start()

    def stop_profile(self) -> None:
        self.profiler.stop()
        write_json(self.directory / "clock_anchor.json", self.anchor)


class InferenceHandler(BaseHTTPRequestHandler):
    server: InferenceServer

    def log_message(self, format: str, *args: object) -> None:
        pass

    def reply(self, value: dict) -> None:
        payload = json.dumps(value).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def do_GET(self) -> None:
        if self.path == "/model_info":
            self.reply(self.server.engine_info)
        else:
            self.send_error(404)

    def do_POST(self) -> None:
        try:
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            if self.path == "/start_profile":
                self.server.start_profile()
                self.reply({"status": "started"})
            elif self.path == "/stop_profile":
                self.server.stop_profile()
                self.reply({"status": "exported"})
            elif self.path == "/generate":
                engine = self.server.engine
                fields = {"request_ids": [body["rid"]]}
                if self.server.plan["profiling"]:
                    stamp = time.time_ns()
                    received_ns = stamp
                params = body["sampling_params"]
                output = engine.generate(body["rid"], body["input_ids"], params["max_new_tokens"], params["ignore_eos"])
                value = {
                    "output_ids": output,
                    "meta_info": {
                        "completion_tokens": len(output),
                        "finish_reason": {"type": "length" if len(output) == params["max_new_tokens"] else "stop"},
                    },
                }
                if self.server.plan["profiling"]:
                    sent_ns = time.time_ns()
                self.reply(value)
                if engine.writer is not None:
                    engine.flush()
                if self.server.plan["profiling"]:
                    engine.writer.write("runtime.request.socket_received", received_ns, received_ns, fields)
                    engine.writer.write("runtime.response.scheduler_send", sent_ns, sent_ns, fields)
                if self.server.plan["profiling"]:
                    self.server.profiler.step()
            else:
                self.send_error(404)
        except Exception:
            traceback.print_exc()
            self.send_error(500, "Inference failed; see server.log")


class Capture(SerialHttpClient):
    def __init__(self, path: Path) -> None:
        self.path = path
        self.plan = read_json(path)
        super().__init__(
            path.parent, self.plan["workload"]["formal_begin_index"], time.monotonic() + self.plan["budget"]["wall_sec"]
        )

    def run(self) -> None:
        rows = self.plan["workload"]["requests"]
        if any(row["return_logprob"] for row in rows):
            raise ValueError("The dense KTransformers adapter does not yet collect logprob output")
        encode_inputs(rows, self.plan["server"]["model_path"], self.plan["budget"]["input_tokens"])
        if self.plan["profiling"] or self.plan["calibration"]:
            (self.directory / ("probe" if self.plan["profiling"] else "regions")).mkdir()
        if self.plan["profiling"]:
            (self.directory / "trace").mkdir()
        command = [sys.executable, "-m", "markov.adapters.ktransformers.capture", "server", "--input", str(self.path)]
        write_json(
            self.directory / "config.json",
            {
                "deployment": self.plan["deployment"],
                "workload": self.plan["workload"],
                "server": {"arguments": self.plan["server"]},
            },
        )

        with inference_server(command, self.directory / "server.log", self) as info:
            write_json(self.directory / "engine.json", json.loads(info))
            if self.plan["profiling"]:
                self.http("/start_profile", {})
            for index, row in enumerate(rows):
                self.request(index, row)
            if self.plan["profiling"]:
                self.http("/stop_profile", {})

        if self.plan["profiling"]:
            self.publish_manifest()
        elif self.plan["calibration"]:
            files = sorted((self.directory / "regions").glob("*.jsonl"))
            responses = set()
            for path in files:
                with path.open(encoding="utf-8") as stream:
                    for line in stream:
                        event = json.loads(line)
                        fields = event["args"]
                        if event["name"] == "runtime.ktransformers.phase":
                            responses.add((fields["phase"], fields["emit_token"]))
                        elif event["name"] == "runtime.ktransformers.generate":
                            responses.add(("control", False))
            write_json(
                self.directory / "region_measurements.json",
                {
                    "kind": "independent_ktransformers_regions",
                    "status": "completed",
                    "profiling": False,
                    "deployment": self.plan["deployment"],
                    "configuration": self.plan["server"],
                    "formal_begin_index": self.formal_begin_index,
                    "files": [str(p.relative_to(self.directory)) for p in files],
                    "responses": [{"phase": phase, "emit_token": emit} for phase, emit in sorted(responses)],
                    "limitations": [
                        "Synchronous eager dense Torch operators on one otherwise idle NPU; no serving overlap or contention model.",
                        "Whole phase includes forward, optional greedy token selection and completion observation; not bare device service.",
                        "First-phase status aggregates first use and does not identify all compiler/cache initialization.",
                        "Light clock observation remains included; no profiler or snapshots are used.",
                    ],
                },
            )
        else:
            write_json(
                self.directory / "normal_observation.json",
                {
                    "kind": "normal_http_observation",
                    "status": "completed",
                    "profiling": False,
                    "framework": "ktransformers",
                    "config_path": "config.json",
                    "workload_report": "workload_report.json",
                },
            )

    def publish_manifest(self) -> None:
        from markov.adapters.ascend.clock import trace_clock

        anchor = read_json(self.directory / "clock_anchor.json")
        traces = sorted((self.directory / "trace").rglob("trace_view.json"))
        if len(traces) != 1:
            raise ValueError("Single-device KTransformers profiling requires one completed device trace")
        probes = sorted((self.directory / "probe").glob("*.jsonl"))
        if len(probes) != 1:
            raise ValueError("Single-device KTransformers profiling requires one phase observation stream")
        ports = {name: Counter() for name in ("runtime.request.socket_received", "runtime.response.scheduler_send")}
        phases = Counter()
        generations = Counter()
        with probes[0].open(encoding="utf-8") as stream:
            for line in stream:
                event = json.loads(line)
                if event["name"] in ports:
                    ports[event["name"]].update(event["args"]["request_ids"])
                elif event["name"] == "runtime.ktransformers.phase":
                    phases.update(event["args"]["request_ids"])
                elif event["name"] == "runtime.ktransformers.generate":
                    generations.update(event["args"]["request_ids"])
        expected = Counter({row["logical_request_id"]: 1 for row in self.rows})
        chunk = self.plan["server"]["prefill_chunk_tokens"]
        expected_phases = {
            row["logical_request_id"]: (row["input_tokens"] + chunk - 1) // chunk + row["generated_tokens"] - 1
            if chunk is not None
            else row["generated_tokens"]
            for row in self.rows
        }
        if any(counts != expected for counts in ports.values()) or phases != expected_phases or generations != expected:
            raise ValueError("KTransformers phase observations do not cover the completed HTTP workload")
        write_json(
            self.directory / "profile_manifest.json",
            {
                "framework": "ktransformers",
                "run_dir": str(self.directory),
                "config_path": "config.json",
                "status": "completed",
                "profiling_ready": True,
                "dry_run": False,
                "scope": "Dense synchronous KTransformers phases and raw device trace; causality and cost coverage require separate qualification",
                "trace": {
                    "torch_trace_files": [
                        {
                            "path": str(p.relative_to(self.directory)),
                            "host_clock": trace_clock(p, {-1: anchor}, rank=-1),
                        }
                        for p in traces
                    ],
                    "ld_preload_trace_files": [],
                },
                "sidecar": {
                    "python_probe_files": [
                        {"path": str(p.relative_to(self.directory)), "format": "jsonl"}
                        for p in sorted((self.directory / "probe").glob("*.jsonl"))
                    ]
                },
                "bench": {"workload_report_files": [{"path": "workload_report.json"}]},
            },
        )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("collect", "server"))
    parser.add_argument("--input", type=Path, required=True)
    args = parser.parse_args()
    if args.action == "collect":
        Capture(args.input).run()
    else:
        with InferenceServer(args.input) as server:
            server.serve_forever()


if __name__ == "__main__":
    main()
