"""Parse a prediction request once, before entering the modeling workflow."""

from __future__ import annotations

from dataclasses import asdict, dataclass
import json
import math
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
    deployment: Deployment

    @classmethod
    def parse(cls, raw: dict[str, Any], source: Deployment) -> Target:
        reject_unknown(raw, {"name", "configuration", "deployment"}, "target")
        if not isinstance(raw["configuration"], dict):
            raise ValueError("target.configuration must be a JSON object")

        deployment = Deployment.parse({**asdict(source), **raw.get("deployment", {})})
        if any(getattr(deployment, name) != getattr(source, name) for name in asdict(source) if name != "tensor_parallel"):
            raise ValueError("Targets currently change tensor_parallel within the same model, dtype, framework and platform")
        return cls(required_text(raw["name"], "target.name"), raw["configuration"], deployment)


@dataclass(frozen=True)
class ScalingRule:
    name: str
    factor: float

    @classmethod
    def parse(cls, raw: dict[str, Any]) -> ScalingRule:
        reject_unknown(raw, {"name", "factor"}, "node_scale rule")
        factor = raw["factor"]
        if type(factor) not in (int, float) or not math.isfinite(factor) or factor < 0:
            raise ValueError("node_scale factor must be finite and nonnegative")
        return cls(required_text(raw["name"], "node_scale name"), float(factor))


@dataclass(frozen=True)
class Execution:
    output: Path
    memory_limit_gib: int | None
    capture: CaptureSettings | None = None
    kernel_cache_initial_state: str | None = None
    node_scale: tuple[ScalingRule, ...] = ()
    record_graph: bool = False

    @classmethod
    def parse(cls, raw: dict[str, Any], directory: Path) -> Execution:
        from markov.artifacts.capture import CaptureSettings

        reject_unknown(
            raw,
            {
                "output",
                "memory_limit_gib",
                "capture",
                "kernel_cache_initial_state",
                "node_scale",
                "record_graph",
            },
            "execution",
        )
        memory_limit = raw.get("memory_limit_gib")
        if memory_limit is not None:
            memory_limit = positive_integer(memory_limit, "memory_limit_gib")
        initial = raw.get("kernel_cache_initial_state")
        if initial is not None and initial not in ("empty", "prepared"):
            raise ValueError("execution.kernel_cache_initial_state must be empty or prepared")
        record_graph = raw.get("record_graph", False)
        if type(record_graph) is not bool:
            raise ValueError("execution.record_graph must be a boolean")

        return cls(
            resolve_path(directory, raw["output"]),
            memory_limit,
            CaptureSettings.parse(raw["capture"]) if "capture" in raw else None,
            initial,
            tuple(ScalingRule.parse(rule) for rule in raw.get("node_scale", [])),
            record_graph,
        )


@dataclass(frozen=True)
class PredictionRequest:
    deployment: Deployment
    base: BaseInput
    workload: Path
    targets: tuple[Target, ...]
    models: Path | None
    database: Path | None
    sampling_budget: dict[str, int] | None
    execution: Execution

    @classmethod
    def load(cls, path: Path) -> PredictionRequest:
        return cls.parse(read_json(path), path.resolve().parent)

    @classmethod
    def parse(cls, raw: dict[str, Any], directory: Path) -> PredictionRequest:
        reject_unknown(raw, {"deployment", "base", "workload", "targets", "performance", "execution"}, "request")
        deployment = Deployment.parse(raw["deployment"])
        targets = tuple(Target.parse(item, deployment) for item in raw["targets"])
        if not targets:
            raise ValueError("targets must contain at least one configuration")
        if len({item.name for item in targets}) != len(targets):
            raise ValueError("target names must be unique within a request")

        performance = raw.get("performance", {})
        from markov.artifacts.capture import parse_budget

        reject_unknown(performance, {"models", "database", "sampling_budget"}, "performance")
        model_path = performance.get("models")
        database = performance.get("database")
        sampling_budget = parse_budget(performance["sampling_budget"]) if "sampling_budget" in performance else None
        if sampling_budget is not None and database is None:
            raise ValueError("Shared sampling requires performance.database")
        return cls(
            deployment,
            BaseInput.parse(raw["base"], directory),
            resolve_path(directory, raw["workload"]),
            targets,
            resolve_path(directory, model_path) if model_path is not None else None,
            resolve_path(directory, database) if database is not None else None,
            sampling_budget,
            Execution.parse(raw["execution"], directory),
        )

    def document(self) -> dict[str, Any]:
        """Export the public request with resolved paths, not private prepared input."""
        execution = asdict(self.execution)
        execution = {key: value for key, value in execution.items() if value is not None}
        execution["output"] = str(self.execution.output)
        if "capture" in execution:
            execution["capture"] = {key: value for key, value in execution["capture"].items() if value is not None}
            execution["capture"]["devices"] = list(self.execution.capture.devices)
        return {
            "deployment": asdict(self.deployment),
            "base": {"manifest": str(self.base.manifest)} if self.base.manifest else {"configuration": self.base.configuration},
            "workload": str(self.workload),
            "targets": [asdict(target) for target in self.targets],
            "performance": {
                **({"models": str(self.models)} if self.models else {}),
                **({"database": str(self.database)} if self.database else {}),
                **({"sampling_budget": self.sampling_budget} if self.sampling_budget else {}),
            },
            "execution": execution,
        }
