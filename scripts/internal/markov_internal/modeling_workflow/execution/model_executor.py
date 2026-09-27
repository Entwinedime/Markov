"""Execution and compact persistence of normalized model-run cells."""

from __future__ import annotations

import concurrent.futures
from collections.abc import Iterator
from pathlib import Path
from typing import TYPE_CHECKING

from ...common.io import load_json, write_json
from ...common.process import run_command
from ...modeling.backend import build_trace_graph_command
from ..progress import StageProgress
from ..types import ModelRunResult, ModelRunSpec
from .runner_adapter import hicache_model_config, runner_config

if TYPE_CHECKING:
    from ..context import WorkflowOptions


def run_model_runs(options: WorkflowOptions, specs: list[ModelRunSpec]) -> Iterator[ModelRunResult]:
    """Execute one plan; keep unstarted cells explicit after a command failure."""

    detail = f"jobs {options.model_run_jobs}" if options.model_run_jobs > 1 else ""
    progress = StageProgress("prediction", len(specs), detail, unit="target")
    completed = set()
    succeeded = failed = skipped = 0
    for result, inflight in _result_stream(options, specs):
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


def _result_stream(options: WorkflowOptions, specs: list[ModelRunSpec]) -> Iterator[tuple[ModelRunResult, int]]:
    """Use the same bounded scheduling and failure policy at any worker count."""

    max_workers = min(options.model_run_jobs, max(1, len(specs)))
    remaining = iter(specs)
    stop_scheduling = False

    with concurrent.futures.ThreadPoolExecutor(max_workers=max_workers) as executor:
        futures: set[concurrent.futures.Future[ModelRunResult]] = set()

        def submit_next() -> bool:
            try:
                spec = next(remaining)
            except StopIteration:
                return False
            futures.add(executor.submit(_run_one, options, spec))
            return True

        while len(futures) < max_workers and submit_next():
            pass

        while futures:
            done, _ = concurrent.futures.wait(futures, return_when=concurrent.futures.FIRST_COMPLETED)
            futures.difference_update(done)
            completed = [future.result() for future in done]
            # Inspect the entire completed batch before replacing workers;
            # set iteration order must not decide whether failures stop work.
            if not options.continue_on_error and any(result.return_code != 0 for result in completed):
                stop_scheduling = True

            while not stop_scheduling and len(futures) < max_workers and submit_next():
                pass

            for result in completed:
                yield result, len(futures)


def _run_one(options: WorkflowOptions, spec: ModelRunSpec) -> ModelRunResult:
    log_path = spec.output_dir / "model.log"
    if spec.skip_reason:
        return ModelRunResult(spec, 0, skip_reason=spec.skip_reason)

    try:
        spec.output_dir.mkdir(parents=True, exist_ok=True)
        if not options.dry_run:
            log_path.unlink(missing_ok=True)

        config = runner_config(spec, options)
        model = hicache_model_config(
            spec.target,
            source_prefetch_policy=spec.source.hicache_config["prefetch_policy"],
            source_target_same_config=spec.target.matches_source(spec.source),
            io_model=options.hicache_io_model,
        )
        write_json(spec.output_dir / "cpp_model_config.json", model)
        write_json(spec.output_dir / "runner_config.json", config.to_raw())
        if options.dry_run:
            return ModelRunResult(spec, 0, skip_reason="dry_run")

        # The report belongs to this attempt. Do not use file timestamps to
        # distinguish fresh failures from leftovers of an earlier command.
        report = spec.output_dir / "run_summary.json"
        report.unlink(missing_ok=True)
        (spec.output_dir / "model_summary.json").unlink(missing_ok=True)
        return_code = run_command(build_trace_graph_command(config), log_path=log_path).returncode
        if return_code == 0:
            if not options.diagnostics.keep_debug_artifacts:
                log_path.unlink(missing_ok=True)
        else:
            truncate_log_tail(
                log_path,
                options.diagnostics.failure_log_max_bytes,
            )
        missing_costs = ()
        if return_code != 0 and report.is_file():
            failure = load_json(report)
            if failure.get("status") == "data_limitation":
                missing_costs = tuple(failure["missing_costs"])
        return ModelRunResult(spec, return_code, missing_costs=missing_costs)
    except Exception as error:
        # Keep subprocess diagnostics if reading its report failed.
        with log_path.open("a", encoding="utf-8") as log_file:
            log_file.write(f"\ninternal model runner error: {error}\n")
        truncate_log_tail(
            log_path,
            options.diagnostics.failure_log_max_bytes,
        )
        return ModelRunResult(spec, 1)


def truncate_log_tail(path: Path, limit: int) -> None:
    """Retain at most ``limit`` trailing bytes from one failed command log."""

    if not path.is_file() or path.stat().st_size <= limit:
        return
    marker = f"[model log truncated to final {limit} bytes]\n".encode()
    tail_bytes = max(0, limit - len(marker))
    with path.open("rb") as log_file:
        log_file.seek(-tail_bytes, 2)
        tail = log_file.read()
    path.write_bytes(marker + tail)
