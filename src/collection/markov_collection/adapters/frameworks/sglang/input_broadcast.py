"""Describe native request broadcasts without observing serialized contents."""

from __future__ import annotations

import inspect
from collections.abc import Callable
from dataclasses import dataclass
from typing import Any

from markov_collection.adapters.runtimes.pytorch.gloo import GlooBroadcastLayout


@dataclass(slots=True)
class InputBroadcast:
    channel: str
    receiver: Any
    sequence: int
    ordinal: int = 0


@dataclass(slots=True)
class NativeInputMessage:
    message: dict[str, Any]
    group: Any
    work: Any = None


class SGLangInputBroadcast:
    def __init__(self, channel: str, markers: Any) -> None:
        self.channel = channel
        self.markers = markers
        self.current: InputBroadcast | None = None
        self.active_message: NativeInputMessage | None = None
        self._sequences: dict[Any, int] = {}

    def invoke(self, original: Callable[..., Any], receiver: Any, *args: Any, **kwargs: Any) -> Any:
        with self.markers.observer():
            previous = self.current
            group = receiver.tp_cpu_group
            sequence = self._sequences.get(group, 0)
            self._sequences[group] = sequence + 1
            # Count every native receiver round, including unrecorded warmup.
            # This pairs ranks within one real group run, never separate Engines.
            self.current = InputBroadcast(self.channel, receiver, sequence)
        try:
            return original(receiver, *args, **kwargs)
        finally:
            with self.markers.observer():
                self.current = previous

    def message(self, arguments: inspect.BoundArguments) -> dict[str, Any] | None:
        import torch.distributed as dist

        current = self.current
        if current is None or arguments.arguments["group"] is not current.receiver.tp_cpu_group:
            return None

        # The native request codec chooses how many tensors it broadcasts.
        # Count actual calls rather than assuming a header/payload protocol.
        receiver = current.receiver
        tensor = arguments.arguments["tensor"]
        group_source = arguments.arguments.get("group_src")
        root = receiver.tp_group.ranks[group_source] if group_source is not None else arguments.arguments["src"]
        ranks = list(receiver.tp_group.ranks)
        message = {
            "channel": current.channel,
            "broadcast_sequence": current.sequence,
            "ordinal": current.ordinal,
            "group_ranks": ranks,
            "root": root,
            "rank": receiver.ps.tp_rank,
            "data_type": str(tensor.dtype),
            "element_count": tensor.numel(),
            "wire_bytes": tensor.numel() * tensor.element_size(),
            "physical_group_size": dist.get_world_size(arguments.arguments["group"]),
            "synchronous": not arguments.arguments["async_op"],
            "completion_entry_dependencies": GlooBroadcastLayout.completion_entry_dependencies(
                arguments.arguments["group"], ranks, root
            ),
        }
        current.ordinal += 1
        return message

    def describe(
        self, semantic: str, original: Callable[..., Any], *args: Any, **kwargs: Any
    ) -> NativeInputMessage | None:
        if semantic == "input_message":
            arguments = inspect.signature(original).bind(*args, **kwargs)
            arguments.apply_defaults()
            message = self.message(arguments)
            return None if message is None else NativeInputMessage(message, arguments.arguments["group"])

        call = self.active_message
        if call is None:
            return None
        if semantic == "input_message_submit" and args[0] is call.group:
            return call
        if semantic == "input_message_wait" and call.work is not None and args[0] is call.work:
            return call
        return None

    def invoke_message(
        self, semantic: str, call: NativeInputMessage, original: Callable[..., Any], *args: Any, **kwargs: Any
    ) -> Any:
        if semantic != "input_message":
            result = original(*args, **kwargs)
            if semantic == "input_message_submit":
                with self.markers.observer():
                    call.work = result
            return result

        # Only the real business broadcast activates its native Work linkage.
        # Auxiliary source calls bypass this wrapper, including both children.
        with self.markers.observer():
            previous = self.active_message
            self.active_message = call
        try:
            return original(*args, **kwargs)
        finally:
            with self.markers.observer():
                self.active_message = previous
