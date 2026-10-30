"""Plan and execute source/target prediction cells with compact persistence."""

from __future__ import annotations

from collections.abc import Iterator
from itertools import product
from pathlib import Path
from typing import TYPE_CHECKING, Any

from markov_internal.common.io import load_json, truncate_log_tail, write_json
from markov_internal.common.naming import safe_slug
from markov_internal.modeling.source_dag.profiles import ProfileRunRef
from markov_internal.common.progress import StageProgress
from markov_internal.modeling.types import ModelRunResult, ModelRunSpec, ModelRunStarted, SourcePreparation
from markov_internal.modeling.run_config import ModelingRunConfig
from markov_internal.modeling.execution.runner_adapter import hicache_model_config, runner_config
from markov_internal.modeling.execution.source_session import SourceSession

if TYPE_CHECKING:
    from markov_internal.modeling.context import DiagnosticLevel, WorkflowOptions


def plan_model_runs(
    options: WorkflowOptions, runs: list[ProfileRunRef], preflight_report: dict[str, Any]
) -> list[ModelRunSpec]:
    """Plan cells from admitted targets and source readiness, not cost availability.

    The public boundary has already admitted the required independent package.
    A planned cell can still report a concrete cost gap during target execution.
    """

    source_readiness = {row["manifest_path"]: row for row in preflight_report["sources"]}
    specs = []
    for source, target in product(runs, options.target_configs):
        pair = f"{safe_slug(source.config_id)}_to_{safe_slug(target.label)}"
        run_id = "__".join(("hicache", safe_slug(source.input_id), pair))
        specs.append(
            ModelRunSpec(
                run_id=run_id,
                output_dir=options.output_dir / "model_runs" / run_id,
                source=source,
                target=target,
                skip_reason=source_readiness[str(source.manifest_path)]["skip_reason"],
                cpu_service_cost=options.cpu_service_costs.get(source.manifest_path.resolve()),
            )
        )

    run_ids = [spec.run_id for spec in specs]
    if len(run_ids) != len(set(run_ids)):
        raise ValueError("HiCache prediction plan produced duplicate source/target/workload cells")
    # Preserve caller priority so deadlines do not starve required reference cells.
    return specs


class ExecutionSession:
    """Only the current source stays resident; switching it releases the old process."""

    def __init__(self, directory: Path, diagnostics: DiagnosticLevel) -> None:
        self.directory = directory
        self.diagnostics = diagnostics
        self.source: SourceSession | None = None
        self.identity: tuple[Path, Path | None] | None = None
        self.source_count = 0

    def source_for(self, config: ModelingRunConfig) -> SourceSession:
        identity = config.profile_manifest, config.cpu_service_cost
        if identity != self.identity or (
            self.source is not None and self.source.process is None and self.source.failed
        ):
            self.close()
            self.source = SourceSession(config, self.directory / "sources" / str(self.source_count))
            self.source_count += 1
            self.identity = identity
        return self.source

    def close(self) -> None:
        if self.source is not None:
            source = self.source
            self.source = None
            self.identity = None
            source.close()
            if not source.failed and not self.diagnostics.keep_debug_artifacts:
                source.log_path.unlink(missing_ok=True)
            else:
                truncate_log_tail(source.log_path, self.diagnostics.failure_log_max_bytes)


def run_model_runs(
    options: WorkflowOptions, specs: list[ModelRunSpec], session: ExecutionSession
) -> Iterator[ModelRunResult | ModelRunStarted | SourcePreparation]:
    """Execute one plan; keep unstarted cells explicit after a command failure."""

    detail = f"jobs {options.model_run_jobs}" if options.model_run_jobs > 1 else ""
    progress = StageProgress("prediction", len(specs), detail, unit="target")
    completed = set()
    succeeded = failed = skipped = 0
    for result, inflight in _result_stream(options, specs, session):
        if not isinstance(result, ModelRunResult):
            yield result
            continue

        completed.add(result.spec.run_id)
        succeeded += result.ok
        failed += result.return_code != 0
        skipped += bool(result.skip_reason)
        progress.advance({"inflight": str(inflight)})
        yield result

    for spec in specs:
        if spec.run_id not in completed:
            skipped += 1
            yield ModelRunResult(spec, 0, skip_reason="not_started_after_failure")

    if failed:
        status = "ERROR"
    elif skipped:
        status = "CHECK"
    else:
        status = "OK" if specs else "EMPTY"

    progress.finish(
        status,
        f"{len(specs)} runs | {succeeded} commands succeeded | {failed} failed | {skipped} skipped",
    )


def _result_stream(
    options: WorkflowOptions, specs: list[ModelRunSpec], session: ExecutionSession
) -> Iterator[tuple[ModelRunResult | ModelRunStarted | SourcePreparation, int]]:
    """Prepare each source once; native targets keep the bounded job policy.

    Sources are processed in plan order, not concurrently. Trace buffers and
    lazy caches live only in this invocation; nothing is cached on disk.
    """

    sources: dict[tuple[Path, Path | None], list[ModelRunSpec]] = {}
    for spec in specs:
        sources.setdefault((spec.source.manifest_path, spec.cpu_service_cost), []).append(spec)

    for batch in sources.values():
        failed = False
        for result, inflight in _run_source(options, batch, session):
            if isinstance(result, ModelRunResult):
                failed |= result.return_code != 0
            yield result, inflight
        if failed and not options.continue_on_error:
            return


def _run_source(
    options: WorkflowOptions, specs: list[ModelRunSpec], session: ExecutionSession
) -> Iterator[tuple[ModelRunResult | ModelRunStarted | SourcePreparation, int]]:
    """Publish one batch while the owning session retains the source process."""

    ready = []
    configs = []
    for spec in specs:
        if spec.skip_reason:
            yield ModelRunResult(spec, 0, skip_reason=spec.skip_reason), 0
            continue

        spec.output_dir.mkdir(parents=True, exist_ok=True)
        try:
            config = runner_config(spec, options)
            model = hicache_model_config(
                spec.target,
                source_prefetch_policy=spec.source.hicache_config["prefetch_policy"],
                source_target_same_config=spec.target.matches_source(spec.source),
                cost_model_path=options.cost_model_path,
            )
            write_json(spec.output_dir / "cpp_model_config.json", model)
            write_json(spec.output_dir / "runner_config.json", config.to_raw())
        except Exception as error:
            (spec.output_dir / "model.log").write_text(f"model input preparation failed: {error}\n", encoding="utf-8")
            yield ModelRunResult(spec, 1), 0
            if not options.continue_on_error:
                return
            continue
        if options.dry_run:
            yield ModelRunResult(spec, 0, skip_reason="dry_run"), 0
            continue

        # Every output belongs to this attempt, including previously enabled
        # experimental reports. No old file can turn failure into success.
        for name in (
            "model.log",
            "run_summary.json",
            "model_summary.json",
            "source_replay_report.json",
            "structure_report.json",
        ):
            (spec.output_dir / name).unlink(missing_ok=True)
        ready.append(spec)
        configs.append(config)

    if not ready:
        return

    native = session.source_for(configs[0])
    source_directory = native.directory
    source_directory.mkdir(parents=True, exist_ok=True)
    plan_path = source_directory / "target_plan.json"
    write_json(
        plan_path,
        {
            "jobs": options.model_run_jobs,
            "continue_on_error": options.continue_on_error,
            "runs": [
                {"output_dir": str(config.output_dir), "model_config": str(config.model_config_path)}
                for config in configs
            ],
        },
    )
    batch_log = native.log_path
    completed = set()
    return_code = 1
    try:
        for outcome in native.execute(plan_path):
            if outcome.get("event") == "batch_started":
                native.memory_budget = {key: value for key, value in outcome.items() if key != "event"}
                continue
            if outcome.get("event") in {"source_prepared", "source_reused"}:
                yield (
                    SourcePreparation(
                        ready[0].source.manifest_path,
                        ready[0].cpu_service_cost,
                        outcome.get("wall_seconds", 0.0),
                        outcome.get("performance", {}),
                        reused=outcome["event"] == "source_reused",
                    ),
                    0,
                )
                continue
            if outcome.get("event") == "batch_completed":
                return_code = outcome["return_code"]
                break

            index = outcome["index"]
            if outcome.get("event") == "target_started":
                yield ModelRunStarted(ready[index]), 0
                continue

            result = _finish_result(
                options,
                ready[index],
                outcome["return_code"],
                outcome["skip_reason"],
                {**outcome.get("performance", {}), "prediction_memory_budget": native.memory_budget},
            )
            completed.add(index)
            yield result, outcome["inflight"]
    except Exception as error:
        native.failed = True
        native.close()
        with batch_log.open("a", encoding="utf-8") as log:
            log.write(f"\nnative prediction batch failed: {error}\n")

    # Source preparation/process failure has no target result. Preserve its
    # cause for all affected cells rather than reading leftovers or filling 0.
    if native.process is None:
        truncate_log_tail(batch_log, options.diagnostics.failure_log_max_bytes)
    for index, spec in enumerate(ready):
        if index not in completed:
            detail = batch_log.read_text(encoding="utf-8") or "native target completion record missing"
            (spec.output_dir / "model.log").write_text(detail, encoding="utf-8")
            yield _finish_result(options, spec, return_code or 1), 0


def _finish_result(
    options: WorkflowOptions,
    spec: ModelRunSpec,
    return_code: int,
    skip_reason: str = "",
    performance: dict[str, Any] | None = None,
) -> ModelRunResult:
    """Read only failed current reports; successful summaries belong to read_row."""

    log = spec.output_dir / "model.log"
    missing_costs = ()
    structure_gaps = ()
    if return_code != 0:
        truncate_log_tail(log, options.diagnostics.failure_log_max_bytes)
        report = spec.output_dir / "run_summary.json"
        if report.is_file():
            failure = load_json(report)
            if failure.get("status") == "data_limitation":
                missing_costs = tuple(failure["missing_costs"])
            elif failure.get("status") == "structure_limitation":
                structure_gaps = tuple(failure["structure_gaps"])
    elif not options.diagnostics.keep_debug_artifacts:
        log.unlink(missing_ok=True)

    return ModelRunResult(spec, return_code, skip_reason, missing_costs, structure_gaps, performance or {})
