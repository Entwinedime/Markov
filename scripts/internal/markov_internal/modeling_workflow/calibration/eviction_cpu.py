"""Measure SGLang's locked-candidate loop, without inference or a copied loop body."""

from __future__ import annotations

import argparse
import math
import os
from pathlib import Path
import platform
import random
from statistics import median
import time
from types import SimpleNamespace

from ...common.io import load_json, write_json
from ...common.paths import ROOT_DIR, repo_relative_path, require_repo_path
from ...common.commands import positive_int
from ..io_model_contract import nonnegative_finite_number, positive_finite_number
from .options import parse_cpu_sets, parse_positive_csv


def measure_locked_candidates(evict, cache, params, *, batch: int, repeats: int) -> list[dict]:
    """Subtract the same call with num_tokens=0; both calls build the same heap.

    All candidates are locked, so the active call drains the heap without device
    work. Alternating call order reduces drift. Thread CPU time excludes scheduler
    waits; batching amortizes the two clock reads, not the operation itself.
    """

    def timed(num_tokens):
        request = params(num_tokens=num_tokens)
        start = time.thread_time_ns()
        for _ in range(batch):
            result = evict(cache, request)
        elapsed = (time.thread_time_ns() - start) / (1000 * batch)
        if result.num_tokens_evicted != 0:
            raise ValueError("Locked-candidate calibration unexpectedly evicted data")
        return elapsed

    for active in (0, 1):
        timed(active)  # Equal warmup for both paths, never retained as observations.
    rows = []
    for repeat in range(repeats):
        order = (0, 1) if repeat % 2 == 0 else (1, 0)
        durations = {active: timed(active) for active in order}
        rows.append(
            dict(
                repeat=repeat,
                order=list(order),
                baseline_us=durations[0],
                active_us=durations[1],
                loop_us=durations[1] - durations[0],
            )
        )
    return rows


def capture_locked_candidates(evict, node_type, strategy, params, *, heap_sizes, cpu_sets, batch, repeats, output):
    """Keep raw paired samples; a negative difference is evidence, never clamped."""
    original_affinity = os.sched_getaffinity(0)
    report = dict(
        status="sampling",
        operation="eviction_locked_candidate",
        framework_entry="sglang.srt.mem_cache.hiradix_cache.HiRadixCache.evict",
        python=platform.python_version(),
        machine=platform.machine(),
        clock="thread_time_ns",
        batch=batch,
        repeats=repeats,
        heap_sizes=list(heap_sizes),
        target_workload_trace_used=False,
        target_score_used=False,
        samples=[],
        limitations=[
            "Synthetic locked candidates isolate the loop; this is not a legal initial cache-state workload.",
            "Measures loop guards, heap pop and lock check, not setup, release, I/O, or residual waits.",
            "Random distinct LRU priorities; equal-priority comparisons and other eviction policies are unmeasured.",
            "Sequential CPU scopes, no inference contention; deployment applicability still needs admission.",
            "Raw observations only: no coefficients or permission to extrapolate outside the sampled domain.",
        ],
    )
    write_json(output, report)
    try:
        for rank, cpus in enumerate(cpu_sets):
            if not cpus or not cpus <= original_affinity:
                raise ValueError("Sampling CPUs must belong to the process allowed affinity")
            os.sched_setaffinity(0, cpus)
            for count in heap_sizes:
                priorities = list(range(count))
                random.Random(count).shuffle(priorities)
                nodes = [node_type() for _ in priorities]
                for node, priority in zip(nodes, priorities):
                    node.lock_ref, node.last_access_time = 1, float(priority)
                cache = SimpleNamespace(
                    evictable_leaves=nodes,
                    eviction_strategy=strategy,
                    cache_controller=SimpleNamespace(write_policy="write_through"),
                    update_eviction_metrics=lambda *_: None,
                )
                pairs = measure_locked_candidates(evict, cache, params, batch=batch, repeats=repeats)
                report["samples"].append(
                    dict(
                        rank=rank,
                        cpu_affinity=sorted(os.sched_getaffinity(0)),
                        heap_size=count,
                        pairs=pairs,
                        median_loop_us=median(row["loop_us"] for row in pairs),
                    )
                )
                write_json(output, report)
        report["status"] = "measured"
    except BaseException as error:
        report.update(status="failed", error=f"{type(error).__name__}: {error}")
        raise
    finally:
        os.sched_setaffinity(0, original_affinity)
        write_json(output, report)
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--cpu-sets", required=True, help="One allowed CPU set per deployment rank, separated by |")
    parser.add_argument("--heap-sizes", default="1,8,64", help="Explicit primitive domain, not target observations")
    parser.add_argument("--batch", type=positive_int, default=128)
    parser.add_argument("--repeats", type=positive_int, default=7)
    args = parser.parse_args(argv)
    from sglang.srt.mem_cache.hiradix_cache import HiRadixCache
    from sglang.srt.mem_cache.radix_cache import TreeNode
    from sglang.srt.mem_cache.base_prefix_cache import EvictParams
    from sglang.srt.mem_cache.evict_policy import LRUStrategy

    capture_locked_candidates(
        HiRadixCache.evict,
        TreeNode,
        LRUStrategy(),
        EvictParams,
        heap_sizes=parse_positive_csv(args.heap_sizes, "heap-sizes"),
        cpu_sets=parse_cpu_sets(args.cpu_sets, len(args.cpu_sets.split("|"))),
        batch=args.batch,
        repeats=args.repeats,
        output=args.output_dir / "eviction_cpu.json",
    )


def acquire_eviction_cpu(group, needs, *, dry_run=False):
    """Acquire one declared primitive domain for all affected targets, not per cell."""
    from ..physical_capture import physical_attempts, physical_usage, _completed, run_primitive_attempt
    from ..group import source_environment
    from ..planning.profile_runs import parse_server_command_flags

    required = [need for need in needs if need["component"] == "execution_control/eviction_locked_candidate"]
    if not required:
        return dict(status="not_required")
    spec = group.raw.get("physical_capture", {})
    sampling = spec.get("eviction_cpu")
    plan = dict(status="needs_cpu_primitive", requirements=required, usage=physical_usage(physical_attempts(group)))
    if not sampling or not spec.get("cpu_sets"):
        return dict(plan, stop_reason="declare physical_capture.eviction_cpu domain and cpu_sets")
    flags = parse_server_command_flags(group.sources[0].run_dir / "server_cmd.txt")
    if flags.get("radix_eviction_policy", "lru") != "lru":
        return dict(plan, stop_reason="CPU primitive sampler supports LRU only")
    cpus = parse_cpu_sets(spec["cpu_sets"], len(spec["cpu_sets"].split("|")))
    environment = source_environment(group.sources[0])
    if len(cpus) != int(environment["server"]["tp_size"]):
        raise ValueError("CPU primitive needs one CPU set per base TP rank")
    sizes = parse_positive_csv(",".join(str(size) for size in sampling["heap_sizes"]), "heap_sizes")
    definition = dict(
        environment=environment,
        cpu_sets=[sorted(value) for value in cpus],
        heap_sizes=sizes,
        batch=positive_int(str(sampling.get("batch", 128))),
        repeats=positive_int(str(sampling.get("repeats", 7))),
        logical_io_bytes={"eviction_cpu": 0},
    )

    def validate(path):
        observed = load_json(path)
        if (
            observed["status"] != "measured"
            or observed["operation"] != "eviction_locked_candidate"
            or observed["target_workload_trace_used"] is not False
            or observed["target_score_used"] is not False
        ):
            raise ValueError("Expected independent locked-candidate CPU observations")
        expected = {(rank, size) for rank in range(len(cpus)) for size in sizes}
        samples = observed["samples"]
        if len(samples) != len(expected) or {(row["rank"], row["heap_size"]) for row in samples} != expected:
            raise ValueError("CPU primitive report does not cover its declared rank/heap domain")
        if any(
            set(row["cpu_affinity"]) != cpus[row["rank"]] or len(row["pairs"]) != definition["repeats"]
            for row in samples
        ):
            raise ValueError("CPU primitive placement or repeat count differs from its declaration")
        return len(samples)

    existing = _completed(physical_attempts(group), definition, "eviction_cpu")
    source_ledger = sampling.get("reuse_ledger")
    if existing is None and source_ledger:
        existing = _completed(load_json(require_repo_path(source_ledger))["attempts"], definition, "eviction_cpu")
        if existing is None:
            return dict(
                plan, stop_reason="Declared CPU measurement does not cover this environment and sampling domain"
            )
    if existing:
        validate(require_repo_path(existing["report"]))
        return dict(plan, status="cpu_primitive_measured", report=existing["report"], reused=True)
    if dry_run:
        return dict(plan, definition=definition, stop_reason="dry_run")

    def command(output: Path) -> list[str]:
        return [
            str(ROOT_DIR / "scripts/model.sh"),
            "calibrate-hicache",
            "eviction-cpu",
            "--output-dir",
            str(repo_relative_path(output / "result")),
            "--cpu-sets",
            spec["cpu_sets"],
            "--heap-sizes",
            ",".join(str(size) for size in sizes),
            "--batch",
            str(definition["batch"]),
            "--repeats",
            str(definition["repeats"]),
        ]

    attempt = run_primitive_attempt(group, definition, "eviction_cpu", command, "eviction_cpu.json", validate)
    return dict(
        plan,
        status="cpu_primitive_measured" if attempt["status"] == "completed" else attempt["status"],
        attempt=attempt,
        usage=physical_usage(physical_attempts(group)),
        report=attempt.get("report"),
        reused=False,
    )


def estimate_locked_candidate_cost(report):
    """A pop at heap size n costs a + b*log2(n); draining N sums that cost.

    N=1 identifies a; the largest heap identifies b through log2(N!). Interior
    heaps are checks, never extra fitted parameters. Pool ranks by medians.
    """
    if report["clock"] != "thread_time_ns":
        raise ValueError("Locked-candidate coefficients require thread CPU time")
    by_rank = {}
    for row in report["samples"]:
        differences = [
            nonnegative_finite_number(pair["active_us"], "active CPU")
            - nonnegative_finite_number(pair["baseline_us"], "baseline CPU")
            for pair in row["pairs"]
        ]
        value = positive_finite_number(median(differences), "median loop CPU")
        by_rank.setdefault(row["rank"], {})[row["heap_size"]] = (value, min(differences), max(differences))
    intercepts, slopes = [], []
    for values in by_rank.values():
        if 1 not in values or len(values) < 3:
            raise ValueError("Locked-candidate model needs heap 1, a larger anchor and an interior check")
        largest = max(values)
        fixed = values[1][0]
        slope = (values[largest][0] - largest * fixed) / (math.lgamma(largest + 1) / math.log(2))
        intercepts.append(fixed)
        slopes.append(nonnegative_finite_number(slope, "heap growth coefficient"))
    fixed, slope = median(intercepts), median(slopes)
    checks = []
    for rank, values in by_rank.items():
        for size, (observed, low, high) in sorted(values.items()):
            predicted = size * fixed + slope * math.lgamma(size + 1) / math.log(2)
            checks.append(
                dict(
                    rank=rank,
                    heap_size=size,
                    observed_loop_us=observed,
                    predicted_loop_us=predicted,
                    relative_error=predicted / observed - 1,
                    observed_range_us=[low, high],
                    role="anchor" if size in (1, max(values)) else "check",
                )
            )
    return dict(locked_candidate_us=fixed, locked_candidate_log2_heap_us=slope), dict(
        formula="per pop: a + b*log2(n); drained heap: N*a + b*log2(N!)",
        estimator="N=1 and largest heap per rank, then median rank coefficients; interior sizes held out",
        heap_sizes=sorted({size for values in by_rank.values() for size in values}),
        checks=checks,
        interpretation="Shared CPU service estimate; not residual wait or whole eviction cost",
        limitations=report["limitations"]
        + [
            "Logarithmic growth outside measured heap sizes is an unvalidated extrapolation.",
            "Held-out errors and repeat ranges describe calibration approximation, not target E2E accuracy.",
        ],
    )


def group_locked_candidate_cost(group):
    """Read matching completed group evidence; model construction never collects."""
    existing = acquire_eviction_cpu(
        group,
        [{"component": "execution_control/eviction_locked_candidate", "reason": "lookup_existing_measurement"}],
        dry_run=True,
    )
    if existing["status"] != "cpu_primitive_measured":
        return {}, {}
    coefficients, evidence = estimate_locked_candidate_cost(load_json(require_repo_path(existing["report"])))
    evidence.update(origin="independent_calibration", report=existing["report"], shared_across_group=True)
    return coefficients, evidence


if __name__ == "__main__":
    main()
