"""Bind core-declared regions to model modules and serving components without device tensor copies."""

from __future__ import annotations

import inspect
from collections.abc import Callable
from dataclasses import dataclass
from typing import Any

from markov_collection.instrumentation.blocks import BlockWrapper, BoundBlock
from markov_collection.instrumentation.recording import Observation, Recorder


@dataclass(frozen=True)
class BoundNativeRegion(BoundBlock):
    module_type: str
    method: str
    tensor_inputs: tuple[str, ...]
    dimensions: tuple[tuple[str, str], ...]
    call_parameters: tuple[tuple[str, str], ...]
    tensor_outputs: tuple[tuple[str, int | None], ...]
    module_inputs: tuple[tuple[str, str], ...]
    component_path: str
    cpu_inputs: tuple[str, ...]
    component_owner: str
    request_id_path: str

    @classmethod
    def read(cls, binding: dict[str, Any], parameters: tuple[str, ...]) -> BoundNativeRegion:
        return cls(
            binding["id"],
            binding["step"],
            binding["phase"],
            parameters,
            binding["module_type"],
            binding["method"],
            tuple(binding["inputs"]),
            tuple((parameter["name"], parameter["attribute"]) for parameter in binding["parameters"]),
            tuple((parameter["name"], parameter["attribute"]) for parameter in binding["call_parameters"]),
            tuple((output["name"], output["index"]) for output in binding["outputs"]),
            tuple((item["name"], item["attribute"]) for item in binding["module_inputs"]),
            binding["component_path"],
            tuple(binding["cpu_inputs"]),
            binding["component_owner"],
            binding["request_id_path"],
        )


class NativeRegionWrapper(BlockWrapper):
    def __init__(
        self, binding: BoundNativeRegion, path: str, module: Any, recorder: Recorder, original: Callable[..., Any]
    ) -> None:
        super().__init__((binding,))
        self.binding = binding
        self.path = path
        self.module = module
        self.recorder = recorder
        self.original = original
        self.signature = inspect.signature(self.original)

    def context(self, *args: Any, **kwargs: Any) -> tuple[Recorder, dict[str, Any]] | None:
        if not self.recorder.in_scope():
            return None

        # Object parameters include mutable control state (for example a free
        # group flag), so read them at entry rather than at wrapper installation.
        dimensions = {}
        for name, attribute in self.binding.dimensions:
            value = self.module
            for part in attribute.split("."):
                value = getattr(value, part)
            dimensions[name] = int(value)

        arguments = self.signature.bind(*args, **kwargs)
        arguments.apply_defaults()
        # Branch values are CPU scalars from the actual invocation, not
        # module constants or device tensor contents.
        call_parameters = {}
        for name, attribute in self.binding.call_parameters:
            value = self.argument(arguments, attribute)
            call_parameters[name] = int(value)

        # Per-request regions carry native identity explicitly. Neither tensor
        # lengths nor the order of repeated operations identify a request.
        request_id = None
        if self.binding.request_id_path:
            request_id = self.argument(arguments, self.binding.request_id_path)

        # Allocators replace their free-page tensor between calls. Read current
        # layout metadata here rather than caching a stale object at installation.
        inputs = {}
        for name, attribute in self.binding.module_inputs:
            tensor = self.module
            for part in attribute.split("."):
                tensor = getattr(tensor, part)
            inputs[name] = self.tensor_layout(tensor)

        for name in self.binding.tensor_inputs:
            inputs[name] = self.tensor_layout(self.argument(arguments, name))

        cpu_values = {}
        for name in self.binding.cpu_inputs:
            tensor = self.argument(arguments, name)
            if tensor.device.type != "cpu":
                raise ValueError(f"native region CPU input is on a device: {name}")
            cpu_values[name] = tensor.tolist()

        return self.recorder, {
            "module_path": self.path,
            "module_type": self.binding.module_type,
            "method": self.binding.method,
            "module_parameters": dimensions,
            "call_parameters": call_parameters,
            "inputs": inputs,
            "outputs": {},
            "cpu_values": cpu_values,
            "request_id": request_id,
        }

    @staticmethod
    def argument(arguments: inspect.BoundArguments, path: str) -> Any:
        # Nested batch fields retain their declared path as the observation key.
        root, *parts = path.split(".")
        value = arguments.arguments[root]
        for part in parts:
            value = getattr(value, part)
        return value

    @staticmethod
    def tensor_layout(tensor: Any) -> dict[str, Any] | None:
        if tensor is None:
            return None

        # Layout access reads host metadata, never device values.
        return {
            "shape": list(tensor.shape),
            "stride": list(tensor.stride()),
            "dtype": str(tensor.dtype),
            "device": str(tensor.device),
        }

    def completed(self, observation: Observation, result: Any, *args: Any, **kwargs: Any) -> None:
        observation.metadata["outputs"] = {
            name: self.tensor_layout(result if index is None else result[index])
            for name, index in self.binding.tensor_outputs
        }

    def __call__(self, *args: Any, **kwargs: Any) -> Any:
        return self.invoke(self.original, *args, **kwargs)


class NativeRegions:
    def __init__(self, contract: dict[str, Any]) -> None:
        parameters = tuple(parameter["name"] for parameter in contract["definition"]["parameters"])
        self.bindings: dict[str, list[BoundNativeRegion]] = {}
        for binding in contract["bindings"]:
            # Explicit class hooks are installed by the framework adapter; they
            # must not also wrap methods on individual component instances.
            if binding["hook"]:
                continue

            region = BoundNativeRegion.read(binding, parameters)
            self.bindings.setdefault(region.module_type, []).append(region)
        self.model: Any = None
        self.components: dict[str, Any] = {}

    def install_components(self, owner: Any, recorder: Recorder, owner_kind: str) -> None:
        for bindings in self.bindings.values():
            for binding in bindings:
                path = binding.component_path
                if not path or binding.component_owner != owner_kind:
                    continue

                component = owner
                # A dot denotes the declared owner itself (for example ModelRunner.sample).
                if path != ".":
                    for part in path.split("."):
                        component = getattr(component, part)
                if self.components.get(binding.identity) is component:
                    continue

                component_type = f"{type(component).__module__}.{type(component).__qualname__}"
                if component_type == binding.module_type:
                    original = getattr(component, binding.method)
                    wrapper = NativeRegionWrapper(binding, path, component, recorder, original)
                    setattr(component, binding.method, wrapper)
                    self.components[binding.identity] = component

    def install(self, model: Any, recorder: Recorder) -> None:
        if self.model is model:
            return

        # Object paths retain layer identities. Each implementation binding uses
        # the same wrapper lifecycle, including nested exception/parent restoration.
        for path, module in model.named_modules():
            module_type = f"{type(module).__module__}.{type(module).__qualname__}"
            for binding in self.bindings.get(module_type, ()):
                if not binding.component_path:
                    original = getattr(module, binding.method)
                    wrapper = NativeRegionWrapper(binding, path, module, recorder, original)
                    setattr(module, binding.method, wrapper)
        self.model = model
