"""Measure local SGLang eviction and failed-load control paths without inference."""

from __future__ import annotations

import argparse
from collections import OrderedDict
import math
import os
from pathlib import Path
import platform
import random
from statistics import median
import time
from threading import RLock
from types import SimpleNamespace
from typing import Any, TYPE_CHECKING

from markov_internal.common.io import load_json, write_json
from markov_internal.common.paths import ROOT_DIR, repo_relative_path, require_repo_path
from markov_internal.common.commands import positive_int
from markov_internal.modeling.cost.io_contract import nonnegative_finite_number, positive_finite_number
from markov_internal.preparation.calibration.options import parse_cpu_sets, parse_integer_csv, parse_positive_csv

if TYPE_CHECKING:
    from markov_internal.preparation.physical_capture import PhysicalCaptureInputs


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


def capture_write_rejections(
    *, heap_sizes: list[int], page_sizes: list[int], cpu_sets: list[set[int]], batch: int, repeats: int, output: Path
) -> dict[str, Any]:
    """Time actual failed backup calls, including host cleanup and the allocation retry.

    CPU-only objects provide indices and legal pinned/evictable leaves; no KV payload,
    device queue or target workload is used. Rebuild mutated state outside each timer.
    """

    import torch
    from sglang.srt.managers.cache_controller import HiCacheController
    from sglang.srt.mem_cache.evict_policy import LRUStrategy
    from sglang.srt.mem_cache.hiradix_cache import HiRadixCache
    from sglang.srt.mem_cache.memory_pool_host import MHATokenToKVPoolHost
    from sglang.srt.mem_cache.radix_cache import RadixKey, TreeNode

    def initial_state(count: int, released: int, page: int) -> tuple[Any, Any]:
        pool = MHATokenToKVPoolHost.__new__(MHATokenToKVPoolHost)
        pool.page_size, pool.lock = page, RLock()
        pool.free_slots = torch.empty(0, dtype=torch.int64)
        controller = HiCacheController.__new__(HiCacheController)
        controller.mem_pool_host = pool

        cache = HiRadixCache.__new__(HiRadixCache)
        cache.page_size, cache.cache_controller = page, controller
        cache.enable_kv_cache_events = False
        cache.eviction_strategy = LRUStrategy()
        cache.evictable_host_leaves = set()
        cache._diagnostic_storage_tombstones = OrderedDict()
        root = cache.root_node = TreeNode()
        root.key = RadixKey([])
        root.value = torch.empty(0, dtype=torch.int64)
        root.host_value = root.value
        for index in range(count):
            leaf = TreeNode()
            leaf.parent, leaf.key = root, RadixKey([index + 1] * page)
            leaf.host_value = torch.arange(page, dtype=torch.int64)
            leaf.host_ref_counter = int(index >= released)
            leaf.last_access_time = float(index)
            root.children[leaf.key.child_key(page)] = leaf
            cache.evictable_host_leaves.add(leaf)

        node = TreeNode()
        node.parent, node.key = root, RadixKey(list(range((count + 1) * page)))
        node.value = torch.arange(len(node.key), dtype=torch.int64)
        return cache, node

    original_affinity = os.sched_getaffinity(0)
    report: dict[str, Any] = dict(
        status="sampling",
        operation="eviction_backup_rejected",
        framework_entry="sglang.srt.mem_cache.hiradix_cache.HiRadixCache.write_backup",
        clock="thread_time_ns",
        batch=batch,
        repeats=repeats,
        target_workload_trace_used=False,
        target_score_used=False,
        samples=[],
        operator_observations=[],
        limitations=[
            "CPU-only failed allocation, host cleanup and retry; no successful write or DMA is measured.",
            "Flat host leaves with pinned and removable variants; deep host ancestor chains are not sampled.",
            "Thread CPU time excludes scheduler waits and inference contention.",
        ],
    )
    write_json(output, report)
    try:
        for rank, cpus in enumerate(cpu_sets):
            if not cpus or not cpus <= original_affinity:
                raise ValueError("Sampling CPUs must belong to the process allowed affinity")
            os.sched_setaffinity(0, cpus)
            for page in page_sizes:
                for count in (0, *heap_sizes):
                    for released in sorted({0, count // 2, count}):
                        for repeat in range(repeats):
                            durations = []
                            for _ in range(batch):
                                cache, node = initial_state(count, released, page)
                                started = time.thread_time_ns()
                                result = cache.write_backup(node, write_back=True)
                                duration = (time.thread_time_ns() - started) / 1000
                                if (
                                    result != 0
                                    or cache.cache_controller.mem_pool_host.available_size() != released * page
                                ):
                                    raise ValueError(
                                        "Rejected-backup primitive did not follow its declared CPU-only branch"
                                    )
                                durations.append(duration)

                            sample = dict(
                                rank=rank,
                                cpu_affinity=sorted(os.sched_getaffinity(0)),
                                page_size_tokens=page,
                                host_candidates=count,
                                released_candidates=released,
                                repeat=repeat,
                                durations_us=durations,
                            )
                            report["samples"].append(sample)
                            report["operator_observations"].append(
                                dict(
                                    position=f"rank{rank}:page{page}:heap{count}:released{released}:repeat{repeat}",
                                    operation="eviction.backup_rejected",
                                    implementation="sglang.HiRadixCache.write_backup",
                                    timing_scope="thread_cpu_service",
                                    features=dict(
                                        requested_tokens=len(node.key),
                                        page_size_tokens=page,
                                        host_candidates=count,
                                        host_released_tokens=released * page,
                                    ),
                                    duration_us=median(durations),
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


def capture_load_failure_returns(
    *,
    page_sizes: list[int],
    request_pages: list[int],
    prefix_depths: list[int],
    cpu_sets: list[set[int]],
    batch: int,
    repeats: int,
    output: Path,
) -> dict[str, Any]:
    """Measure the real load_back return after its final allocation failure.

    A controlled allocator returns None before the measured interval. The timer
    starts at dec_lock_ref, so neither allocation, index preparation nor eviction
    can enter this return cost. Warning logging remains enabled, as in inference.
    """

    import torch
    from sglang.srt.mem_cache.hiradix_cache import HiRadixCache
    from sglang.srt.mem_cache.radix_cache import RadixKey, TreeNode

    def initial_state(page: int, pages: int, depth: int) -> tuple[Any, Any, dict[str, int]]:
        cache = HiRadixCache.__new__(HiRadixCache)
        cache.disable, cache.page_size, cache.load_back_threshold = False, page, 0
        cache.evictable_size_, cache.protected_size_ = depth * page, 0
        cache.evictable_leaves, cache.evictable_host_leaves = set(), set()
        # Allocation failure is a precondition, not a measured allocator model.
        cache.cache_controller = SimpleNamespace(load=lambda **_: None)
        cache.evict = lambda *_: None
        root = cache.root_node = TreeNode()
        root.key, root.value = RadixKey([]), torch.empty(0, dtype=torch.int64)
        parent = root
        for _ in range(depth):
            node = TreeNode()
            node.parent, node.key = parent, RadixKey([1] * page)
            node.value = torch.arange(page, dtype=torch.int64)
            parent.children[node.key.child_key(page)] = node
            parent = node

        node = TreeNode()
        node.parent, node.key = parent, RadixKey([2] * (pages * page))
        node.host_value = torch.arange(pages * page, dtype=torch.int64)
        parent.children[node.key.child_key(page)] = node
        stamps: dict[str, int] = {}

        def release_prefix(prefix: Any) -> Any:
            stamps["wall"] = time.perf_counter_ns()
            return HiRadixCache.dec_lock_ref(cache, prefix)

        cache.dec_lock_ref = release_prefix
        return cache, node, stamps

    original_affinity = os.sched_getaffinity(0)
    report: dict[str, Any] = dict(
        status="sampling",
        operation="load_failure_return",
        framework_entry="sglang.srt.mem_cache.hiradix_cache.HiRadixCache.load_back",
        measured_entry="dec_lock_ref after the last failed load",
        clock="perf_counter_ns; measured control interval, CPU and logging waits are not separated",
        batch=batch,
        repeats=repeats,
        target_workload_trace_used=False,
        target_score_used=False,
        samples=[],
        operator_observations=[],
        limitations=[
            "A controlled failed allocator and eviction run before the timer; they supply no cost observations.",
            "Measures prefix unlock, host-reference release, warning and return; stderr is the logging sink.",
            "No inference contention. Prefix depths are sampled but are not a variable in the current target return descriptor.",
        ],
    )
    write_json(output, report)
    try:
        for rank, cpus in enumerate(cpu_sets):
            if not cpus or not cpus <= original_affinity:
                raise ValueError("Sampling CPUs must belong to the process allowed affinity")
            os.sched_setaffinity(0, cpus)
            for page in page_sizes:
                for pages in request_pages:
                    for depth in prefix_depths:
                        for repeat in range(repeats):
                            durations = []
                            for _ in range(batch):
                                cache, node, stamps = initial_state(page, pages, depth)
                                result = cache.load_back(node)
                                wall_us = (time.perf_counter_ns() - stamps["wall"]) / 1000
                                if result is not None or node.host_ref_counter != 0:
                                    raise ValueError(
                                        f"Load return branch mismatch: returned={result}, host_refs={node.host_ref_counter}"
                                    )
                                durations.append(wall_us)

                            position = f"rank{rank}:page{page}:pages{pages}:depth{depth}:repeat{repeat}"
                            features = dict(page_size_tokens=page, requested_tokens=pages * page, pages=pages)
                            report["samples"].append(
                                dict(
                                    position=position,
                                    cpu_affinity=sorted(os.sched_getaffinity(0)),
                                    prefix_depth=depth,
                                    features=features,
                                    durations_us=durations,
                                )
                            )
                            report["operator_observations"].append(
                                dict(
                                    position=position,
                                    operation="load.index.failure_tail",
                                    implementation="ascend.paged_unsorted",
                                    timing_scope="control_interval",
                                    features=features,
                                    duration_us=median(durations),
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


def capture_empty_prefix_returns(*, cpu_sets: list[set[int]], batch: int, repeats: int, output: Path) -> dict[str, Any]:
    """Measure empty NPU-index concatenation and the actual request bookkeeping API."""

    import torch
    import torch_npu  # Registers the deployed NPU implementation in this explicit sampler.
    from sglang.srt.managers.schedule_batch import Req

    original_affinity = os.sched_getaffinity(0)
    report: dict[str, Any] = dict(
        status="sampling",
        operation="empty_prefix_return",
        framework_entry=["torch.cat", "sglang.srt.managers.schedule_batch.Req.set_extend_input_len"],
        measured_entry="load-admission return to host-load branch return with two empty indices",
        clock="perf_counter_ns; local control interval, not pure thread CPU time",
        batch=batch,
        repeats=repeats,
        target_workload_trace_used=False,
        target_score_used=False,
        samples=[],
        operator_observations=[],
        limitations=[
            "Empty contiguous 1-D int64 NPU indices; no payload, device task or inference workload is measured.",
            "Includes concatenation, actual request-length update and prefix bookkeeping; default disabled logprobs only.",
            "Sequential deployment CPU/device scopes, without inference contention. Initialization and warmup are excluded.",
        ],
    )
    write_json(output, report)
    try:
        for rank, cpus in enumerate(cpu_sets):
            if not cpus or not cpus <= original_affinity:
                raise ValueError("Sampling CPUs must belong to the process allowed affinity")
            os.sched_setaffinity(0, cpus)
            torch_npu.npu.set_device(rank)
            empty = torch.empty(0, dtype=torch.int64, device=f"npu:{rank}")
            request = Req.__new__(Req)
            request.prefix_indices, request.logprob_start_len = empty, -1

            def run() -> None:
                # This is the four-line host-load return block in PrefillAdder.add_one_req.
                request.prefix_indices = torch.cat([request.prefix_indices, empty])
                request.set_extend_input_len(len(request.full_untruncated_fill_ids) - len(request.prefix_indices))
                prefix_len = len(request.prefix_indices)
                request.cache_protected_len = prefix_len

            for prompt_tokens in (64, 1024, 4096):
                request.full_untruncated_fill_ids = [0] * prompt_tokens
                for _ in range(batch):
                    run()
                torch_npu.npu.synchronize()

                for repeat in range(repeats):
                    started = time.perf_counter_ns()
                    for _ in range(batch):
                        run()
                    duration_us = (time.perf_counter_ns() - started) / (1000 * batch)
                    if (
                        request.prefix_indices.numel()
                        or request.extend_input_len != prompt_tokens
                        or request.cache_protected_len
                    ):
                        raise ValueError("Empty-prefix return did not preserve the expected request state")

                    report["samples"].append(
                        dict(
                            rank=rank,
                            cpu_affinity=sorted(cpus),
                            prompt_tokens=prompt_tokens,
                            repeat=repeat,
                            duration_us=duration_us,
                        )
                    )
                    report["operator_observations"].append(
                        dict(
                            position=f"rank{rank}:prompt{prompt_tokens}:repeat{repeat}",
                            operation="load.admission.prefix_concat",
                            implementation="torch_npu.cat.empty",
                            timing_scope="control_interval",
                            features=dict(prefix_tokens=0, new_tokens=0, output_tokens=0, nonempty_inputs=0),
                            duration_us=duration_us,
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


def main(argv: list[str] | None = None) -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--cpu-sets", required=True, help="One allowed CPU set per deployment rank, separated by |")
    parser.add_argument("--heap-sizes", default="1,8,64", help="Explicit primitive domain, not target observations")
    parser.add_argument("--batch", type=positive_int, default=128)
    parser.add_argument("--repeats", type=positive_int, default=7)
    parser.add_argument(
        "--operation",
        choices=("locked_candidate", "backup_rejected", "load_return", "empty_prefix_return"),
        default="locked_candidate",
    )
    parser.add_argument("--page-sizes", default="32,64,128", help="Declared CPU primitive domain for rejected backup")
    parser.add_argument("--request-pages", default="1,16,32,64", help="Declared load-return request sizes")
    parser.add_argument("--prefix-depths", default="0,1,8", help="Declared device-prefix node counts for load return")
    args = parser.parse_args(argv)
    if args.operation == "empty_prefix_return":
        capture_empty_prefix_returns(
            cpu_sets=parse_cpu_sets(args.cpu_sets, len(args.cpu_sets.split("|"))),
            batch=args.batch,
            repeats=args.repeats,
            output=args.output_dir / "empty_prefix_return.json",
        )
        return

    if args.operation == "load_return":
        capture_load_failure_returns(
            page_sizes=parse_positive_csv(args.page_sizes, "page-sizes"),
            request_pages=parse_positive_csv(args.request_pages, "request-pages"),
            prefix_depths=parse_integer_csv(args.prefix_depths, "prefix-depths", allow_zero=True),
            cpu_sets=parse_cpu_sets(args.cpu_sets, len(args.cpu_sets.split("|"))),
            batch=args.batch,
            repeats=args.repeats,
            output=args.output_dir / "load_return.json",
        )
        return

    if args.operation == "backup_rejected":
        capture_write_rejections(
            heap_sizes=parse_positive_csv(args.heap_sizes, "heap-sizes"),
            page_sizes=parse_positive_csv(args.page_sizes, "page-sizes"),
            cpu_sets=parse_cpu_sets(args.cpu_sets, len(args.cpu_sets.split("|"))),
            batch=args.batch,
            repeats=args.repeats,
            output=args.output_dir / "write_rejection.json",
        )
        return

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


def acquire_eviction_cpu(
    group: PhysicalCaptureInputs, needs: list[dict[str, Any]], *, dry_run: bool = False
) -> dict[str, Any]:
    """Acquire one declared primitive domain for all affected targets, not per cell."""
    from markov_internal.preparation.physical_capture import (
        physical_attempts,
        physical_usage,
        _completed,
        run_primitive_attempt,
    )
    from markov_internal.modeling.deployment import source_environment
    from markov_internal.modeling.source_dag.profiles import parse_server_command_flags
    from markov_internal.preparation.control_acquisition import operator_requirements

    required = operator_requirements(needs, "eviction.locked_candidate")
    required.extend(need for need in needs if need["component"] == "execution_control/eviction_locked_candidate")
    if not required:
        return dict(status="not_required")
    spec = group.capture
    sampling = spec.get("eviction_cpu")
    plan = dict(status="needs_cpu_primitive", requirements=required, usage=physical_usage(physical_attempts(group)))
    if not sampling or not spec.get("cpu_sets"):
        return dict(plan, stop_reason="declare physical_capture.eviction_cpu domain and cpu_sets")
    flags = parse_server_command_flags(group.sources[0].run_dir / "server_cmd.txt")
    if flags.get("radix_eviction_policy", "lru") != "lru":
        return dict(plan, stop_reason="CPU primitive sampler supports LRU only")
    cpus = parse_cpu_sets(spec["cpu_sets"], len(spec["cpu_sets"].split("|")))
    environment = source_environment(group.sources[0], group.resources)
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


def locked_candidate_observations(
    report: dict[str, Any], source: str, deployment: dict[str, Any]
) -> list[dict[str, Any]]:
    """Keep drained-loop observations, not invented per-pop measurements.

    A drain performs N pops and sum(log2(n)) heap work. Offline linear training
    can use these additive variables with a declared zero intercept, then query
    one pop as (1, log2(heap_size)). This is an explicitly reported extrapolation
    of primitive work, not a measurement of every individual pop.
    """

    if report["clock"] != "thread_time_ns" or report["status"] != "measured":
        raise ValueError("Locked-candidate observations require completed thread-CPU sampling")

    rows = []
    for sample in report["samples"]:
        size = sample["heap_size"]
        differences = [
            nonnegative_finite_number(pair["active_us"], "active CPU")
            - nonnegative_finite_number(pair["baseline_us"], "baseline CPU")
            for pair in sample["pairs"]
        ]
        duration = positive_finite_number(median(differences), "median loop CPU")
        rows.append(
            dict(
                capture="cpu_primitive:" + source,
                manifest=source,
                deployment=deployment,
                position=f"rank:{sample['rank']}:drain:{size}",
                operation="eviction.locked_candidate",
                implementation="lru_heap",
                timing_scope="thread_cpu_service",
                features={"pop_count": float(size), "sum_log2_heap": math.lgamma(size + 1) / math.log(2)},
                duration_us=duration,
            )
        )

    return rows


if __name__ == "__main__":
    main()
