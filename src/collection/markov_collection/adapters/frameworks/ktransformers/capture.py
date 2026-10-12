"""Observe the official Ascend BalanceServe engine without changing its scheduling loop."""

from __future__ import annotations

import faulthandler
import importlib.metadata
import json
import os
import queue
import signal
import sys
import time
import traceback
from collections.abc import Callable
from functools import partial
from pathlib import Path
from typing import Any

from ktransformers.server.backend.interfaces import balance_serve as native

from markov_collection.adapters.runtimes.pytorch.profiling import TorchProfilerMarkers
from markov_collection.artifacts.json_file import write_json
from markov_collection.instrumentation.recording import Recorder
from markov_collection.instrumentation.session import CaptureSession


class CaptureFinished(Exception):
    """Leave the native infinite loop after all declared completion messages."""


class KTransformersObserver:
    def __init__(self, engine: Any) -> None:
        self.engine = engine
        with Path(os.environ["MARKOV_PREPARED_INPUT"]).open(encoding="utf-8") as stream:
            self.prepared = json.load(stream)

        self.directory = Path(os.environ["MARKOV_PREPARED_INPUT"]).parent
        self.recorder: Recorder | None = None
        self.batch_sequence = 0
        self.batches: dict[int, dict[str, Any]] = {}
        self.current: dict[str, Any] | None = None
        self.published_batches: dict[int, dict[str, Any]] = {}
        self.finished: set[int] = set()

    def start(self) -> None:
        import torch_npu

        self.profiler = torch_npu.profiler.profile(
            activities=[torch_npu.profiler.ProfilerActivity.CPU, torch_npu.profiler.ProfilerActivity.NPU],
            record_shapes=True,
            with_stack=False,
            on_trace_ready=torch_npu.profiler.tensorboard_trace_handler(str(self.directory / "native_profile")),
            experimental_config=torch_npu.profiler._ExperimentalConfig(
                profiler_level=torch_npu.profiler.ProfilerLevel.Level1,
                data_simplification=False,
            ),
        )
        self.profiler.start()

        self.recorder = Recorder(Path(os.environ["MARKOV_OBSERVATIONS"]), 0, TorchProfilerMarkers())
        write_json(self.directory / "worker_ready.json", {"pid": os.getpid(), "rank": 0})

    def finish(self) -> None:
        self.recorder.close()
        self.profiler.stop()

        self.engine.gen_queue.original.close()
        self.engine.gen_queue.original.join_thread()

        write_json(self.directory / "worker_completed.json", {"native_request_ids": sorted(self.finished)})

    def batch(self, batch: Any) -> dict[str, Any]:
        prefill = {identity: (start, length) for identity, start, length in batch.prefill_mini_batches}
        decode = [identity for mini_batch in batch.decode_mini_batches for identity in mini_batch]
        requests = []
        for index, identity in enumerate(batch.query_ids):
            info = self.engine.query_manager.query_map.get(identity)
            start, length = prefill[identity] if identity in prefill else (info.active_position, 1)
            requests.append(
                {
                    "native_id": identity,
                    "phase": "prefill" if identity in prefill else "decode",
                    "input_tokens": length,
                    "context_tokens": start,
                    "prompt_tokens": batch.query_lengths[index],
                    "active_position": info.active_position if info is not None else start,
                    "allocated_tokens": batch.query_tokens[index].shape[0],
                    "kv_block_shape": list(batch.block_indexes[index].shape),
                }
            )

        return {
            "batch_id": self.batches[id(batch)]["batch_id"],
            "phase": "mixed" if prefill and decode else "prefill" if prefill else "decode",
            "native_requests": requests,
            "prefill_mini_batches": [list(item) for item in batch.prefill_mini_batches],
            "decode_mini_batches": batch.decode_mini_batches,
        }

    def schedule(self, original: Callable[..., Any], updates: list[Any]) -> Any:
        metadata = {
            "executing_batch_id": self.current["batch_id"] if self.engine.batch is not None else None,
            "updates": [
                {
                    "native_id": update.id,
                    "batch_id": self.published_batches[update.id]["batch_id"],
                    "active_position": update.active_position,
                    "finished": update.decode_done,
                }
                for update in updates
            ],
        }
        with self.recorder.scope("native_schedule", metadata) as observation:
            batch = original(updates)
            if batch.query_ids:
                self.batch_sequence += 1
                self.batches[id(batch)] = {"batch_id": self.batch_sequence}
                observation.result["batch_id"] = self.batch_sequence
                observation.result["native_request_ids"] = batch.query_ids
            else:
                observation.result["batch_id"] = None

        # Stop only after the native scheduler has absorbed terminal updates
        # and the engine has no in-flight batch. The last visible token alone
        # precedes this boundary in the official lookahead loop.
        if not batch.query_ids and self.engine.batch is None and len(self.finished) == len(self.prepared["requests"]):
            raise CaptureFinished

        return batch

    def prepare(self, original: Callable[..., Any], batch: Any) -> Any:
        with self.recorder.scope("native_prepare", self.batch(batch)):
            return original(batch)

    def execute(self, original: Callable[..., Any], batch: Any, query_manager: Any) -> Any:
        self.current = self.batch(batch)
        with self.recorder.scope("native_execute", self.current):
            return original(batch, query_manager)

    def forward(self, original: Callable[..., Any], *args: Any, **kwargs: Any) -> Any:
        if self.recorder is None:
            return original(*args, **kwargs)

        features = args[1]
        metadata = {
            **self.current,
            "phase": "prefill" if kwargs["is_prefill"] else "decode",
            "feature_shape": list(features.shape),
            "feature_stride": list(features.stride()),
            "feature_dtype": str(features.dtype),
            "feature_device": str(features.device),
        }
        with self.recorder.scope("native_model_forward", metadata):
            return original(*args, **kwargs)

    def replay(self, original: Callable[..., Any], graph_index: int = -1) -> Any:
        if self.recorder is None:
            return original(graph_index)

        metadata = {**self.current, "phase": "decode", "graph_index": graph_index}
        with self.recorder.scope("native_graph_replay", metadata):
            return original(graph_index)

    def complete(self, original: Callable[..., Any], *args: Any, **kwargs: Any) -> Any:
        if self.recorder is None:
            return original(*args, **kwargs)

        with self.recorder.scope("native_complete", self.current):
            return original(*args, **kwargs)

    def sample(self, original: Callable[..., Any], output: Any) -> Any:
        with self.recorder.scope("native_sample", self.current):
            return original(output)

    def update(self, original: Callable[..., Any], batch: Any) -> Any:
        with self.recorder.scope("native_update", self.current) as observation:
            updates = original(batch)
            observation.result["updates"] = [
                {"native_id": update.id, "active_position": update.active_position, "finished": update.decode_done}
                for update in updates
            ]

            for update in updates:
                self.published_batches[update.id] = self.current

            return updates


class ObservedQueue:
    def __init__(self, original: Any, observer: KTransformersObserver) -> None:
        self.original = original
        self.observer = observer

    def put(self, item: tuple[int, int | None], *, timeout: float) -> None:
        identity, token = item
        metadata = {**self.observer.published_batches[identity], "native_id": identity}
        with self.observer.recorder.scope("native_publish", metadata) as observation:
            self.original.put(item, timeout=timeout)
            observation.result["finished"] = token is None

        if token is None:
            self.observer.finished.add(identity)


class ObservedEngine(native.Engine):
    def __init__(self, *args: Any, **kwargs: Any) -> None:
        super().__init__(*args, **kwargs)
        self.observer = KTransformersObserver(self)

        # These bindings only bracket real calls; the official loop still owns
        # dispatch, lookahead, synchronization, sampling and publication.
        self.model_runner.run_split = partial(self.observer.execute, self.model_runner.run_split)
        self.model_runner.sync = partial(self.observer.complete, self.model_runner.sync)
        self.model_runner.replay = partial(self.observer.replay, self.model_runner.replay)
        self.model.forward = partial(self.observer.forward, self.model.forward)
        self.query_manager.add_query = partial(self.observer.prepare, self.query_manager.add_query)
        self.query_manager.update = partial(self.observer.update, self.query_manager.update)
        self.sched_client.update_last_batch = partial(self.observer.schedule, self.sched_client.update_last_batch)
        self.gen_queue = ObservedQueue(self.gen_queue, self.observer)

    def loop(self) -> None:
        self.observer.start()
        try:
            super().loop()
        except CaptureFinished:
            self.observer.finish()

    def sampling(self, output: Any) -> Any:
        return self.observer.sample(super().sampling, output)


def run_observed_engine(*args: Any, **kwargs: Any) -> None:
    # A top-level spawn target installs an image-local runtime wrapper. No source
    # file, native control rule or generated token is replaced.
    faulthandler.enable()
    native.Engine = ObservedEngine
    try:
        native.run_engine(*args, **kwargs)
    except Exception as error:
        traceback.print_exc()
        write_json(
            Path(os.environ["MARKOV_PREPARED_INPUT"]).parent / "worker_failure.json",
            {"error_type": type(error).__name__, "message": str(error)},
        )
        os.kill(os.getppid(), signal.SIGQUIT)
        raise
    finally:
        import torch.distributed as distributed

        if distributed.is_initialized():
            distributed.destroy_process_group()

    # Capture has flushed its queue and profiler at native quiescence. End this
    # worker explicitly; the serving engine has no finite-run interpreter
    # teardown path for its graph callbacks and CPU inference resources.
    sys.stdout.flush()
    sys.stderr.flush()
    os._exit(0)


class KTransformersCapture(CaptureSession):
    def _arguments(self) -> Any:
        from ktransformers.server.args import ArgumentParser
        from ktransformers.server.config.config import Config

        parameters = self.prepared["configuration"]
        if parameters["tp"] != 1 or parameters["backend_type"] != "balance_serve":
            raise ValueError("KTransformers capture currently requires Ascend BalanceServe TP=1")

        arguments = [
            "ktransformers",
            "--model_path",
            self.prepared["model_path"],
            "--model_name",
            self._model_configuration()["architectures"][0],
            "--device",
            "npu:0",
            "--log_dir",
            str(self.directory),
        ]
        for key, value in parameters.items():
            if isinstance(value, bool):
                arguments.append(f"--{key}" if value else f"--no-{key}")
            else:
                arguments.extend([f"--{key}", str(value)])

        original = sys.argv
        try:
            sys.argv = arguments
            self.native_configuration = Config()
            return ArgumentParser(self.native_configuration).parse_args()
        finally:
            sys.argv = original

    def run(self) -> None:
        os.environ.update(USE_MERGE="0", INF_NAN_MODE_FORCE_DISABLE="1", TASK_QUEUE_ENABLE="0", LOCAL_WORLD_SIZE="1")
        arguments = self._arguments()
        write_json(
            self.directory / "native_environment.json",
            {
                "framework": "ktransformers",
                "framework_version": importlib.metadata.version("ktransformers"),
                "configuration": self.prepared["configuration"],
                "model_configuration": self._model_configuration(),
                "effective_server_args": {
                    name: getattr(arguments, name)
                    for name in (
                        "model_dir",
                        "architectures",
                        "device",
                        "gguf_path",
                        "optimize_config_path",
                        "tp",
                        "cpu_infer",
                        "max_batch_size",
                        "chunk_size",
                        "page_size",
                        "cache_lens",
                        "gpu_memory_size",
                        "memory_gpu_only",
                        "cpu_memory_size_GB",
                        "use_cuda_graph",
                        "sched_strategy",
                        "sched_port",
                        "sched_metrics_port",
                        "kvc2_metrics_port",
                    )
                },
            },
        )

        signal.signal(signal.SIGQUIT, self._handle_worker_failure)
        native.run_engine = run_observed_engine
        try:
            self._run_interface(arguments)
        finally:
            # Constructor failures can leave a worker in native/process-pool
            # teardown. Stop our children before Python's implicit join.
            for process in native.mp.active_children():
                process.terminate()
                process.join()

    def _run_interface(self, arguments: Any) -> None:
        interface = native.BalanceServeInterface(self.native_configuration, arguments)
        while not (self.directory / "worker_ready.json").is_file():
            self._check_workers(interface)
            time.sleep(0.05)

        responses = self._execute(interface)
        for process in interface._engines:
            process.join()
        self._check_workers(interface)

        with (self.directory / "worker_completed.json").open(encoding="utf-8") as stream:
            completed = json.load(stream)
        if set(completed["native_request_ids"]) != {response["native_id"] for response in responses}:
            raise RuntimeError("native completion messages do not cover the declared requests")

    def _check_workers(self, interface: Any) -> None:
        for process in interface._engines:
            if process.exitcode is not None and process.exitcode != 0:
                raise RuntimeError(f"KTransformers native worker {process.pid} exited with {process.exitcode}")

    def _execute(self, interface: Any) -> list[dict[str, Any]]:
        from ktransformers.server.balance_serve.settings import sched_ext

        origin_ns = time.perf_counter_ns()
        requests = self.prepared["requests"]
        next_request = 0
        pending: dict[int, dict[str, Any]] = {}
        responses = []
        submitted = []

        while next_request < len(requests) or pending:
            while (
                next_request < len(requests)
                and origin_ns + requests[next_request]["arrival_time_ns"] <= time.perf_counter_ns()
            ):
                request = requests[next_request]
                query = sched_ext.QueryAdd()
                query.query_token = request["input_tokens"]
                query.query_length = len(request["input_tokens"])

                # QueryInfo stops at estimated_length - 2 and the terminal
                # decode emits None. Preserve that real extra decode operation.
                query.estimated_length = query.query_length + request["output_tokens"] + 2
                query.stop_criteria = []
                query.user_id = next_request
                query.sample_options = sched_ext.SampleOptions()
                query.sample_options.temperature = 0
                query.sample_options.top_p = 1

                response = {"id": request["id"], "submitted_ns": time.perf_counter_ns(), "output_tokens": []}
                identity = interface.sched_client.add_query(query)
                response["native_id"] = identity
                pending[identity] = response
                submitted.append(
                    {"id": request["id"], "native_id": identity, "estimated_length": query.estimated_length}
                )
                write_json(self.directory / "native_requests.json", submitted)
                next_request += 1

            timeout_sec = 0.1
            if next_request < len(requests):
                timeout_sec = max(
                    0,
                    min(
                        timeout_sec,
                        (origin_ns + requests[next_request]["arrival_time_ns"] - time.perf_counter_ns())
                        / 1_000_000_000,
                    ),
                )

            try:
                identity, token = interface.token_queue.get(timeout=timeout_sec)
            except queue.Empty:
                self._check_workers(interface)
                continue

            # Native lookahead can publish again after a query's first terminal
            # message. The native chat stream has already ended at that boundary;
            # retain those later operations in the observer, not in the response.
            if any(response["native_id"] == identity for response in responses):
                continue
            if token is not None:
                pending[identity]["output_tokens"].append(token)
            else:
                response = pending.pop(identity)
                response["returned_ns"] = time.perf_counter_ns()
                declared = next(request for request in requests if request["id"] == response["id"])
                if len(response["output_tokens"]) != declared["output_tokens"]:
                    raise RuntimeError(f"native output length differs for request {response['id']}")
                responses.append(response)
                write_json(self.directory / "responses.json", responses)

        return responses
