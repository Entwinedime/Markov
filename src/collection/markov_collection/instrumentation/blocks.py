"""Shared lifecycle for native wrappers consuming core-defined block contracts."""

from __future__ import annotations

from abc import ABC, abstractmethod
from collections.abc import Callable, Iterator
from contextlib import contextmanager
from dataclasses import dataclass
from typing import Any

from markov_collection.instrumentation.recording import Observation, Recorder


@dataclass(frozen=True)
class BoundBlock:
    identity: str
    step: str
    phase: str | None
    parameters: tuple[str, ...]


class BlockWrapper(ABC):
    def __init__(self, blocks: tuple[BoundBlock, ...]) -> None:
        self.blocks = blocks

    @abstractmethod
    def context(self, *args: Any, **kwargs: Any) -> tuple[Recorder, dict[str, Any]] | None:
        """Return native inputs, or None for calls outside the capture window."""

    def completed(self, observation: Observation, result: Any, *args: Any, **kwargs: Any) -> None:
        """Attach native result metadata before closing the observation."""
        return None

    def closed(self, observation: Observation) -> None:
        """Allow publication to flush only after its final record has been written."""
        return None

    @contextmanager
    def scope(self, *args: Any, **kwargs: Any) -> Iterator[Observation | None]:
        context = self.context(*args, **kwargs)
        if context is None:
            yield None
            return

        recorder, values = context
        block = self.blocks[0]
        if len(self.blocks) > 1:
            block = next(candidate for candidate in self.blocks if candidate.phase == values["phase"])

        # Method wrappers and inline regions share identity and exception cleanup.
        metadata = {name: values[name] for name in block.parameters}
        with recorder.scope(block.step, metadata, binding=block.identity) as observation:
            yield observation

        self.closed(observation)

    def invoke(self, original: Callable[..., Any], *args: Any, **kwargs: Any) -> Any:
        with self.scope(*args, **kwargs) as observation:
            result = original(*args, **kwargs)
            if observation is not None:
                self.completed(observation, result, *args, **kwargs)

        return result
