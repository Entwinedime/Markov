"""PyTorch profiler marker implementation; imported without initializing a device."""

from contextlib import AbstractContextManager

from markov_collection.instrumentation.recording import ProfilerMarkers


class TorchProfilerMarkers(ProfilerMarkers):
    def scope(self, name: str) -> AbstractContextManager:
        import torch

        return torch.profiler.record_function(name)
