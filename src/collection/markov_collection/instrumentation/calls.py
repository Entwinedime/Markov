"""One native-call lifecycle for module and runtime function interfaces."""

from __future__ import annotations

import inspect
from contextlib import ExitStack, nullcontext
from typing import Any

from markov_collection.instrumentation.recording import Recording


class ShapeUnavailable(RuntimeError):
    """The native result exists, but its reusable shape relation is not established."""


class NativeCall:
    def __init__(
        self,
        recording: Recording,
        values: Any,
        path: str,
        original: Any,
        inputs: dict[str, dict[int, str]],
        *,
        role: str = "call",
        interface: str | None = None,
        mutations: tuple[int, ...] = (),
    ) -> None:
        self.recording = recording
        self.values = values
        self.path = path
        self.original = original
        self.inputs = inputs

        self.interface = interface
        self.role = role
        self.mutations = mutations
        self.signature = inspect.signature(original) if inputs else None

    def __call__(self, *args: Any, **kwargs: Any) -> Any:
        if self.recording.current_scope.get() is None:
            return self.original(*args, **kwargs)

        role = "operation" if self.interface is not None else self.role
        with self.recording.scope(role, path=self.path) as scope, ExitStack() as input_bindings:
            with self.values.metadata():
                bound = {}
                if self.signature is not None:
                    arguments = self.signature.bind(*args, **kwargs)
                    arguments.apply_defaults()
                    bound = arguments.arguments
                    for path, axes in self.inputs.items():
                        name, *attributes = path.split(".")
                        value = arguments.arguments[name]
                        for attribute in attributes:
                            value = getattr(value, attribute)
                        if value is not None:
                            input_bindings.enter_context(self.values.bind_input(value, axes))

                scope.metadata["arguments"], scope.metadata["keywords"] = self.arguments(args, kwargs)
                reads, writes = self.resources(bound)
                state_writes = self.values.tensor_leaves(writes)
                opaque = self.values.tensor_leaves(reads) + state_writes
                scope.metadata["inputs"] = list(dict.fromkeys(self.values.references((args, kwargs, opaque))))
                if opaque:
                    scope.metadata["opaque_inputs"] = self.values.references(opaque)

                # Snapshot mutation versions before invoking code that may bypass dispatcher annotations.
                modified = [args[index] for index in self.mutations] + state_writes
                versions = {}
                for value in modified:
                    storage = self.values.storage(value)
                    versions[storage] = self.values.versions[storage]

                if self.interface is not None:
                    scope.metadata["interface"] = self.interface
                    scope.metadata["writes"] = sorted(versions)

            # Interface internals remain source evidence; only their declared ports are compiled for reuse.
            context = self.values.native_interface(scope.identity) if self.interface is not None else nullcontext()
            with context:
                result = self.invoke(*args, **kwargs)

            # Some native functions mutate without a dispatcher write annotation.
            with self.values.metadata():
                for storage, version in versions.items():
                    if self.values.versions[storage] == version:
                        self.values.versions[storage] += 1

                try:
                    scope.metadata["shape_error"] = self.completed(args, kwargs, result, scope.metadata)
                except ShapeUnavailable as error:

                    # Preserve complete native evidence; template construction still rejects this unresolved relation.
                    scope.metadata["shape_error"] = str(error)
                    for tensor in self.values.tensor_leaves(self.outputs(result)):
                        self.values.shapes.unavailable[tensor] = self.path
                        self.values.shapes.tensors.pop(tensor, None)
                scope.metadata["result"] = self.values.encode(result)
                scope.metadata["outputs"] = self.values.references(self.outputs(result))
                if modified:
                    scope.metadata["mutated_values"] = self.values.references(modified)
                if state_writes:
                    scope.metadata["opaque_outputs"] = self.values.references(state_writes)

            return result

    def invoke(self, *args: Any, **kwargs: Any) -> Any:
        return self.original(*args, **kwargs)

    def arguments(self, args: tuple[Any, ...], kwargs: dict[str, Any]) -> tuple[Any, Any]:
        return self.values.encode(args), self.values.encode(kwargs)

    def resources(self, arguments: dict[str, Any]) -> tuple[Any, Any]:
        """Declare opaque state reads and read/write tensors outside the argument tree."""
        return (), ()

    def completed(
        self, args: tuple[Any, ...], kwargs: dict[str, Any], result: Any, metadata: dict[str, Any]
    ) -> str | None:
        """Specialized native interfaces can declare output geometry before publication."""
        return None

    def outputs(self, result: Any) -> Any:
        return result
