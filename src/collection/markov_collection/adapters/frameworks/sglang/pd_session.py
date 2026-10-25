"""Run two native PD services; SGLang owns requests, pools and transfer state."""

from __future__ import annotations

import json
import os
import socket
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from contextlib import ExitStack
from pathlib import Path
from typing import Any
from urllib.error import URLError
from urllib.request import Request, urlopen

from markov_collection.adapters.frameworks.sglang.session import NativeServingSession
from markov_collection.artifacts.json_file import write_json


class NativeDisaggregatedSession:
    @staticmethod
    def structure_requests(page_size: int) -> tuple[tuple[int, int], ...]:

        # Retain two requests long enough to observe both single- and multi-member Decode semantics.
        return tuple(
            (prompt, 16 if index < 2 else output)
            for index, (prompt, output) in enumerate(NativeServingSession.warmup_requests(page_size))
        )

    def __init__(self, request: dict[str, Any]) -> None:
        self.request = request
        self.output = Path(request["output"])
        self.children: list[subprocess.Popen[bytes]] = []

        # The upstream gRPC default is HTTP + 10000; allocate explicit independent ports instead.
        with ExitStack() as stack:
            listeners = [stack.enter_context(socket.socket()) for _ in range(6)]
            for listener in listeners:
                listener.bind(("127.0.0.1", 0))
            self.http_ports = [listener.getsockname()[1] for listener in listeners[:2]]
            self.grpc_ports = [listener.getsockname()[1] for listener in listeners[2:4]]
            self.bootstrap_port = listeners[4].getsockname()[1]
            self.store_port = listeners[5].getsockname()[1]

    def start(self, role: str, device: int) -> None:
        directory = self.output / role
        directory.mkdir(parents=True)
        (directory / "runtime").mkdir()
        description = directory / "session.json"
        write_json(
            description,
            {
                "output": str(directory),
                "source_requests": [f"capture/{index}" for index in range(len(self.request["requests"]))],
                "baseline_requests": [f"baseline/{index}" for index in range(len(self.request["requests"]))],
                "matrix_selection": self.request["matrix_selection"],
                "native_library": self.request["native_library"],
                "pp_size": 1,
                "ep_size": 1,
                "disaggregation_role": role,
            },
        )

        command = [
            sys.executable,
            "-m",
            "sglang.launch_server",
            "--model-path",
            self.request["model"],
            "--device",
            "npu",
            "--dtype",
            self.request["dtype"],
            "--load-format",
            "dummy",
            "--skip-tokenizer-init",
            "--skip-server-warmup",
            "--attention-backend",
            self.request["attention_backend"],
            "--tp-size",
            "1",
            "--base-gpu-id",
            str(device),
            "--host",
            "127.0.0.1",
            "--port",
            str(self.http_ports[device]),
            "--disaggregation-mode",
            role,
            "--disaggregation-transfer-backend",
            "ascend",
            "--disaggregation-bootstrap-port",
            str(self.bootstrap_port),
            "--chunked-prefill-size",
            "-1",
            "--disable-radix-cache",
            "--disable-overlap-schedule",
            "--disable-cuda-graph",
            "--disaggregation-decode-extra-slots",
            "0",
            "--mem-fraction-static",
            "0.8",
            "--log-level",
            "warning",
        ]
        for key, value in self.request["configuration"].items():
            command.extend(["--" + key.replace("_", "-"), str(value)])

        environment = dict(
            os.environ,
            MARKOV_GRAPH_SESSION=str(description),
            SGLANG_PLUGINS="markov",
            MARKOV_NATIVE_RUNTIME_DIR=str(directory / "runtime"),
            SGLANG_GRPC_PORT=str(self.grpc_ports[device]),
            ASCEND_MF_STORE_URL=f"tcp://127.0.0.1:{self.store_port}",
            ASCEND_MF_TRANSFER_PROTOCOL="sdma",
        )
        log = directory / "server.log"
        with log.open("w") as stream:
            process = subprocess.Popen(command, env=environment, stdout=stream, stderr=subprocess.STDOUT)
        self.children.append(process)

        # This endpoint observes readiness; /health can itself start a generation request.
        started = time.monotonic()
        while process.poll() is None:
            try:
                self.query(device, "/model_info", timeout=1)
                print(f"markov: native {role} ready", flush=True)
                return
            except (URLError, TimeoutError):
                pass
            if time.monotonic() - started > 100:
                raise TimeoutError(f"{role} server startup timed out")
            time.sleep(0.2)
        raise RuntimeError(f"{role} server exited ({process.returncode}); see {log}")

    def query(self, device: int, path: str, payload: Any = None, *, timeout: int = 120) -> bytes:
        data = None if payload is None else json.dumps(payload).encode()
        request = Request(
            f"http://127.0.0.1:{self.http_ports[device]}{path}",
            data=data,
            headers={"Content-Type": "application/json"},
        )
        with urlopen(request, timeout=timeout) as response:
            return response.read()

    def generate(self, device: int, prefix: str, pairs: Any, rooms: list[int]) -> list[Any]:
        payload = {
            "input_ids": [[1] * prompt for prompt, _ in pairs],
            "sampling_params": [
                {"max_new_tokens": output, "temperature": 0, "ignore_eos": True} for _, output in pairs
            ],
            "rid": [f"{prefix}/{index}" for index in range(len(pairs))],
            "bootstrap_host": ["127.0.0.1"] * len(pairs),
            "bootstrap_port": [self.bootstrap_port] * len(pairs),
            "bootstrap_room": rooms,
            "stream": True,
        }
        body = self.query(device, "/generate", payload)
        return [
            json.loads(line[6:]) for line in body.splitlines() if line.startswith(b"data: ") and line != b"data: [DONE]"
        ]

    def run(self) -> None:
        started = time.perf_counter_ns()
        try:
            self.start("prefill", 0)
            self.start("decode", 1)
            initialized = time.perf_counter_ns()
            warmup = NativeServingSession.warmup_requests(self.request["configuration"]["page_size"])
            requests = self.request["requests"]
            room = 1
            with ExitStack() as profiles, ThreadPoolExecutor(max_workers=2) as executor:
                for prefix, pairs in (("warmup", warmup), ("baseline", requests), ("capture", requests)):
                    rooms = list(range(room, room + len(pairs)))
                    room += len(pairs)
                    futures = [executor.submit(self.generate, device, prefix, pairs, rooms) for device in range(2)]
                    responses = {
                        role: future.result() for role, future in zip(("prefill", "decode"), futures, strict=True)
                    }
                    write_json(self.output / "responses" / f"{prefix}.json", responses, indent=2)
                    print(f"markov: native PD {prefix} complete", flush=True)

                    # Both native replies complete before reset; the observation keeps warmed allocator state.
                    for device in range(2):
                        self.query(device, "/flush_cache?timeout=30")
                    if prefix == "warmup":
                        warmed = time.perf_counter_ns()
                        for device, role in enumerate(("prefill", "decode")):
                            self.query(
                                device,
                                "/start_profile",
                                {
                                    "output_dir": str(self.output / role / "native"),
                                    "activities": ["CPU", "GPU"],
                                    "record_shapes": False,
                                    "with_stack": False,
                                },
                            )

                            # Each started profiler must export even when a later request or export fails.
                            profiles.callback(self.query, device, "/stop_profile")

            for rank, role in enumerate(("prefill", "decode")):
                (self.output / role / f"rank-{rank}.json").stat()
            write_json(
                self.output / "session-summary.json",
                {
                    "initialization_ns": initialized - started,
                    "warmup_ns": warmed - initialized,
                    "warmup_requests": warmup,
                    "capture_and_export_ns": time.perf_counter_ns() - warmed,
                    "model": self.request["model"],
                    "profile": True,
                    "weight_source": "native_dummy_loader_full_model",
                    "configuration": self.request["configuration"],
                    "transport": "ascend_sdma",
                    "instances": [{"role": "prefill", "rank": 0}, {"role": "decode", "rank": 1}],
                },
                indent=2,
            )
        finally:
            for process in reversed(self.children):
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()

    @staticmethod
    def interpret(request: dict[str, Any]) -> None:
        from markov_collection.adapters.frameworks.sglang.acquisition import SGLangAcquisition

        directory = Path(request["output"])
        for rank, role in enumerate(("prefill", "decode")):
            SGLangAcquisition.interpret_role(directory / role, rank, 1, True)
