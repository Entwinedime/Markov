"""Read the actual PyTorch module tree and observe direct forward calls."""

from __future__ import annotations

from collections.abc import Callable
from typing import Any

import torch

from markov_collection.instrumentation.calls import NativeCall
from markov_collection.instrumentation.recording import Recording


class ModuleObservation:
    def __init__(
        self,
        recording: Recording,
        model: Any,
        values: Any,
        inputs: dict[str, dict[int, str]],
        calls: dict[type, Callable[..., NativeCall]] | None = None,
    ) -> None:
        self.recording = recording
        self.model = model
        self.values = values
        self.inputs = inputs
        self.calls = calls or {}
        self.originals: list[tuple[Any, bool, Any]] = []

    def __enter__(self) -> ModuleObservation:
        try:
            with self.values.metadata():
                self.install()
        except BaseException:
            self.restore()
            raise

        return self

    def install(self) -> None:
        describe = not self.recording.modules
        for path, module in self.model.named_modules():
            if describe:
                self.recording.modules.append(self.describe(path, module))

            owned = "forward" in module.__dict__
            original = module.forward
            self.originals.append((module, owned, original))
            call = self.calls.get(type(module), NativeCall)
            module.forward = call(
                self.recording, self.values, path, original, self.inputs if not path else {}, role="module"
            )

    def describe(self, path: str, module: Any) -> dict[str, Any]:
        parameters = []

        # Parameters have fixed geometry in this model/dtype/TP domain, including those read by opaque kernels.
        for name, value in module.named_parameters(recurse=False):
            self.values.shapes.tensor(value)
            parameters.append(
                {"name": name, "shape": list(value.shape), "dtype": str(value.dtype), "value": self.values.value(value)}
            )

        return {
            "path": path,
            "type": f"{type(module).__module__}.{type(module).__qualname__}",
            "kind": "sequence" if isinstance(module, (torch.nn.ModuleList, torch.nn.Sequential)) else "module",
            "parameters": parameters,
        }

    def __exit__(self, *_: Any) -> None:
        self.restore()

    def restore(self) -> None:

        # Remove temporary instance overrides when forward originally came from the class.
        for module, owned, original in reversed(self.originals):
            if owned:
                module.forward = original
            else:
                del module.forward

        self.originals.clear()
