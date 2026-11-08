"""Keep only boundary tensor identities and live mutation provenance."""

from __future__ import annotations

from collections.abc import Iterator
from contextlib import contextmanager
from dataclasses import dataclass
from typing import Any

import torch
from torch.multiprocessing.reductions import StorageWeakRef
from torch.utils import _pytree
from torch.utils._python_dispatch import TorchDispatchMode, _disable_current_modes
from torch.utils.weak import WeakIdKeyDictionary

from markov_collection.instrumentation.capture import SemanticCapture


@dataclass
class StorageBoundary:
    lifetime: StorageWeakRef
    identity: int
    version: int = 0
    writer: int | None = None
    allocation: int | None = None
    exposed: bool = True
    buffer: str = ""


class TensorBoundaries(TorchDispatchMode):
    def __init__(self, capture: SemanticCapture) -> None:
        super().__init__()
        self.capture = capture
        self.tensors: Any = WeakIdKeyDictionary()
        self.shapes: Any = WeakIdKeyDictionary()
        self.storages: dict[tuple[int, str | None], StorageBoundary] = {}
        self.next_storage = 0
        self.next_value = 0
        self.namespace = ""

    @contextmanager
    def metadata(self) -> Iterator[None]:
        with self.capture.runtime.scope("markov.observer"), torch._C.DisableTorchFunction(), _disable_current_modes():
            yield

    @contextmanager
    def auxiliary(self) -> Iterator[None]:
        with self.capture.runtime.scope("markov.auxiliary"), torch._C.DisableTorchFunction(), _disable_current_modes():
            yield

    def storage(self, tensor: torch.Tensor, region: str | None = None) -> StorageBoundary:
        native = tensor.untyped_storage()
        identity = native._cdata, region
        previous = self.storages.get(identity)
        if previous is None or previous.lifetime.expired():
            previous = StorageBoundary(StorageWeakRef(native), self.next_storage)
            self.next_storage += 1
            self.storages[identity] = previous
        return previous

    def written(self, tensor: torch.Tensor, region: str | None = None) -> None:
        storage = self.storage(tensor, region)
        owner = self.capture.current.get()

        # A version identifies an externally visible mutation epoch, not the number of hidden operators.
        if storage.exposed or storage.writer != owner:
            storage.version += 1
        storage.writer = owner
        storage.exposed = False

    def bind_buffer(self, tensor: torch.Tensor, name: str) -> None:
        storage = self.storage(tensor)
        if storage.buffer and storage.buffer != name:
            raise ValueError("native buffer aliases require one declared storage owner")
        storage.buffer = name

    def declare_shape(self, tensor: torch.Tensor, rule: list[Any]) -> None:
        if len(rule) != tensor.ndim:
            raise ValueError("declared boundary geometry has a different rank from the native tensor")
        self.shapes[tensor] = tuple(tensor.shape), list(rule)

    def shape_rule(self, tensor: torch.Tensor) -> list[Any]:
        declared = self.shapes.get(tensor)
        if declared is None:
            raise ValueError("collective input needs a shape relation from its producer boundary")
        observed, rule = declared
        if observed != tuple(tensor.shape):
            raise ValueError("tensor geometry changed after its boundary declaration")
        return rule

    def observe(self, tensor: torch.Tensor, shape: list[Any] | None, *, region: str | None = None) -> dict[str, Any]:
        with self.metadata():
            storage = self.storage(tensor, region)
            key = storage.identity, storage.version
            previous = self.tensors.get(tensor)
            if previous is None or previous[0] != key:
                previous = key, self.next_value
                self.next_value += 1
                self.tensors[tensor] = previous
            storage.exposed = True
            result = {
                "value": f"{self.namespace}value/{previous[1]}", "storage": f"{self.namespace}storage/{storage.identity}",
                "version": storage.version, "dtype": str(tensor.dtype), "shape": list(tensor.shape),
                "device": tensor.device.type, "writer": storage.writer, "allocation": storage.allocation, "buffer": storage.buffer,
            }
            if shape is not None:
                self.declare_shape(tensor, shape)
                result["shape_rule"] = shape
            return result

    def retire(self) -> None:

        # Captured ports contain plain metadata; only live native storage needs mutation tracking between calls.
        self.storages = {identity: state for identity, state in self.storages.items() if not state.lifetime.expired()}

    def __torch_dispatch__(self, operation: Any, types: Any, args: tuple = (), kwargs: dict | None = None) -> Any:
        keywords = {} if kwargs is None else kwargs
        incoming = {
            self.storage(tensor).identity
            for tensor in _pytree.tree_leaves((args, keywords))
            if isinstance(tensor, torch.Tensor)
        }
        mutated = []
        for index, parameter in enumerate(operation._schema.arguments):
            if parameter.alias_info is not None and parameter.alias_info.is_write:
                value = args[index] if index < len(args) else keywords.get(parameter.name)
                mutated.extend(item for item in _pytree.tree_leaves(value) if isinstance(item, torch.Tensor))

        result = operation(*args, **keywords)
        with torch._C.DisableTorchFunction(), _disable_current_modes():
            for tensor in mutated:
                self.written(tensor)

            # torch.tensor materializes storage before dispatching lift_fresh; its aliased argument is still newly created.
            fresh = operation is torch.ops.aten.lift_fresh.default
            for tensor in _pytree.tree_leaves(result):
                if isinstance(tensor, torch.Tensor):
                    state = self.storage(tensor)
                    if state.writer is None and (fresh or state.identity not in incoming):
                        state.writer = self.capture.current.get()
                        state.allocation = state.writer
                        state.exposed = False
        return result
