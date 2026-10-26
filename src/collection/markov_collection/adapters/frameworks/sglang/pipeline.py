"""Observe native PP messages without taking ownership of requests or communication work."""

from __future__ import annotations

import inspect
import math
from contextlib import contextmanager
from functools import partial
from typing import Any

import torch

from markov_collection.adapters.frameworks.sglang.batches import BatchTensors
from markov_collection.adapters.runtimes.pytorch.collectives import CommunicationWork
from markov_collection.adapters.runtimes.pytorch.factories import TensorGeometryObservation
from markov_collection.instrumentation.calls import NativeCall


class MessageGeometry(TensorGeometryObservation):
    def __init__(self, values: Any, metadata: dict[str, Any], tensors: dict[str, Any]) -> None:
        super().__init__(values)
        self.metadata = metadata
        self.tensors = tensors
        self.fields: list[tuple[str, Any]] = []
        self.allocations = 0
        self.transfers = 0
        self.header = False
        self.group_size = 1

    def bind_group(self, original: Any, all_gather_group: Any) -> None:
        self.group_size = 1 if all_gather_group is None else all_gather_group.world_size
        self.scalars[inspect.unwrap(original).__code__, "all_gather_size"] = self.group_size
        self.metadata["bindings"] = {"payload_parts": self.group_size}

    def describe(self, fields: list[tuple[str, Any]]) -> None:
        from sglang.srt.distributed.parallel_state import TensorMetadata

        self.fields = [(name, field) for name, field in fields if isinstance(field, TensorMetadata)]
        self.metadata["payload_shapes"] = {}
        for index, (name, field) in enumerate(self.fields):
            axes = [f"payload_{index}_axis_{axis}" for axis in range(len(field.size))]
            self.metadata["payload_shapes"][name] = axes
            for axis, size in zip(axes, field.size, strict=True):
                self.metadata.setdefault("parameters", {})[axis] = size
                self.values.bounded_parameter(axis, 0, 2**63 - 1)
            if name in self.tensors:
                self.values.bind_shape(self.tensors[name], tuple(self.values.shapes.symbol(axis) for axis in axes))
                declaration = {"value": self.metadata["ports"]["inputs"][name], "shape": axes}
                if declaration not in self.metadata.setdefault("shape_contracts", []):
                    self.metadata["shape_contracts"].append(declaration)

            # The native send-allgather branch slices only tensors divisible by the actual TP group.
            if self.group_size > 1:
                elements = axes[0] if axes else 1
                if len(axes) > 1:
                    elements = ["mul", *axes]
                self.metadata.setdefault("guards", []).append(
                    {
                        "condition": ["eq", ["mod", elements, self.group_size], 0],
                        "expected": math.prod(field.size) % self.group_size == 0,
                    }
                )

    def factory_geometry(self, function: Any) -> tuple[str | int, ...] | None:
        if (
            self.header
            or self.values.recording.current_scope.get() != self.scope
            or inspect.unwrap(function) is not inspect.unwrap(torch.empty)
        ):
            return None

        # recv_tensor_dict allocates one complete tensor for each metadata field, before any TP slice.
        name, _ = self.fields[self.allocations]
        self.allocations += 1
        return tuple(self.metadata["payload_shapes"][name])

    def next_field(self) -> str:
        while math.prod(self.fields[self.transfers][1].size) == 0:
            self.transfers += 1
        name, _ = self.fields[self.transfers]
        self.transfers += 1
        return name


class TransferCall(NativeCall):
    def __init__(self, values: Any, original: Any, tensor: Any, communication: dict[str, Any]) -> None:
        super().__init__(
            values.recording,
            values,
            f"torch.distributed.{communication['primitive']}",
            original,
            {},
            interface="point_to_point",
            mutations=(0,) if communication["primitive"] == "receive" else (),
        )
        self.tensor = tensor
        self.communication = communication

    def invoke(self, *args: Any, **kwargs: Any) -> Any:

        # c10d's public Python API redispatches through TorchFunction; capture its dispatcher call only once.
        with torch._C.DisableTorchFunction():
            return self.original(*args, **kwargs)

    def outputs(self, result: Any) -> Any:
        return self.tensor if self.communication["primitive"] == "receive" else ()

    def completed(self, args: tuple[Any, ...], kwargs: dict[str, Any], result: Any, metadata: dict[str, Any]) -> None:
        metadata["communication"] = self.communication

        # The process group and native work stay in the framework; the portable operation uses one tensor port.
        metadata["argument_relations"] = [{"value": metadata["inputs"][0]}]
        metadata["keywords"] = {"mapping": []}
        begin = self.recording.regions[self.recording.current_scope.get()]["operation_begin"]
        for operation in self.recording.operations[begin:]:
            if operation["name"].startswith("c10d."):
                operation["communication"] = True


class ObservedWork(CommunicationWork):
    def __init__(self, observation: Any, work: Any, tensor: Any, identity: int, channel: str, field: str) -> None:
        super().__init__(observation.values, work, (tensor,))
        self.operators = observation.operators
        self.tensor = tensor
        self.identity = identity
        self.entrypoint = f"pipeline_wait_{channel}_{field}"

    def wait(self, *args: Any, **kwargs: Any) -> Any:
        if self.values.recording.current_scope.get() is not None:
            return self(*args, **kwargs)

        # A native send may be committed in a later loop; the original batch still owns this wait.
        with self.values.recording.scope("PipelineWait", batch=self.identity) as scope, self.operators:
            scope.metadata.update(
                entrypoint=self.entrypoint,
                ports={"inputs": self.values.port_values({"tensor": self.tensor}), "outputs": {}},
            )
            return self(*args, **kwargs)


class PipelineObservation:
    fields = BatchTensors.fields | {"model_tokens": "input_ids"}

    def __init__(self, observation: Any) -> None:
        from torch.utils.weak import WeakIdKeyDictionary

        self.observation = observation
        self.operators = observation.operators
        self.values = self.operators.values
        self.recording = observation.recording
        self.tensor_batches: Any = WeakIdKeyDictionary()
        self.received: Any = WeakIdKeyDictionary()
        self.event_records: Any = WeakIdKeyDictionary()
        self.ready_events: Any = WeakIdKeyDictionary()
        self.receive_batch: Any = None
        self.receive_identity: int | None = None
        self.receive_channel = ""
        self.message: MessageGeometry | None = None
        self.channel = ""
        self.export_pending = False

    def export(self, scheduler: Any) -> None:
        self.export_pending = True
        self.write_if_ready(scheduler)

    def write_if_ready(self, scheduler: Any) -> None:
        if scheduler.send_proxy_work or scheduler.send_output_work or self.recording.current_scope.get() is not None:
            return

        # The native loop owns submission and waits; publication only follows its completed queues.
        self.observation.write()
        self.export_pending = False

    def commit(self, original: Any, scheduler: Any, work: Any) -> Any:
        result = original(scheduler, work)
        if self.export_pending:
            self.write_if_ready(scheduler)
        return result

    @staticmethod
    def payload(tensors: dict[str, Any]) -> dict[str, torch.Tensor]:

        # The native dictionary also carries message type and other host metadata.
        return {name: tensor for name, tensor in tensors.items() if isinstance(tensor, torch.Tensor)}

    @staticmethod
    def declare_waits(regions: list[dict[str, Any]]) -> None:
        senders = {}
        for region in regions:
            if region["role"] != "PipelineSend":
                continue
            metadata = region["metadata"]
            channel = metadata["entrypoint"].removeprefix("pipeline_send_")
            for port in metadata["ports"]["outputs"]:
                if port.startswith("wire."):
                    senders[metadata["batch"], f"pipeline_wait_{channel}_{port[5:]}"] = metadata

        for region in regions:
            if region["role"] != "PipelineWait":
                continue
            metadata = region["metadata"]
            sender = senders[metadata["batch"], metadata["entrypoint"]]

            # Equal wire shapes can belong to different send branches; Work.wait retains its native send's conditions.
            for name in ("parameters", "bindings", "guards"):
                if name in sender:
                    metadata[name] = sender[name]

    def remember(self, identity: int, result: Any) -> None:
        payload = result.pp_hidden_states_proxy_tensors
        tensors = payload.tensors if payload is not None else result.next_token_ids
        for tensor in self.values.tensor_leaves(tensors):
            self.tensor_batches[tensor] = identity

    def receive_outputs(
        self,
        original: Any,
        scheduler: Any,
        next_first_rank_mb_id: int,
        next_mb_id: int,
        mbs: Any,
        *args: Any,
        **kwargs: Any,
    ) -> Any:
        previous = self.receive_batch
        self.receive_batch = mbs[next_mb_id]
        try:
            result = original(scheduler, next_first_rank_mb_id, next_mb_id, mbs, *args, **kwargs)
            event = result[2]
            if event is not None and self.receive_batch is not None:
                identity = self.observation.batch_ids.get(self.receive_batch)
                if identity is not None:
                    self.ready_events[event] = identity
                if event in self.event_records:
                    self.event_records[event]["entrypoint"] = "pipeline_ready_record"
            return result
        finally:
            self.receive_batch = previous

    def record_event(self, original: Any, event: Any, stream: Any = None) -> Any:
        if self.recording.current_scope.get() is not None or self.receive_batch is None:
            return original(event, stream)

        identity = self.observation.batch_ids.get(self.receive_batch)
        with self.scope(identity, "event", "record", {}) as observed:
            if observed:
                self.event_records[event] = self.recording.regions[self.recording.current_scope.get()]["metadata"]
            return NativeCall(
                self.recording,
                self.values,
                "torch.npu.Event.record",
                partial(original, event),
                {},
                interface="event_record",
            )(stream)

    def synchronize_event(self, original: Any, event: Any) -> Any:
        identity = self.ready_events.get(event)
        with self.scope(identity, "ready", "synchronize", {}):
            return NativeCall(
                self.recording,
                self.values,
                "torch.npu.Event.synchronize",
                partial(original, event),
                {},
                interface="event_synchronize",
            )()

    @contextmanager
    def scope(self, identity: int | None, direction: str, channel: str, tensors: dict[str, Any]) -> Any:
        if identity is None:
            yield False
            return
        if self.recording.batches[identity]["observation"] == "baseline":
            with self.recording.markers.scope(f"markov.baseline/{identity}"):
                yield False
            return

        with self.recording.scope(f"Pipeline{direction.capitalize()}", batch=identity) as scope, self.operators:
            scope.metadata.update(entrypoint=f"pipeline_{direction}_{channel}", ports={"inputs": {}, "outputs": {}})
            previous = self.message, self.channel
            self.message = MessageGeometry(self.values, scope.metadata, tensors)
            self.channel = channel
            try:
                with self.message:
                    yield True
            finally:
                self.message, self.channel = previous

    def send(
        self,
        original: Any,
        scheduler: Any,
        tensor_dict: dict[str, Any],
        async_send: bool = True,
        msg_type: str = "default",
    ) -> Any:
        payload = self.payload(tensor_dict)
        identities = {self.tensor_batches.get(tensor) for tensor in payload.values()}
        if identities == {None}:
            return original(scheduler, tensor_dict, async_send=async_send, msg_type=msg_type)
        (identity,) = identities
        with self.scope(identity, "send", msg_type, payload) as observed:
            if observed:
                metadata = self.recording.regions[self.recording.current_scope.get()]["metadata"]
                metadata["ports"]["inputs"] = self.values.port_values(payload)
            return original(scheduler, tensor_dict, async_send=async_send, msg_type=msg_type)

    def receive(
        self, original: Any, scheduler: Any, expected_kind: str = "default", all_gather_group: Any = None
    ) -> Any:
        batch = scheduler.cur_batch if expected_kind == "proxy" else self.receive_batch
        identity = self.observation.batch_ids.get(batch)
        previous = self.receive_identity, self.receive_channel
        self.receive_identity, self.receive_channel = identity, expected_kind
        try:
            result = original(scheduler, expected_kind=expected_kind, all_gather_group=all_gather_group)
            payload = self.payload(result)
            if identity is not None:
                for tensor in payload.values():
                    self.tensor_batches[tensor] = identity
                    if tensor in self.received:
                        self.received[tensor]["batch"] = identity
            return result
        finally:
            self.receive_identity, self.receive_channel = previous

    def send_message(
        self,
        original: Any,
        group: Any,
        tensor_dict: dict[str, Any],
        dst: int | None = None,
        all_gather_group: Any = None,
        async_send: bool = False,
    ) -> Any:
        if self.message is not None:
            self.message.bind_group(original, all_gather_group)
        return original(group, tensor_dict, dst=dst, all_gather_group=all_gather_group, async_send=async_send)

    def receive_message(self, original: Any, group: Any, src: int | None = None, all_gather_group: Any = None) -> Any:
        with self.scope(self.receive_identity, "receive", self.receive_channel, {}) as observed:
            if observed:
                self.message.bind_group(original, all_gather_group)
            result = original(group, src=src, all_gather_group=all_gather_group)
            if observed:
                payload = self.payload(result)
                metadata = self.message.metadata
                metadata["ports"]["outputs"] = self.values.port_values(payload)
                metadata.setdefault("shape_contracts", []).extend(
                    {"value": metadata["ports"]["outputs"][name], "shape": metadata["payload_shapes"][name]}
                    for name in payload
                )

                # Native demultiplexing may stash this packet; bind its batch when the framework consumes it.
                for tensor in payload.values():
                    self.received[tensor] = metadata
            return result

    def prepare_result(self, original: Any, scheduler: Any, batch: Any, mb_metadata: Any, pp_outputs: Any) -> Any:
        identity = self.observation.batch_ids.get(batch)
        with self.scope(identity, "result", "tokens", {}) as observed:
            if observed:
                with self.values.metadata():
                    self.values.bind(pp_outputs["next_token_ids"], {0: "B"})
                    self.values.bind_shape(
                        scheduler.future_map.output_tokens_buf, (self.values.shapes.symbol("R") + 1,)
                    )
                metadata = self.recording.regions[self.recording.current_scope.get()]["metadata"]
                metadata["ports"]["inputs"] = self.values.port_values(
                    {
                        "sampled_tokens": pp_outputs["next_token_ids"],
                        "token_relay": scheduler.future_map.output_tokens_buf,
                    }
                )
            result = original(scheduler, batch, mb_metadata, pp_outputs)
            if observed:
                metadata["ports"]["outputs"] = self.values.port_values(
                    {"sampled_tokens": result.next_token_ids, "token_relay": scheduler.future_map.output_tokens_buf}
                )
            return result

    def header(self, original: Any, group: Any, *args: Any, **kwargs: Any) -> Any:
        if self.message is None:
            return original(group, *args, **kwargs)

        self.message.header = True

        # Python metadata selects allocations; its tensors are outside the declared device payload flow.
        try:
            with self.values.auxiliary():
                result = original(group, *args, **kwargs)
            fields = result if original.__name__ == "recv_object" else args[0]
            with self.values.metadata():
                self.channel = dict(fields)["__msg_type__"]
                direction = "receive" if original.__name__ == "recv_object" else "send"
                self.message.metadata["entrypoint"] = f"pipeline_{direction}_{self.channel}"
                self.message.describe(fields)
            return result
        finally:
            self.message.header = False

    def transfer(self, original: Any, tensor: Any, *args: Any, **kwargs: Any) -> Any:
        if self.message is None or self.message.header:
            return original(tensor, *args, **kwargs)

        primitive = "receive" if original.__name__ == "irecv" else "send"
        from sglang.srt.distributed import get_pp_group

        communication = {
            "primitive": primitive,
            "group": "pp",
            "size": get_pp_group().world_size,
            "offset": -1 if primitive == "receive" else 1,
            "channel": self.channel,
            "field": self.message.next_field(),
        }
        work = TransferCall(self.values, original, tensor, communication)(tensor, *args, **kwargs)

        # The deferred native Work.wait consumes the actual wire view, which can be a TP slice.
        if primitive == "send":
            self.message.metadata["ports"]["outputs"].update(
                self.values.port_values({f"wire.{communication['field']}": tensor})
            )
        return (
            ObservedWork(self, work, tensor, self.message.metadata["batch"], self.channel, communication["field"])
            if work is not None
            else work
        )

    def install(self) -> None:
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        targets = {
            "sglang.srt.managers.scheduler.Scheduler._pp_commit_comm_work": self.commit,
            "sglang.srt.managers.scheduler.Scheduler._pp_send_recv_and_preprocess_output_tensors": self.receive_outputs,
            "sglang.srt.managers.scheduler.Scheduler._pp_send_dict_to_next_stage": self.send,
            "sglang.srt.managers.scheduler.Scheduler._pp_recv_typed_dict": self.receive,
            "sglang.srt.managers.scheduler.Scheduler._pp_prep_batch_result": self.prepare_result,
            "sglang.srt.distributed.parallel_state.GroupCoordinator.send_object": self.header,
            "sglang.srt.distributed.parallel_state.GroupCoordinator.recv_object": self.header,
            "sglang.srt.distributed.parallel_state.GroupCoordinator.recv_tensor_dict": self.receive_message,
            "sglang.srt.distributed.parallel_state.GroupCoordinator.send_tensor_dict": self.send_message,
            "torch.distributed.isend": self.transfer,
            "torch.distributed.send": self.transfer,
            "torch.distributed.irecv": self.transfer,
            "torch_npu.npu.streams.Event.record": self.record_event,
            "torch_npu.npu.streams.Event.synchronize": self.synchronize_event,
        }
        for target, hook in targets.items():
            HookRegistry.register(target, hook, HookType.AROUND)
