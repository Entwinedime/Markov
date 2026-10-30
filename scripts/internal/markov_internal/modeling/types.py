"""Core immutable contracts shared across modeling-workflow layers."""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from markov_internal.common.io import load_json
from markov_internal.common.paths import require_repo_path
from markov_internal.modeling.cost.control import CALIBRATION_FIELDS
from markov_internal.modeling.cost.io_contract import positive_u64
from markov_internal.modeling.sglang_config import canonical_hicache
from markov_internal.modeling.source_dag.profiles import ProfileRunRef


@dataclass(frozen=True)
class TargetHiCacheConfig:
    """Target policy/capacity input with prefetch_policy resolved at admission."""

    label: str
    fields: dict[str, Any]

    @classmethod
    def load(cls, path: Path) -> TargetHiCacheConfig:
        """Load a policy-only declaration; observations and costs are forbidden."""

        resolved = require_repo_path(path)
        return cls.from_raw(load_json(resolved), resolved)

    @classmethod
    def from_raw(cls, raw: dict[str, Any], source_path: Path | None = None) -> TargetHiCacheConfig:
        """Admit explicit files and inline group targets through the same boundary."""

        if not isinstance(raw, dict) or not isinstance(raw.get("hicache"), dict):
            raise ValueError(f"target config must contain a hicache object: {source_path}")
        if set(raw) - {"name", "hicache"}:
            raise ValueError("target declaration accepts only name and hicache policy/capacity")

        fields: dict[str, Any] = dict(raw["hicache"])
        if any(
            field in fields
            for field in (
                "io_cost",
                "phase_cost",
                "operator_models",
                "cost_package",
                "kv_bytes_per_page",
                "dag_patch",
                *CALIBRATION_FIELDS,
                "control_calibrations",
            )
        ):
            raise ValueError(f"target config cannot contain HiCache model fields: {source_path}")

        fields["page_size"] = positive_u64(fields.get("page_size"), "target hicache.page_size")
        fields.setdefault("prefetch_policy", "timeout")
        if fields["prefetch_policy"] not in {"best_effort", "wait_complete", "timeout"}:
            raise ValueError("target prefetch policy must identify the actual control path")

        label = str(raw.get("name") or (source_path.stem if source_path else ""))
        if not label:
            raise ValueError("inline target declaration requires a name")
        return cls(label=label, fields=fields)

    def matches_source(self, source: ProfileRunRef) -> bool:
        """Compare policy/capacity values without relying on experiment identifiers."""

        if source.hicache_config is None:
            return False
        return canonical_hicache(source.hicache_config) == canonical_hicache(self.fields)


@dataclass(frozen=True)
class ModelRunSpec:
    """One source/target task; shared execution settings belong to WorkflowOptions."""

    run_id: str
    output_dir: Path
    source: ProfileRunRef
    target: TargetHiCacheConfig
    skip_reason: str = ""
    cpu_service_cost: Path | None = None

    @property
    def label(self) -> str:
        """Return the source-to-target prediction label."""

        return f"{self.source.input_id}/{self.source.config_id}->{self.target.label}"


@dataclass(frozen=True)
class ModelRunResult:
    """Outcome of executing, skipping, or dry-running one spec."""

    spec: ModelRunSpec
    return_code: int
    skip_reason: str = ""
    missing_costs: tuple[dict[str, Any], ...] = ()
    structure_gaps: tuple[dict[str, Any], ...] = ()
    performance: dict[str, Any] = field(default_factory=dict)

    @property
    def ok(self) -> bool:
        """Command success only; DAG completion is checked from its execution report."""

        return not self.skip_reason and self.return_code == 0


@dataclass(frozen=True)
class SourcePreparation:
    """Actual source construction wall time, not simulated operation cost."""

    manifest: Path
    cpu_service_cost: Path | None
    wall_seconds: float
    performance: dict[str, Any] = field(default_factory=dict)
    reused: bool = False


@dataclass(frozen=True)
class ModelRunStarted:
    """Native target launch, retained for interrupted search budget accounting."""

    spec: ModelRunSpec
