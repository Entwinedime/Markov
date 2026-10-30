"""Observe native process groups and isolated logical TP shards through explicit ports."""

from __future__ import annotations

import inspect
from functools import partial
from pathlib import Path
from typing import Any

from markov_collection.adapters.runtimes.pytorch.factories import TensorGeometryObservation
from markov_collection.instrumentation.calls import NativeCall


class CollectiveCall(NativeCall):
    @staticmethod
    def gather_view_condition(shape: list[Any], size: int, dimension: int) -> list[Any]:
        axis = dimension + len(shape) if dimension < 0 else dimension
        if not 0 <= axis < len(shape):
            raise ValueError("all-gather axis is outside its input rank")

        # The moved TP axis can merge with the gather axis when their strides agree or either axis is singleton.
        return [
            "or",
            ["eq", size, 1],
            ["eq", ["mul", 1, *shape[:axis]], 1],
            ["eq", shape[axis], 1],
            ["eq", ["mul", 1, *shape], 0],
        ]

    @staticmethod
    def declare_gather_shape(metadata: dict[str, Any], values: list[dict[str, Any]]) -> list[Any] | None:
        source, output = metadata["inputs"][0], metadata["outputs"][0]
        shape = values[source]["shape_relation"]
        if shape is None:
            return None

        # B and padded B are caller expressions; the same native API consumes local tensor extents.
        axes = {f"collective_dim_{axis}": axis for axis, size in enumerate(shape) if not isinstance(size, int)}
        metadata.setdefault("parameters", {}).update(
            {name: values[source]["shape"][axis] for name, axis in axes.items()}
        )
        metadata.setdefault("call_parameters", {}).update({name: shape[axis] for name, axis in axes.items()})
        local = list(shape)
        for name, axis in axes.items():
            local[axis] = name
        result = list(local)
        spec = metadata["communication"]
        axis = spec["dimension"]
        result[axis] = ["mul", spec["size"], result[axis]]
        metadata["shape_contracts"] = [
            item for item in metadata.get("shape_contracts", []) if item["value"] not in (source, output)
        ] + [{"value": source, "shape": local}, {"value": output, "shape": result}]
        return local

    @staticmethod
    def bind_transports(
        regions: list[dict[str, Any]], operations: list[dict[str, Any]], values: list[dict[str, Any]]
    ) -> None:
        calls: dict[int, list[dict[str, Any]]] = {}
        native = {
            "all_reduce": "c10d.allreduce_.default",
            "all_gather": "c10d._allgather_base_.default",
            "reduce_scatter": "c10d._reduce_scatter_base_.default",
        }
        for operation in operations:
            if operation["name"] in native.values():
                calls.setdefault(operation["region"], []).append(operation)

        for region in regions:
            metadata = region["metadata"]
            if metadata.get("interface") != "collective":
                continue

            primitive = metadata["communication"]["primitive"]
            (transport,) = [operation for operation in calls[region["id"]] if operation["name"] == native[primitive]]

            # Mark the actual backend call; all-gather's later layout work remains local device work.
            transport["communication"] = True
            if primitive == "all_gather" and not metadata["communication"].get("buffered_output"):
                shape = CollectiveCall.declare_gather_shape(metadata, values)
                if shape is None:
                    continue

                spec = metadata["communication"]
                output = values[metadata["outputs"][0]]
                buffer = values[transport["inputs"][0]]
                metadata["execution_guards"] = [
                    {
                        "condition": CollectiveCall.gather_view_condition(shape, spec["size"], spec["dimension"]),
                        "expected": output["storage"] == buffer["storage"],
                    }
                ]

    def __init__(self, values: Any, size: int, kind: str, original: Any) -> None:
        self.buffered = kind in {"all_gather_into_tensor", "reduce_scatter_tensor"}
        super().__init__(
            values.recording,
            values,
            f"sglang.NpuCommunicator.{kind}",
            original,
            {},
            interface="collective",
            mutations=(0,) if self.buffered or kind == "all_reduce" else (),
        )
        self.size = size
        self.kind = {"all_gather_into_tensor": "all_gather", "reduce_scatter_tensor": "reduce_scatter"}.get(kind, kind)

    def invoke(self, *args: Any, **kwargs: Any) -> Any:
        if self.buffered:
            self.output = args[0]
        return super().invoke(*args, **kwargs)

    def outputs(self, result: Any) -> Any:
        return self.output if self.buffered else result

    def completed(
        self, args: tuple[Any, ...], kwargs: dict[str, Any], result: Any, metadata: dict[str, Any]
    ) -> str | None:
        metadata["communication"] = {
            "primitive": self.kind,
            "group": "tp",
            "size": self.size,
            "dimension": 0 if self.buffered else args[1] if self.kind == "all_gather" else None,
        }
        if self.buffered:
            metadata["communication"]["buffered_output"] = True
        source = args[1] if self.buffered else args[0]
        output = self.outputs(result)
        shapes = self.values.shapes
        if source in shapes.unavailable:
            shapes.unavailable[output] = self.path
            shapes.tensors.pop(output, None)
            return f"collective input relation unavailable: {shapes.unavailable[source]}"

        shape = list(shapes.tensor(source).shape)
        if self.kind == "all_gather":
            shape[0 if self.buffered else args[1]] *= self.size
        elif self.kind == "reduce_scatter":
            metadata["guards"] = [
                {"condition": ["eq", ["mod", shapes.expression(shape[0]), self.size], 0], "expected": True}
            ]
            shape[0] //= self.size
        self.values.bind_shape(output, shape)

        return None


class BatchCoordinationCall(NativeCall):
    def __init__(self, values: Any, original: Any) -> None:
        super().__init__(values.recording, values, original.__qualname__, original, {})

    def completed(self, args: tuple[Any, ...], kwargs: dict[str, Any], result: Any, metadata: dict[str, Any]) -> None:
        if result is None or not result.reqs:
            return
        metadata.update(
            entrypoint="batch_coordination",
            entrypoint_phase=None,
            ports={"inputs": {}, "outputs": {}},
        )


class ProcessGroupObservation:
    def __init__(self, observation: Any) -> None:
        self.observation = observation
        self.recording = observation.recording
        self.values = observation.operators.values

    def profile_request(self, original: Any, manager: Any, request: Any) -> Any:
        if not self.observation.selected:
            import torch.distributed as distributed
            from sglang.srt.managers.io_struct import ProfileReqOutput, ProfileReqType

            # The native Torch profiler stops with a TP/DP barrier, including peers without an exporter.
            if request.req_type == ProfileReqType.STOP_PROFILE:
                distributed.barrier(manager.dp_tp_cpu_group)

            # This worker still serves the group; only observation and export are skipped.
            return ProfileReqOutput(success=True, message="Participant has no assigned observation")
        return original(manager, request)

    def profile(self, original: Any, manager: Any, output_dir: str, *args: Any, **kwargs: Any) -> Any:
        rank = self.observation.parallel["rank"]
        self.profile_output = Path(output_dir) / f"rank-{rank}"
        return original(manager, str(self.profile_output.with_name(f".rank-{rank}")), *args, **kwargs)

    def stop_profile(self, original: Any, manager: Any, *args: Any, **kwargs: Any) -> Any:
        result = original(manager, *args, **kwargs)

        # Publish only after the native recording is closed and its raw buffers are flushed.
        if result.success:
            manager.torch_profiler_output_dir.rename(self.profile_output)
        return result

    def collective(self, original: Any, group: Any, tensor: Any, *args: Any, **kwargs: Any) -> Any:
        if self.recording.current_scope.get() is None or group.world_size == 1:
            return original(group, tensor, *args, **kwargs)

        call = CollectiveCall(self.values, group.world_size, original.__name__, partial(original, group))
        if original.__name__ == "all_gather" and not args:
            return call(tensor, kwargs.pop("dim", -1), **kwargs)
        return call(tensor, *args, **kwargs)

    def coordinate(self, original: Any, *args: Any, **kwargs: Any) -> Any:
        if self.recording.current_scope.get() is None:
            return original(*args, **kwargs)
        return BatchCoordinationCall(self.values, original)(*args, **kwargs)

    def coordination_geometry(self, original: Any, info: Any, *args: Any, **kwargs: Any) -> Any:
        if self.recording.current_scope.get() is None:
            return original(info, *args, **kwargs)
        code = inspect.unwrap(original).__code__
        dimensions = {(code, f"self.{field}"): getattr(info, field) for field in ("dp_size", "tp_size", "cp_size")}
        with TensorGeometryObservation(self.values, scalars=dimensions):
            return original(info, *args, **kwargs)

    def gather_into_tensor(
        self, original: Any, output: Any, source: Any, group: Any = None, async_op: bool = False
    ) -> Any:
        from sglang.srt.distributed.parallel_state import get_tp_group

        if self.recording.current_scope.get() is None or self.values.interface is not None:
            return original(output, source, group=group, async_op=async_op)
        parallel = get_tp_group()
        if group not in (parallel.device_group, parallel.cpu_group):
            return original(output, source, group=group, async_op=async_op)

        call = CollectiveCall(self.values, parallel.world_size, "all_gather_into_tensor", original)
        return call(output, source, group=group, async_op=async_op)

    def install(self) -> None:
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        targets = {
            (
                "sglang.srt.managers.scheduler_components.profiler_manager.SchedulerProfilerManager._profile"
            ): self.profile_request,
            "sglang.srt.distributed.parallel_state.GroupCoordinator.all_reduce": self.collective,
            "sglang.srt.distributed.parallel_state.GroupCoordinator.all_gather": self.collective,
            "sglang.srt.distributed.parallel_state.GroupCoordinator.all_gather_into_tensor": self.collective,
            "sglang.srt.distributed.parallel_state.GroupCoordinator.reduce_scatter_tensor": self.collective,
            "sglang.srt.managers.scheduler_components.dp_attn.prepare_mlp_sync_batch_raw": self.coordinate,
            "sglang.srt.managers.scheduler_components.dp_attn.MLPSyncBatchInfo.all_gather": self.coordination_geometry,
            "torch.distributed.all_gather_into_tensor": self.gather_into_tensor,
            (
                "sglang.srt.managers.scheduler_components.profiler_manager.SchedulerProfilerManager._init_profile"
            ): self.profile,
            (
                "sglang.srt.managers.scheduler_components.profiler_manager.SchedulerProfilerManager._stop_profile"
            ): self.stop_profile,
        }
        for target, hook in targets.items():
            HookRegistry.register(target, hook, HookType.AROUND)


class SingleDeviceShard:
    def __init__(self, size: int, rank: int, values: Any) -> None:
        if size < 2 or not 0 <= rank < size:
            raise ValueError("invalid logical tensor-parallel role")

        self.size = size
        self.rank = rank
        self.values = values

        self.depth = 0
        self.communicators: dict[Any, Any] = {}

    def execute(self, original: Any, *args: Any, **kwargs: Any) -> Any:
        from sglang.srt.runtime_context import get_parallel

        # Limit logical topology overrides to native model and memory-pool execution.
        with get_parallel().override(
            tp_size=self.size,
            tp_rank=self.rank,
            attn_tp_size=self.size,
            attn_tp_rank=self.rank,
            moe_tp_size=self.size,
            moe_tp_rank=self.rank,
        ):
            self.depth += 1
            try:
                return original(*args, **kwargs)
            finally:
                self.depth -= 1

    def attention_size(self, original: Any) -> int:
        return self.size if self.depth else original()

    def communicator(self, group: Any) -> Any:
        from sglang.srt.distributed.device_communicators.npu_communicator import NpuCommunicator

        with self.values.auxiliary():
            physical = group.device_group
            if physical not in self.communicators:
                communicator = NpuCommunicator(physical)
                if communicator.world_size != 1:
                    raise ValueError("single-device shard requires a physical process group of one")
                communicator.world_size = self.size
                self.communicators[physical] = communicator

            return self.communicators[physical]

    def all_reduce(self, original: Any, group: Any, input_: Any) -> Any:
        if not self.depth or group.world_size != 1:
            return original(group, input_)

        call = CollectiveCall(self.values, self.size, "all_reduce", self.communicator(group).all_reduce)
        return call(input_)

    def all_gather(self, original: Any, group: Any, input_: Any, dim: int = -1, output_tensor_list: Any = None) -> Any:
        if not self.depth or group.world_size != 1:
            return original(group, input_, dim, output_tensor_list)
        if output_tensor_list is not None:
            raise NotImplementedError("single-device shard supports tensor-result all-gather")

        call = CollectiveCall(self.values, self.size, "all_gather", self.communicator(group).all_gather)
        return call(input_, dim)

    def gather_into_tensor(
        self, original: Any, output_tensor: Any, input_tensor: Any, group: Any = None, async_op: bool = False
    ) -> Any:
        import torch

        if not self.depth or group not in self.communicators:
            return original(output_tensor, input_tensor, group=group, async_op=async_op)
        if async_op:
            raise ValueError("single-device all-gather transport supports synchronous calls only")

        with self.values.auxiliary():
            chunks = output_tensor.view(self.size, *input_tensor.shape)
            physical_output = chunks[0]

        # c10d redispatches its public API through TorchFunction; the physical buffer is already sliced.
        with torch._C.DisableTorchFunction():
            result = original(physical_output, input_tensor, group=group, async_op=False)

        # Fill absent peers only for local execution; auxiliary work is excluded from source structure.
        with self.values.auxiliary():
            chunks[1:].copy_(input_tensor)

        return result

    def install_transport(self) -> None:
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        targets = {
            "sglang.srt.distributed.parallel_state.GroupCoordinator.all_reduce": self.all_reduce,
            "sglang.srt.distributed.parallel_state.GroupCoordinator.all_gather": self.all_gather,
            "torch.distributed.all_gather_into_tensor": self.gather_into_tensor,
        }
        for target, hook in targets.items():
            HookRegistry.register(target, hook, HookType.AROUND)

    def install(self) -> None:
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        from markov_collection.adapters.frameworks.sglang.roles import WorkerRole

        WorkerRole(self.size, self.rank, self.values).install()
        self.install_transport()

        targets = {
            "sglang.srt.model_executor.model_runner.ModelRunner.load_model": self.execute,
            "sglang.srt.model_executor.model_runner.ModelRunner.init_memory_pool": self.execute,
            "sglang.srt.model_executor.model_runner.ModelRunner.init_attention_backend": self.execute,
            "sglang.srt.model_executor.model_runner.ModelRunner.forward": self.execute,
            "sglang.srt.model_executor.forward_batch_info.ForwardBatch.init_new": self.execute,
            "sglang.srt.layers.dp_attention.get_attention_tp_size": self.attention_size,
        }
        for target, hook in targets.items():
            HookRegistry.register(target, hook, HookType.AROUND)
