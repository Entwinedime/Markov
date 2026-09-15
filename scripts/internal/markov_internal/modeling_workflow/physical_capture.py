"""Group physical bootstrap: deployed-TP DMA and storage, with a separate budget."""

from __future__ import annotations

from dataclasses import asdict
import math
import os
from pathlib import Path
import tempfile
import time
from typing import Any, TYPE_CHECKING

from ..common.io import load_json, write_json
from ..common.paths import ROOT_DIR, repo_relative_path, require_repo_path
from ..contracts.forced_token.plan import load_forced_token_plan
from .calibration.options import format_cpu_set, parse_cpu_sets, parse_integer_csv
from .calibration.runtime_anchors import load_runtime_anchor_projection
from .capture import has_pending_capture, run_container_attempt
from .physical_calibration import derive_kv_geometry, nonnegative_int, positive_int
from .planning.profile_runs import parse_server_command_flags
from .runtime_dma_calibration import build_operation_plan

if TYPE_CHECKING:
    from .group import GroupRequest


def physical_attempts(group: GroupRequest) -> list[dict[str, Any]]:
    path = group.output_dir / "physical_capture_ledger.json"
    return load_json(path)["attempts"] if path.exists() else []


def physical_usage(attempts: list[dict[str, Any]]) -> dict[str, Any]:
    return {"wall_seconds": sum(row.get("wall_seconds", 0) for row in attempts),
            "container_starts": len(attempts),
            "logical_io_bytes": sum(row["reserved_logical_io_bytes"] for row in attempts)}


def physical_context(group: GroupRequest) -> dict[str, Any]:
    """Admission uses workspace inputs; modeling need not mount model metadata/weights."""
    from .group import source_environment
    return {"sampling": {key: value for key, value in (group.raw.get("physical_capture") or {}).items() if key != "budget"},
            "environment": source_environment(group.sources[0]),
            "page_token_sizes": _declared_page_sizes(group),
            "token_inputs": [str(repo_relative_path(path)) if path is not None else None
                             for path in (group.token_plan(workload) for workload in group.workload_ids)]}


def _declared_page_sizes(group: GroupRequest) -> list[int]:
    """Return the platform sampling domain without consulting prediction targets."""

    value = (group.raw.get("physical_capture") or {}).get("page_token_sizes")
    if not isinstance(value, list) or not value:
        raise ValueError("physical_capture.page_token_sizes must declare a target-independent platform domain")
    pages = sorted({positive_int(str(page)) for page in value})
    if len(pages) != len(value):
        raise ValueError("physical_capture.page_token_sizes must be unique")
    return pages


def deployment_cpu_sets(flags: dict[str, str], devices: list[int], explicit: str | None) -> list[str] | None:
    """Honor explicit sampling placement or resolve the base's NUMA CPU policy.

    SGLang's CPU-backend OMP binding is not the NPU scheduler's placement.
    Derived sets describe configuration on this host, not observed affinity.
    """
    if explicit:
        return [format_cpu_set(value) for value in parse_cpu_sets(explicit, len(devices))]
    nodes = [int(value) for value in flags.get("numa_node", "").split()]
    if not nodes or any(device >= len(nodes) for device in devices):
        return None
    try:
        allowed = os.sched_getaffinity(0)
        sets = [parse_cpu_sets(Path(f"/sys/devices/system/node/node{nodes[device]}/cpulist").read_text().strip(), 1)[0]
                & allowed for device in devices]
    except OSError:
        return None
    return [format_cpu_set(value) for value in sets] if all(sets) else None


def physical_definition(group: GroupRequest) -> dict[str, Any]:
    """Use deployment geometry, declared page paths and base inputs, never target labels."""
    spec = group.raw.get("physical_capture") or {}
    if set(spec) - {"budget", "devices", "cpu_sets", "page_token_sizes", "payload_bytes", "warmup", "repeats"}:
        raise ValueError(
            "physical_capture accepts budget, devices/cpu_sets, page_token_sizes, payload_bytes, warmup and repeats"
        )
    source = group.sources[0]
    flags = parse_server_command_flags(source.run_dir / "server_cmd.txt")
    if str(load_json(source.config_path).get("env", {}).get("SGLANG_NUMA_BIND_V2", "0")).lower() in {"1", "true"}:
        return {"limitations": ["physical bootstrap implements preferred-node policy, not SGLang numactl membind"]}
    if (flags.get("hicache_io_backend"), flags.get("hicache_mem_layout"), flags.get("hicache_storage_backend")) != (
            "kernel_ascend", "page_first_direct", "file"):
        return {"limitations": ["physical bootstrap currently measures kernel_ascend/page_first_direct/file only"]}
    tp = positive_int(flags.get("tp_size", "1"))
    geometry = derive_kv_geometry(Path(flags["model_path"]) / "config.json", tp, 0)
    if geometry["kv_element_bytes"] != 2 or flags.get("kv_cache_dtype", "auto") not in {"auto", "bfloat16", "float16"}:
        return {"limitations": ["runtime DMA sampler currently requires two-byte KV elements"]}
    start = int(flags.get("base_gpu_id", "0"))
    step = int(flags.get("gpu_id_step", "1"))
    devices = parse_integer_csv(",".join(str(value) for value in spec.get("devices", range(start, start + tp * step, step))),
                                "devices", allow_zero=True)
    if len(devices) != tp:
        raise ValueError("physical devices must contain the base deployment TP scope count")
    cpu_sets = deployment_cpu_sets(flags, devices, spec.get("cpu_sets"))
    if cpu_sets is None:
        return {"limitations": ["base NUMA CPU placement is unresolved; declare physical_capture.cpu_sets"]}
    nodes = [int(value) for value in flags.get("numa_node", "").split()]
    if nodes and any(device >= len(nodes) for device in devices):
        return {"limitations": ["base NUMA node mapping does not cover calibration devices"]}
    numa_nodes = [nodes[device] for device in devices] if nodes else None
    pages = _declared_page_sizes(group)
    kv = geometry["kv_bytes_per_token_per_rank"]
    page_bytes = [page * kv for page in pages]
    quantum = math.lcm(*page_bytes)
    token_plans = [group.token_plan(workload) for workload in group.workload_ids]
    max_tokens = max([len(row["origin_input_ids"]) for path in token_plans if path is not None
                      for row in load_forced_token_plan(path)["requests"]]
                     or [int(source.hicache_config["l1_capacity_pages"]) * int(source.hicache_config["page_size"])])
    payload = positive_int(str(spec.get("payload_bytes", max(2 * quantum, math.ceil(max_tokens * kv / quantum) * quantum))))
    if payload < 2 * quantum or payload % quantum:
        raise ValueError("physical payload must be page-aligned and provide a distinct sustained anchor")
    warmup, repeats = nonnegative_int(str(spec.get("warmup", 1))), positive_int(str(spec.get("repeats", 3)))
    existing = [1, max(2, payload // max(page_bytes))]
    new_operations, queue = [quantum, payload], 2 * payload
    dma = build_operation_plan(page_token_sizes=pages, payload_bytes=[payload], kv_bytes_per_token_per_rank=kv, repeats=1)
    io_bytes = {
        "tp_dma": sum(row.byte_count for row in dma) * tp * (warmup + repeats),
        "storage": (3 * sum(page_bytes) * tp * (warmup + repeats)
                    + sum(page_bytes) * sum(existing) * tp * (1 + warmup + repeats)
                    + len(pages) * len(new_operations) * queue * tp * (warmup + repeats)),
    }
    return {"input_context": physical_context(group), "geometry": geometry, "devices": devices, "cpu_sets": cpu_sets, "page_token_sizes": pages,
            "cpu_binding_basis": "explicit_sampling_declaration" if spec.get("cpu_sets") else "base_numa_policy_on_current_host",
            "numa_nodes": numa_nodes,
            "payload_bytes": payload, "warmup": warmup, "repeats": repeats,
            "existing_operation_pages": existing, "new_operation_bytes": new_operations, "queue_bytes": queue,
            "logical_io_bytes": io_bytes, "peak_storage_payload_bytes": tp * max(queue, max(page_bytes) * max(existing)),
            "payload_basis": "declared payload or base maximum input token bytes, rounded to common page geometry",
            "io_accounting": "API-attempted transfer/read/write bytes, including population and warmup; not physical disk traffic",
            "limitations": []}


def _completed(attempts: list[dict[str, Any]], definition: dict[str, Any], stage: str) -> dict[str, Any] | None:
    return next((row for row in reversed(attempts) if row["stage"] == stage and row["status"] == "completed"
                 and row["definition"] == definition and require_repo_path(row["report"]).is_file()), None)


def captured_physical_declaration(group: GroupRequest) -> dict[str, Any] | None:
    attempts = physical_attempts(group)
    if not attempts or group.raw.get("physical_capture") is None:
        return None
    context = physical_context(group)
    row = next((row for row in reversed(attempts) if row["stage"] == "storage" and row["status"] == "completed"
                and row["definition"]["input_context"] == context and require_repo_path(row["report"]).is_file()), None)
    if row is None:
        return None
    report = load_json(require_repo_path(row["report"]))
    return {"report": row["report"], "measurement_sources": report["measurement_sources"],
            "measurement_description": "This group's budgeted deployed-TP DMA and HiCacheFile primitives; no inference workloads."}


def physical_command(definition: dict[str, Any], stage: str, output: Path, dma_report: str | None = None) -> list[str]:
    def csv(values):
        return ",".join(str(value) for value in values)
    geometry = definition["geometry"]
    common = ["--output-dir", str(repo_relative_path(output / "result")),
              "--page-token-sizes", csv(definition["page_token_sizes"]),
              "--warmup", str(definition["warmup"]), "--repeats", str(definition["repeats"])]
    if stage == "tp_dma":
        options = ["--devices", csv(definition["devices"]), "--cpu-sets", "|".join(definition["cpu_sets"]),
                   "--payload-bytes", str(definition["payload_bytes"]), "--layer-count", str(geometry["num_hidden_layers"]),
                   "--kv-heads-per-rank", str(geometry["num_key_value_heads_per_rank"]), "--head-dim", str(geometry["head_dim"]),
                   "--element-bytes", str(geometry["kv_element_bytes"])]
        kind = "runtime-dma"
        if definition.get("numa_nodes") is not None:
            options += ["--numa-nodes", csv(definition["numa_nodes"])]
    else:
        options = ["--model-config", geometry["model_config_path"], "--tensor-parallel-size", str(geometry["tensor_parallel_size"]),
                   "--runtime-dma-report", dma_report or "<completed TP DMA report>",
                   "--storage-dir", str(repo_relative_path(output / "storage_work")),
                   "--storage-scope-cpu-sets", "|".join(definition["cpu_sets"]), "--isolated-repeats", str(definition["repeats"]),
                   "--storage-existing-operation-pages", csv(definition["existing_operation_pages"]),
                   "--storage-new-write-operation-bytes-per-scope", csv(definition["new_operation_bytes"]),
                   "--storage-new-write-queue-bytes-per-scope", str(definition["queue_bytes"])]
        kind = "physical"
    return [str(ROOT_DIR / "scripts/model.sh"), "calibrate-hicache", kind, *common, *options]


def capture_physical(group: GroupRequest, *, dry_run: bool) -> dict[str, Any]:
    definition = physical_definition(group)
    attempts = physical_attempts(group)
    budget = group.physical_budget
    plan = {"status": "needs_physical_calibration", "requirements": [], "bootstrap": definition,
            "physical_capture_budget": asdict(budget) if budget else None, "physical_capture_usage": physical_usage(attempts)}
    if definition["limitations"]:
        plan["stop_reason"] = "physical_sampler_limitations"
        return plan
    stages = [stage for stage in ("tp_dma", "storage") if _completed(attempts, definition, stage) is None]
    plan["physical_stages"] = [{"stage": stage, "logical_io_bytes": definition["logical_io_bytes"][stage]} for stage in stages]
    if dry_run or budget is None:
        plan["stop_reason"] = "dry_run" if dry_run else "physical_budget_required"
        return plan
    if has_pending_capture(group.output_dir):
        plan.update(status="capture_incomplete", stop_reason="inspect recorded live group container before resuming")
        return plan
    ledger_path = group.output_dir / "physical_capture_ledger.json"
    for stage in stages:
        usage = physical_usage(attempts)
        remaining = budget.wall_seconds - usage["wall_seconds"]
        limits = [key for key, extra in (("container_starts", 1), ("logical_io_bytes", definition["logical_io_bytes"][stage]))
                  if usage[key] + extra > getattr(budget, key)]
        if remaining <= 30:
            limits.append("wall_seconds")
        if limits:
            plan.update(status="physical_budget_exhausted", limits=limits)
            return plan
        root = group.output_dir / "physical_captures"
        root.mkdir(parents=True, exist_ok=True)
        output = Path(tempfile.mkdtemp(prefix=f"{stage}_", dir=root))
        dma = _completed(attempts, definition, "tp_dma")
        command = physical_command(definition, stage, output, dma["report"] if dma else None)
        report = output / "result" / ("runtime_dma_calibration.json" if stage == "tp_dma" else "calibration_report.json")
        row = {"stage": stage, "status": "running", "definition": definition, "report": str(repo_relative_path(report)),
               "command": command, "command_log": str(repo_relative_path(output / "command.log")),
               "container": f"markov-physical-{os.getpid()}-{time.time_ns()}", "started_at_unix": time.time(),
               "reserved_logical_io_bytes": definition["logical_io_bytes"][stage]}
        attempts.append(row)
        write_json(ledger_path, {"attempts": attempts, "usage": physical_usage(attempts)})
        print(f"physical {stage}: starting; remaining wall budget {remaining:.1f}s", flush=True)
        try:
            run_container_attempt(row, command, remaining,
                                  environment={"ASCEND_VISIBLE_DEVICES": ",".join(str(value) for value in definition["devices"])})
            if row["status"] == "completed":
                try:
                    observed = load_json(report)
                    if observed.get("target_workload_trace_used") is not False or observed.get("target_e2e_used") is not False:
                        raise ValueError("physical capture did not produce an independent primitive report")
                    if stage == "tp_dma":
                        load_runtime_anchor_projection(report,
                            expected_kv_bytes_per_token_per_rank=definition["geometry"]["kv_bytes_per_token_per_rank"],
                            expected_page_bytes=[page * definition["geometry"]["kv_bytes_per_token_per_rank"]
                                                 for page in definition["page_token_sizes"]],
                            expected_concurrent_scope_count=len(definition["devices"]))
                    row["sample_count"] = len(observed.get("samples", [])) if stage == "tp_dma" else len(
                        load_json(output / "result/physical_observations.json")["host_storage"]["samples"])
                    if row["sample_count"] == 0:
                        raise ValueError("physical capture has no completed primitive samples")
                except (OSError, ValueError, KeyError, TypeError) as error:
                    row.update(status="invalid_capture_output", artifact_error=str(error))
        finally:
            row["output_bytes"] = sum(path.stat().st_size for path in output.rglob("*") if path.is_file())
            plan["physical_capture_usage"] = physical_usage(attempts)
            write_json(ledger_path, {"attempts": attempts, "usage": plan["physical_capture_usage"]})
        if row["status"] != "completed":
            plan.update(status=row["status"], failed_stage=stage)
            return plan
    plan["status"] = "physical_captured"
    return plan
