"""Planning and deduplication of semantic C++ model-run requests."""

from __future__ import annotations

from itertools import product
from typing import TYPE_CHECKING, Any

from ...common.naming import safe_slug
from ..types import (
    ModelRunSpec,
    ProfileRunRef,
    TargetHiCacheConfig,
)

if TYPE_CHECKING:
    from ..context import WorkflowOptions


def plan_model_runs(
    options: WorkflowOptions, runs: list[ProfileRunRef], preflight_report: dict[str, Any]
) -> list[ModelRunSpec]:
    """Translate target requests and source preflight gates into execution specs."""

    source_readiness = {row["manifest_path"]: row for row in preflight_report["sources"]}
    io_model = options.hicache_io_model
    specs = []
    for source, target in product(runs, options.target_configs):
        run_id = model_run_id(source, target)
        readiness = source_readiness[str(source.manifest_path)]
        # Trace errors precede a missing model; HiCache fact errors follow it.
        skip_reason = readiness["skip_reason"]
        if readiness["full_trace_ready"] and io_model is None:
            skip_reason = "missing_hicache_io_model"
        specs.append(
            ModelRunSpec(
                run_id=run_id,
                output_dir=options.output_dir / "model_runs" / run_id,
                source=source,
                target=target,
                skip_reason=skip_reason,
                cpu_service_cost=options.cpu_service_costs.get(source.manifest_path.resolve()),
            )
        )
    run_ids = [spec.run_id for spec in specs]
    if len(run_ids) != len(set(run_ids)):
        raise ValueError("HiCache prediction plan produced duplicate source/target/workload cells")
    return sorted(specs, key=lambda spec: spec.run_id)


def model_run_id(source: ProfileRunRef, target: TargetHiCacheConfig) -> str:
    """Build a readable HiCache prediction identifier."""

    pair = f"{safe_slug(source.config_id)}_to_{safe_slug(target.label)}"
    return "__".join(("hicache", safe_slug(source.input_id), pair))
