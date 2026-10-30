"""Parse a prediction request once, before entering the modeling workflow."""

from __future__ import annotations

from dataclasses import dataclass
import json
from pathlib import Path
from typing import Any
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from markov.artifacts.capture import CaptureSettings


def read_json(path: Path) -> dict[str, Any]:
    with path.open(encoding="utf-8") as stream:
        value = json.load(stream)

    if not isinstance(value, dict):
        raise ValueError(f"{path}: expected a JSON object")

    return value


def resolve_path(directory: Path, value: str) -> Path:
    """Relative input paths belong to the document containing the reference."""
    return (directory / value).resolve()


def reject_unknown(raw: dict[str, Any], allowed: set[str], context: str) -> None:
    if not isinstance(raw, dict):
        raise ValueError(f"{context}: expected a JSON object")

    unknown = raw.keys() - allowed
    if unknown:
        raise ValueError(f"{context}: unknown fields: {', '.join(sorted(unknown))}")


def positive_integer(value: int, name: str) -> int:
    if type(value) is not int or value < 1:
        raise ValueError(f"{name} must be a positive integer")

    return value


def required_text(value: str, name: str) -> str:
    if not isinstance(value, str) or not value.strip():
        raise ValueError(f"{name} must be non-empty text")

    return value


@dataclass(frozen=True)
class Deployment:
    platform: str
    framework: str
    runtime: str
    model: str
    dtype: str
    tensor_parallel: int

    @classmethod
    def parse(cls, raw: dict[str, Any]) -> Deployment:
        reject_unknown(raw, {"platform", "framework", "runtime", "model", "dtype", "tensor_parallel"}, "deployment")
        return cls(
            platform=required_text(raw["platform"], "platform"),
            framework=required_text(raw["framework"], "framework"),
            runtime=required_text(raw["runtime"], "runtime"),
            model=required_text(raw["model"], "model"),
            dtype=required_text(raw["dtype"], "dtype"),
            tensor_parallel=positive_integer(raw["tensor_parallel"], "tensor_parallel"),
        )


@dataclass(frozen=True)
class BaseInput:
    manifest: Path | None
    configuration: dict[str, Any] | None

    @classmethod
    def parse(cls, raw: dict[str, Any], directory: Path) -> BaseInput:
        reject_unknown(raw, {"manifest", "configuration"}, "base")
        if ("manifest" in raw) == ("configuration" in raw):
            raise ValueError("base requires exactly one of manifest or configuration")

        manifest = resolve_path(directory, raw["manifest"]) if "manifest" in raw else None
        configuration = raw.get("configuration")
        if manifest is None and not isinstance(configuration, dict):
            raise ValueError("base.configuration must be a JSON object")

        return cls(manifest, configuration)


@dataclass(frozen=True)
class Target:
    name: str
    configuration: dict[str, Any]

    @classmethod
    def parse(cls, raw: dict[str, Any]) -> Target:
        reject_unknown(raw, {"name", "configuration"}, "target")
        if not isinstance(raw["configuration"], dict):
            raise ValueError("target.configuration must be a JSON object")

        return cls(required_text(raw["name"], "target.name"), raw["configuration"])


@dataclass(frozen=True)
class Execution:
    output: Path
    jobs: int
    memory_limit_gib: int | None
    capture: CaptureSettings | None = None
    kernel_cache_initial_state: str | None = None

    @classmethod
    def parse(cls, raw: dict[str, Any], directory: Path) -> Execution:
        from markov.artifacts.capture import CaptureSettings

        reject_unknown(
            raw, {"output", "jobs", "memory_limit_gib", "capture", "kernel_cache_initial_state"}, "execution"
        )
        memory_limit = raw.get("memory_limit_gib")
        if memory_limit is not None:
            memory_limit = positive_integer(memory_limit, "memory_limit_gib")
        initial = raw.get("kernel_cache_initial_state")
        if initial is not None and initial not in ("empty", "prepared"):
            raise ValueError("execution.kernel_cache_initial_state must be empty or prepared")

        return cls(
            resolve_path(directory, raw["output"]),
            positive_integer(raw.get("jobs", 1), "jobs"),
            memory_limit,
            CaptureSettings.parse(raw["capture"]) if "capture" in raw else None,
            initial,
        )


@dataclass(frozen=True)
class PredictionRequest:
    deployment: Deployment
    base: BaseInput
    workload: Path
    targets: tuple[Target, ...]
    models: Path | None
    execution: Execution

    @classmethod
    def load(cls, path: Path) -> PredictionRequest:
        raw = read_json(path)
        reject_unknown(raw, {"deployment", "base", "workload", "targets", "performance", "execution"}, "request")
        directory = path.resolve().parent
        targets = tuple(Target.parse(item) for item in raw["targets"])
        if not targets:
            raise ValueError("targets must contain at least one configuration")
        if len({item.name for item in targets}) != len(targets):
            raise ValueError("target names must be unique within a request")

        performance = raw.get("performance", {})
        reject_unknown(performance, {"models"}, "performance")
        model_path = performance.get("models")
        return cls(
            Deployment.parse(raw["deployment"]),
            BaseInput.parse(raw["base"], directory),
            resolve_path(directory, raw["workload"]),
            targets,
            resolve_path(directory, model_path) if model_path is not None else None,
            Execution.parse(raw["execution"], directory),
        )
