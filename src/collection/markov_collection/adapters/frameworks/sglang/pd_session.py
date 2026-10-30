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

from markov_workload import WorkloadContent

from markov_collection.adapters.frameworks.sglang.session import NativeServingSession
from markov_collection.artifacts import write_json


class NativeDisaggregatedSession:
    @staticmethod
    def instances(domain: dict[str, Any]) -> list[dict[str, Any]]:
        offset = 0
        instances = []
        for role, tp, pp, ep in domain["pd"]:
            NativeServingSession.source_roles(tp, pp, 0, ep)
            instances.append(
                {"role": role, "tp": tp, "pp": pp, "ep": ep, "offset": offset, "ranks": range(offset, offset + tp * pp)}
            )
            offset += tp * pp

        prefill, decode = instances
        if decode["pp"] not in (1, prefill["pp"]):
            raise ValueError("native PD requires Decode PP to equal Prefill PP or 1")
        return instances

    @staticmethod
    def structure_requests(page_size: int) -> tuple[tuple[int, int], ...]:
        requests = list(NativeServingSession.structure_requests(page_size))

        # Keep multi-member Decode and a request that finishes immediately after KV acceptance.
        for index in range(2):
            requests[index] = (requests[index][0], 16)
        requests[-1] = (requests[-1][0], 1)
        return tuple(requests)

    @staticmethod
    def structure_submissions(requests: Any) -> list[list[int]]:

        # Observe scalar Prefill separately from the multi-request serving path.
        groups = [[index] for index, (prompt, _) in enumerate(requests) if prompt == 1]
        bulk = [index for index, (prompt, _) in enumerate(requests) if prompt != 1]
        if bulk:
            groups.append(bulk)
        return groups

    def __init__(self, request: dict[str, Any]) -> None:
        self.request = request
        self.submissions = request["submissions"]
        if not all(self.submissions) or sorted(index for group in self.submissions for index in group) != list(
            range(len(request["requests"]))
        ):
            raise ValueError("PD submissions must contain every request exactly once")
        source = request.get("serving_input")
        if source is not None and [[item["prompt_tokens"], item["output_tokens"]] for item in source["requests"]] != [
            list(pair) for pair in request["requests"]
        ]:
            raise ValueError("injected workload must describe the submitted native requests")
        self.output = Path(request["output"])
        self.content_input = NativeServingSession.content_input(request["requests"], source)
        self.content = WorkloadContent(self.content_input["requests"], self.content_input["prefixes"])
        self.services = self.instances(request)
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

    def start(self, instance: dict[str, Any], index: int) -> None:
        role = instance["role"]
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
                "pp_size": instance["pp"],
                "ep_size": instance["ep"],
                "rank_offset": instance["offset"],
                "disaggregation_role": role,
                "observe_all_workers": self.request.get("observe_all_workers", False),
                "serving_input": self.request.get("serving_input"),
                "content": self.content_input,
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
            str(instance["tp"]),
            "--pp-size",
            str(instance["pp"]),
            "--base-gpu-id",
            str(instance["offset"]),
            "--host",
            "127.0.0.1",
            "--port",
            str(self.http_ports[index]),
            "--disaggregation-mode",
            role,
            "--disaggregation-transfer-backend",
            "ascend",
            "--disaggregation-bootstrap-port",
            str(self.bootstrap_port),
            "--disable-overlap-schedule",
            "--disable-cuda-graph",
            "--mem-fraction-static",
            "0.8",
            "--log-level",
            "warning",
        ]

        # Merge defaults before encoding CLI flags so Prefill can enable cache independently of Decode.
        configuration = {"chunked_prefill_size": -1, "disable_radix_cache": True, **self.request["configuration"][role]}
        for key, value in configuration.items():
            flag = "--" + key.replace("_", "-")
            if isinstance(value, bool):
                if value:
                    command.append(flag)
            else:
                command.extend([flag, str(value)])

        environment = dict(
            os.environ,
            MARKOV_GRAPH_SESSION=str(description),
            SGLANG_PLUGINS="markov",
            MARKOV_NATIVE_RUNTIME_DIR=str(directory / "runtime"),
            SGLANG_GRPC_PORT=str(self.grpc_ports[index]),
            ASCEND_MF_STORE_URL=f"tcp://127.0.0.1:{self.store_port}",
            ASCEND_MF_TRANSFER_PROTOCOL="sdma",
        )
        if instance["ep"] > 1:
            environment.update(DEEP_USE_ALLTOALL_MODE="1", DEEPEP_HCCL_BUFFSIZE="1024", HCCL_BUFFSIZE="1024")
            command.extend(["--ep-size", str(instance["ep"]), "--moe-a2a-backend", "deepep", "--deepep-mode", "normal"])
        log = directory / "server.log"
        with log.open("w") as stream:
            process = subprocess.Popen(command, env=environment, stdout=stream, stderr=subprocess.STDOUT)
        self.children.append(process)

        # This endpoint observes readiness; /health can itself start a generation request.
        while process.poll() is None:
            try:
                self.query(index, "/model_info", timeout=1)
                print(f"markov: native {role} ready", flush=True)
                return
            except (URLError, TimeoutError):
                pass
            time.sleep(0.2)
        raise RuntimeError(f"{role} server exited ({process.returncode}); see {log}")

    def query(self, device: int, path: str, payload: Any = None, *, timeout: int | None = 120) -> bytes:
        data = None if payload is None else json.dumps(payload).encode()
        request = Request(
            f"http://127.0.0.1:{self.http_ports[device]}{path}",
            data=data,
            headers={"Content-Type": "application/json"},
        )
        with urlopen(request, timeout=timeout) as response:
            return response.read()

    def stop_profiles(self, executor: ThreadPoolExecutor, devices: list[int]) -> None:

        # Both services flush raw observations before CPU analysis; the outer native budget bounds the operation.
        futures = [executor.submit(self.query, device, "/stop_profile", timeout=None) for device in devices]
        for future in futures:
            future.result()

        directories = []
        for device in devices:
            directory = self.output / self.services[device]["role"]
            plan = json.loads((directory / "role-plan.json").read_text())
            directories.extend(directory / "native" / f"rank-{rank}" for rank in plan["observed_roles"])

        # PP forwards control requests after replying locally; every selected recording must close before shutdown.
        while not all(directory.is_dir() for directory in directories):
            time.sleep(0.05)

    def generate(self, device: int, prefix: str, pairs: Any, rooms: list[int], indices: list[int]) -> list[Any]:
        if prefix == "warmup":
            source = NativeServingSession.content_input(pairs)
            content = WorkloadContent(source["requests"])
            identities = [request["id"] for request in source["requests"]]
        else:
            content = self.content
            identities = [self.content_input["requests"][index]["id"] for index in indices]
        payload = {
            "input_ids": [content.prompt(identity) for identity in identities],
            "sampling_params": [
                {"max_new_tokens": output, "temperature": 0, "ignore_eos": True} for _, output in pairs
            ],
            "rid": [f"{prefix}/{index}" for index in indices],
            "bootstrap_host": ["127.0.0.1"] * len(pairs),
            "bootstrap_port": [self.bootstrap_port] * len(pairs),
            "bootstrap_room": rooms,
            "stream": True,
        }
        body = self.query(device, "/generate", payload, timeout=None)
        return [
            json.loads(line[6:]) for line in body.splitlines() if line.startswith(b"data: ") and line != b"data: [DONE]"
        ]

    def run(self) -> None:
        started = time.perf_counter_ns()
        try:
            for index, instance in enumerate(self.services):
                self.start(instance, index)

            initialized = time.perf_counter_ns()
            warmup = NativeServingSession.warmup_requests(self.request["page_size"])
            requests = self.request["requests"]
            room = 1
            with ThreadPoolExecutor(max_workers=2) as executor, ExitStack() as profiles:
                started_profiles = []
                profiles.callback(self.stop_profiles, executor, started_profiles)
                for prefix, pairs in (("warmup", warmup), ("baseline", requests), ("capture", requests)):
                    groups = [list(range(len(pairs)))] if prefix == "warmup" else self.submissions
                    responses: dict[str, list[Any]] = {"prefill": [], "decode": []}
                    for indices in groups:
                        rooms = list(range(room, room + len(indices)))
                        room += len(indices)
                        futures = [
                            executor.submit(
                                self.generate, device, prefix, [pairs[index] for index in indices], rooms, indices
                            )
                            for device in range(2)
                        ]
                        for role, future in zip(responses, futures, strict=True):
                            responses[role].extend(future.result())
                    write_json(self.output / "responses" / f"{prefix}.json", responses, indent=2)
                    for role, rows in responses.items():
                        expected = {
                            f"{prefix}/{index}": 1 if role == "prefill" else output
                            for index, (_, output) in enumerate(pairs)
                        }
                        tokens = (
                            None
                            if prefix == "warmup"
                            else {
                                f"{prefix}/{index}": self.content.output_token(request["id"])
                                for index, request in enumerate(self.content_input["requests"])
                            }
                        )
                        NativeServingSession.check_responses(rows, expected, tokens)
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

                            # Each successfully started profiler is stopped even if later work fails.
                            started_profiles.append(device)

            plan = {
                "basis": "native_disaggregated_instance_roles",
                "participants": sum(len(instance["ranks"]) for instance in self.services),
                "representatives": [],
                "observed_roles": [],
                "instances": [],
            }
            for instance in self.services:
                directory = self.output / instance["role"]
                observed = json.loads((directory / "role-plan.json").read_text())
                plan["representatives"].extend(observed["representatives"])
                plan["observed_roles"].extend(observed["observed_roles"])
                for derived in observed["instances"]:
                    layout = directory / f"rank-{derived['environment']['rank']}-layout.json"
                    derived["environment"]["kv_layout"] = json.loads(layout.read_text())
                    layout.unlink()
                    plan["instances"].append(derived)
                for rank in observed["observed_roles"]:
                    (directory / f"rank-{rank}.json").stat()
            write_json(self.output / "role-plan.json", plan, indent=2)
            write_json(
                self.output / "session-summary.json",
                {
                    "initialization_ns": initialized - started,
                    "warmup_ns": warmed - initialized,
                    "warmup_requests": warmup,
                    "capture_and_flush_ns": time.perf_counter_ns() - warmed,
                    "model": self.request["model"],
                    "profile": True,
                    "weight_source": "native_dummy_loader_full_model",
                    "configuration": self.request["configuration"],
                    "transport": "ascend_sdma",
                    "native_participants": [rank for instance in self.services for rank in instance["ranks"]],
                    "observed_roles": plan["observed_roles"],
                    "routing_source": "workload"
                    if (self.request.get("serving_input") or {}).get("ep_routes")
                    else "native",
                    "content_source": "declared_prefixes_and_private_continuations",
                    "instances": [
                        {key: value for key, value in instance.items() if key != "ranks"} for instance in self.services
                    ],
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
