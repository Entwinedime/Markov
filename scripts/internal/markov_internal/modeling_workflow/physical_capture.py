"""Group physical bootstrap: deployed-TP DMA and storage, with a separate budget."""

from __future__ import annotations

from dataclasses import asdict
import ast
import math
import os
from pathlib import Path
import tempfile
from typing import Any, TYPE_CHECKING

from ..common.io import load_json, write_json
from ..common.commands import nonnegative_int, positive_int
from ..common.paths import ROOT_DIR, repo_relative_path, require_repo_path
from ..contracts.forced_token.plan import load_forced_token_plan
from .calibration.options import format_cpu_set, parse_cpu_sets, parse_integer_csv
from .calibration.runtime_anchors import DMA_SERVICES, load_runtime_service_models
from .calibration.prefetch_service import prefetch_experiments, prefetch_io_bytes
from .capture import pending_group_capture, recorded_capture, run_container_attempt
from .physical_calibration import derive_kv_geometry
from .planning.profile_runs import parse_server_command_flags
from .runtime_dma_calibration import build_operation_plan
from .io_model_validation import required_prefetch_stages

if TYPE_CHECKING:
    from .group import GroupRequest


def physical_attempts(group: GroupRequest) -> list[dict[str, Any]]:
    path = group.output_dir / "physical_capture_ledger.json"
    return load_json(path)["attempts"] if path.exists() else []


def physical_usage(attempts: list[dict[str, Any]]) -> dict[str, Any]:
    return {
        "wall_seconds": sum(row.get("wall_seconds", 0) for row in attempts),
        "container_starts": len(attempts),
        "logical_io_bytes": sum(row["reserved_logical_io_bytes"] for row in attempts),
    }


def storage_batch_pages() -> int:
    """Read the deployed checkout's scalar without importing device libraries on the host."""
    source = ROOT_DIR / "third_party/sglang/python/sglang/srt/mem_cache/hicache_storage.py"
    for statement in ast.parse(source.read_text()).body:
        if isinstance(statement, ast.Assign) and any(
            isinstance(name, ast.Name) and name.id == "STORAGE_BATCH_SIZE" for name in statement.targets
        ):
            return positive_int(str(ast.literal_eval(statement.value)))
    raise ValueError("SGLang storage batch size is not statically declared; cannot reserve prefetch bytes")


def physical_context(group: GroupRequest) -> dict[str, Any]:
    """Admission uses workspace inputs; modeling need not mount model metadata/weights."""
    from .group import source_environment

    return {
        "sampling": _io_sampling(group.raw.get("physical_capture") or {}),
        "existing_physical": group.raw.get("physical_calibration"),
        "environment": source_environment(group.sources[0]),
        "page_token_sizes": _declared_page_sizes(group),
    }


def _io_sampling(spec: dict[str, Any]) -> dict[str, Any]:
    """I/O measurements do not depend on the budget or the separate CPU experiment."""
    return {key: value for key, value in spec.items() if key not in {"budget", "eviction_cpu"}}


def _platform_context(group: GroupRequest) -> dict[str, Any]:
    """Geometry and storage scopes depend on deployment, not measurement settings."""
    from .group import source_environment

    return {
        "environment": source_environment(group.sources[0]),
        "devices": (group.raw.get("physical_capture") or {}).get("devices"),
    }


def platform_inputs(group: GroupRequest) -> dict[str, Any] | None:
    """Read deployment metadata prepared on the host, never measured coefficients."""

    path = group.output_dir / "platform_inputs.json"
    if not path.is_file():
        return None
    document = load_json(path)
    return document if document["input_context"] == _platform_context(group) else None


def prepare_platform(group: GroupRequest) -> dict[str, Any]:
    """Expose model geometry to modeling without starting any measurement worker."""

    definition = physical_definition(group, service="metadata")
    if definition["limitations"]:
        return dict(status="needs_physical_calibration", limitations=definition["limitations"])

    write_json(
        group.output_dir / "platform_inputs.json",
        dict(
            input_context=definition["input_context"],
            kv_geometry=definition["geometry"],
            storage_batch_pages=definition["storage_batch_pages"],
            resource_lanes={"storage_read": "scope", "storage_write": "scope"},
            service_models={},
            measurement_sources=[],
            measurement_description="Deployment metadata only; no service cost has been measured",
            measurement_scope=dict(
                storage_scope_devices=definition["devices"],
                storage_scope_numa_nodes=definition["numa_nodes"] or [None] * len(definition["devices"]),
            ),
            target_workload_trace_used=False,
            target_e2e_used=False,
        ),
    )
    return dict(status="platform_ready")


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
        sets = [
            parse_cpu_sets(Path(f"/sys/devices/system/node/node{nodes[device]}/cpulist").read_text().strip(), 1)[0]
            & allowed
            for device in devices
        ]
    except OSError:
        return None
    return [format_cpu_set(value) for value in sets] if all(sets) else None


def physical_definition(
    group: GroupRequest, *, service: str, directions: tuple[str, ...] = tuple(DMA_SERVICES)
) -> dict[str, Any]:
    """Use deployment geometry, declared page paths and base inputs, never target labels."""
    spec = group.raw.get("physical_capture") or {}
    if set(spec) - {
        "budget",
        "devices",
        "cpu_sets",
        "page_token_sizes",
        "payload_bytes",
        "warmup",
        "repeats",
        "eviction_cpu",
    }:
        raise ValueError(
            "physical_capture accepts budget, devices/cpu_sets, page_token_sizes, payload_bytes, warmup and repeats"
        )
    source = group.sources[0]
    flags = parse_server_command_flags(source.run_dir / "server_cmd.txt")
    if (flags.get("hicache_io_backend"), flags.get("hicache_mem_layout"), flags.get("hicache_storage_backend")) != (
        "kernel_ascend",
        "page_first_direct",
        "file",
    ):
        return {"limitations": ["physical bootstrap currently measures kernel_ascend/page_first_direct/file only"]}
    tp = positive_int(flags.get("tp_size", "1"))
    model_config = Path(flags["model_path"]) / "config.json"
    try:
        geometry = derive_kv_geometry(model_config, tp, 0)
    except FileNotFoundError:
        return {
            "limitations": [
                f"Model metadata unavailable at {model_config}; provide an applicable physical_calibration report or make the base model config available on the host."
            ]
        }
    if geometry["kv_element_bytes"] != 2 or flags.get("kv_cache_dtype", "auto") not in {"auto", "bfloat16", "float16"}:
        return {"limitations": ["runtime DMA sampler currently requires two-byte KV elements"]}
    start = int(flags.get("base_gpu_id", "0"))
    step = int(flags.get("gpu_id_step", "1"))
    devices = parse_integer_csv(
        ",".join(str(value) for value in spec.get("devices", range(start, start + tp * step, step))),
        "devices",
        allow_zero=True,
    )
    if len(devices) != tp:
        raise ValueError("physical devices must contain the base deployment TP scope count")
    nodes = [int(value) for value in flags.get("numa_node", "").split()]
    if nodes and any(device >= len(nodes) for device in devices):
        return {"limitations": ["base NUMA node mapping does not cover calibration devices"]}
    numa_nodes = [nodes[device] for device in devices] if nodes else None
    batch_pages = storage_batch_pages()
    definition = {
        "input_context": _platform_context(group),
        "geometry": geometry,
        "devices": devices,
        "numa_nodes": numa_nodes,
        "storage_batch_pages": batch_pages,
        "limitations": [],
    }
    # Geometry has no timing samples. Do not require a runnable sampling
    # placement until an execution demand actually needs independent costs.
    if service == "metadata":
        return definition

    definition["input_context"] = physical_context(group)
    if str(load_json(source.config_path).get("env", {}).get("SGLANG_NUMA_BIND_V2", "0")).lower() in {"1", "true"}:
        return {"limitations": ["physical bootstrap implements preferred-node policy, not SGLang numactl membind"]}
    cpu_sets = deployment_cpu_sets(flags, devices, spec.get("cpu_sets"))
    if cpu_sets is None:
        return {"limitations": ["base NUMA CPU placement is unresolved; declare physical_capture.cpu_sets"]}

    pages = _declared_page_sizes(group)
    kv = geometry["kv_bytes_per_token_per_rank"]
    page_bytes = [page * kv for page in pages]
    warmup, repeats = nonnegative_int(str(spec.get("warmup", 1))), positive_int(str(spec.get("repeats", 3)))
    definition.update(
        {
            "cpu_sets": cpu_sets,
            "page_token_sizes": pages,
            "cpu_binding_basis": "explicit_sampling_declaration"
            if spec.get("cpu_sets")
            else "base_numa_policy_on_current_host",
            "warmup": warmup,
            "repeats": repeats,
            "logical_io_bytes": {},
            "peak_storage_payload_bytes": 0,
            "io_accounting": "API-attempted transfer/read/write bytes, including population and warmup; not physical disk traffic",
        }
    )
    if service == "prefetch":
        definition["logical_io_bytes"]["prefetch"] = prefetch_io_bytes(page_bytes, batch_pages, tp, warmup, repeats)
        definition["peak_storage_payload_bytes"] = tp * max(
            size * count for size, count, _ in prefetch_experiments(page_bytes, batch_pages)
        )
        return definition

    quantum = math.lcm(*page_bytes)
    token_plans = [group.token_plan(workload) for workload in group.workload_ids]
    max_tokens = max(
        [
            len(row["origin_input_ids"])
            for path in token_plans
            if path is not None
            for row in load_forced_token_plan(path)["requests"]
        ]
        or [int(source.hicache_config["l1_capacity_pages"]) * int(source.hicache_config["page_size"])]
    )
    payload = positive_int(
        str(spec.get("payload_bytes", max(2 * quantum, math.ceil(max_tokens * kv / quantum) * quantum)))
    )
    if payload < 2 * quantum or payload % quantum:
        raise ValueError("physical payload must be page-aligned and provide a distinct sustained anchor")
    if service == "dma":
        operations = build_operation_plan(
            page_token_sizes=pages,
            payload_bytes=[payload],
            kv_bytes_per_token_per_rank=kv,
            repeats=1,
            directions=directions,
        )
        definition.update(
            payload_bytes=payload,
            directions=directions,
            logical_io_bytes={"dma": sum(row.byte_count for row in operations) * tp * (warmup + repeats)},
        )
        return definition

    existing = [1, max(2, payload // max(page_bytes))]
    existing_bytes = sum(page_bytes) * sum(existing) * tp * (1 + warmup + repeats)
    if service == "existing_write":
        definition.update(
            existing_operation_pages=existing,
            logical_io_bytes={"existing_write": existing_bytes},
            peak_storage_payload_bytes=tp * max(page_bytes) * max(existing),
        )
        return definition

    new_operations, queue = [quantum, payload], 2 * payload
    # Selection already established the missing coefficients. Planning sizes
    # must not independently refit the base or override that evidence decision.
    new_bytes = len(pages) * len(new_operations) * queue * tp * (warmup + repeats)
    definition.update(
        {
            "payload_bytes": payload,
            "new_operation_bytes": new_operations,
            "queue_bytes": queue,
            "peak_storage_payload_bytes": tp * queue,
            "payload_basis": "declared payload or base maximum input token bytes, rounded to common page geometry",
        }
    )
    definition["logical_io_bytes"] = {"new_write": new_bytes}
    return definition


def _completed(attempts: list[dict[str, Any]], definition: dict[str, Any], stage: str) -> dict[str, Any] | None:
    def matches(row):
        previous = row["definition"]
        if stage == "eviction_cpu":
            return previous == definition
        # The base report only carries other already-measured services into the
        # output. A later supplement changes that path, not this experiment.
        # Compare sampling semantics, not the spelling of a generated CLI.
        if definition["base_report"] != previous["base_report"] and definition["base_report"] != row["report"]:
            sources = load_json(require_repo_path(definition["base_report"]))["measurement_sources"]
            if row["report"] not in sources:
                return False
        fields = ["geometry", "devices", "numa_nodes", "cpu_sets", "page_token_sizes", "warmup", "repeats"]
        fields.extend(
            {
                "dma": ["payload_bytes"],
                "prefetch": ["storage_batch_pages"],
                "existing_write": ["existing_operation_pages", "storage_batch_pages"],
                "new_write": ["new_operation_bytes", "queue_bytes", "storage_batch_pages"],
            }[stage]
        )
        return (
            previous["input_context"]["environment"] == definition["input_context"]["environment"]
            and all(previous[key] == definition[key] for key in fields)
            and (stage != "dma" or tuple(previous["directions"]) == tuple(definition["directions"]))
        )

    return next(
        (
            row
            for row in reversed(attempts)
            if row["stage"] == stage
            and row["status"] == "completed"
            and matches(row)
            and require_repo_path(row["report"]).is_file()
        ),
        None,
    )


def captured_physical_declaration(group: GroupRequest) -> dict[str, Any] | None:
    # Metadata-only preparation declares no sampling domain to match against
    # previous I/O attempts. Explicit physical_calibration remains independent.
    if not (group.raw.get("physical_capture") or {}).get("page_token_sizes"):
        return None

    context = None
    for row in reversed(physical_attempts(group)):
        if (
            row["stage"] not in {"storage", "dma", "prefetch", "existing_write", "new_write"}
            or row["status"] != "completed"
        ):
            continue
        if context is None:
            context = physical_context(group)

        # Token paths and CPU sampling settings do not determine I/O applicability.
        previous = {key: value for key, value in row["definition"]["input_context"].items() if key != "token_inputs"}
        previous["sampling"] = _io_sampling(previous["sampling"])
        path = require_repo_path(row["report"])
        if previous != context or not path.is_file():
            continue

        report = load_json(path)
        return {
            "report": row["report"],
            "measurement_sources": report["measurement_sources"],
            "measurement_description": "This group's budgeted deployed-TP DMA and HiCacheFile primitives; no inference workloads.",
        }
    return None


def physical_command(definition: dict[str, Any], stage: str, output: Path) -> list[str]:
    def csv(values):
        return ",".join(str(value) for value in values)

    common = [
        "--output-dir",
        str(repo_relative_path(output / "result")),
        "--page-token-sizes",
        csv(definition["page_token_sizes"]),
        "--warmup",
        str(definition["warmup"]),
        "--repeats",
        str(definition["repeats"]),
    ]
    if stage == "dma":
        options = [
            "--cpu-sets",
            "|".join(definition["cpu_sets"]),
            "--payload-bytes",
            str(definition["payload_bytes"]),
        ]
        kind = "runtime-dma"
        options.extend(("--base-report", definition["base_report"]))
        options.extend(("--directions", *definition["directions"]))
    else:
        options = [
            "--storage-dir",
            str(repo_relative_path(output / "storage_work")),
            "--storage-scope-cpu-sets",
            "|".join(definition["cpu_sets"]),
        ]
        kind = "physical"
        options.extend(("--base-report", definition["base_report"], "--service", stage))
        if stage == "new_write":
            options.extend(
                (
                    "--storage-new-write-operation-bytes-per-scope",
                    csv(definition["new_operation_bytes"]),
                    "--storage-new-write-queue-bytes-per-scope",
                    str(definition["queue_bytes"]),
                )
            )
        if stage == "existing_write":
            options.extend(("--storage-existing-operation-pages", csv(definition["existing_operation_pages"])))
    return [str(ROOT_DIR / "scripts/model.sh"), "calibrate-hicache", kind, *common, *options]


SERVICE_SAMPLERS = {
    "physical/load": "dma",
    "physical/write_device_to_host": "dma",
    "physical/prefetch_stages": "prefetch",
    "physical/write_host_to_storage_existing": "existing_write",
    "service/write_host_to_storage_new": "new_write",
}


def capture_physical(group: GroupRequest, *, dry_run: bool, required_components: set[str]) -> dict[str, Any]:
    """Acquire requested services serially, preserving each completed supplement."""
    # Group loading reads capture declarations; import only on the execution path.
    from .group import GroupRequest

    attempts = physical_attempts(group)
    plan = {
        "status": "needs_physical_calibration",
        "requirements": [],
        "physical_capture_budget": asdict(group.physical_budget) if group.physical_budget else None,
        "physical_capture_usage": physical_usage(attempts),
    }
    # Only these supplements have a service-specific sampler. Do not silently
    # replace an incomplete report with a full capture or claim it is complete.
    if not required_components:
        return {**plan, "status": "physical_captured", "physical_stages": []}
    if required_components == {"physical"}:
        return dict(plan, stop_reason="platform_inputs_required")
    unsupported = required_components - SERVICE_SAMPLERS.keys()
    if unsupported:
        return dict(plan, stop_reason="service_sampler_unavailable", requirements=sorted(unsupported))

    if "page_token_sizes" not in (group.raw.get("physical_capture") or {}):
        return dict(
            plan,
            stop_reason="physical_capture_declaration_required",
            requirements=["Declare physical_capture.page_token_sizes before requesting I/O measurement."],
        )
    if group.physical is None:
        return dict(plan, stop_reason="platform_inputs_required")
    directions = tuple(
        direction for direction, kind in DMA_SERVICES.items() if "physical/" + kind in required_components
    )
    selected = dict.fromkeys(
        service for component, service in SERVICE_SAMPLERS.items() if component in required_components
    )
    completed_components = []
    stages = []
    budget_denied = None
    for supplement in selected:
        result = {**plan, **_capture_physical_service(group, supplement, directions=directions, dry_run=dry_run)}
        result["physical_capture_usage"] = physical_usage(physical_attempts(group))
        stages.extend(result.get("physical_stages", []))
        result.update(physical_stages=stages, completed_components=completed_components)
        if dry_run and result.get("stop_reason") == "dry_run":
            continue
        if result["status"] == "physical_budget_exhausted":
            # Denial applies to this experiment, not all uses of the ledger.
            # Later services may be smaller or already measured.
            budget_denied = budget_denied or result
            continue
        if result["status"] != "physical_captured":
            return result

        completed_components.extend(
            component for component in sorted(required_components) if SERVICE_SAMPLERS[component] == supplement
        )
        # Each sampler extends the preceding report, never the original snapshot.
        if len(completed_components) < len(required_components):
            group = GroupRequest.load(group.path)

    if budget_denied is not None:
        budget_denied["physical_capture_usage"] = result["physical_capture_usage"]
        return budget_denied
    return result


def _capture_physical_service(
    group: GroupRequest, supplement: str, *, directions: tuple[str, ...], dry_run: bool
) -> dict[str, Any]:
    attempts = physical_attempts(group)
    definition = physical_definition(group, service=supplement, directions=directions)
    plan = {"status": "needs_physical_calibration", "bootstrap": definition}
    if definition["limitations"]:
        plan["stop_reason"] = "physical_sampler_limitations"
        return plan
    declaration = captured_physical_declaration(group) or group.raw.get("physical_calibration")
    if declaration is None and platform_inputs(group) is not None:
        declaration = {"report": str(repo_relative_path(group.output_dir / "platform_inputs.json"))}
    if declaration is None:
        plan["stop_reason"] = "existing_physical_report_required"
        return plan
    definition["base_report"] = declaration["report"]
    dimensions = (
        "num_hidden_layers",
        "num_key_value_heads_per_rank",
        "head_dim",
        "kv_element_bytes",
        "tensor_parallel_size",
    )
    if any(group.physical["kv_geometry"][key] != definition["geometry"][key] for key in dimensions):
        plan["stop_reason"] = "existing_physical_geometry_differs_from_base"
        return plan
    scope = group.physical.get("measurement_scope", {})
    if scope.get("storage_scope_devices") != definition["devices"] or scope.get("storage_scope_numa_nodes") != (
        definition["numa_nodes"] or [None] * len(definition["devices"])
    ):
        plan["stop_reason"] = "existing_storage_placement_missing_or_different"
        plan["requirements"] = [
            "Existing physical report must record storage_scope_devices and storage_scope_numa_nodes "
            "in measurement_scope, matching the declared sampling placement; do not infer missing measurements."
        ]
        return plan
    if _completed(attempts, definition, supplement) is not None:
        return dict(plan, status="physical_captured", physical_stages=[])

    plan["physical_stages"] = [{"stage": supplement, "logical_io_bytes": definition["logical_io_bytes"][supplement]}]
    if dry_run:
        plan["stop_reason"] = "dry_run"
        return plan

    def validate(report: Path) -> int:
        observed = load_json(report)
        if observed.get("target_workload_trace_used") is not False or observed.get("target_e2e_used") is not False:
            raise ValueError("physical capture did not produce an independent primitive report")

        if supplement == "dma":
            dma_path = report.parent / "runtime_dma_calibration.json"
            load_runtime_service_models(
                dma_path,
                expected_kv_bytes_per_token_per_rank=definition["geometry"]["kv_bytes_per_token_per_rank"],
                expected_page_bytes=[
                    page * definition["geometry"]["kv_bytes_per_token_per_rank"]
                    for page in definition["page_token_sizes"]
                ],
                expected_concurrent_scope_count=len(definition["devices"]),
                directions=tuple(definition["directions"]),
            )
            return len(load_json(dma_path)["samples"])

        if supplement == "prefetch":
            required_prefetch_stages(observed["service_models"]["prefetch"]["stages"])
        changed = "prefetch" if supplement == "prefetch" else "write_host_to_storage"
        if any(
            observed["service_models"][kind] != value
            for kind, value in group.physical["service_models"].items()
            if kind != changed
        ):
            raise ValueError("Partial service capture changed unrelated service costs")

        samples = load_json(report.parent / "physical_observations.json")
        return len(
            samples["prefetch_service_observations"] if supplement == "prefetch" else samples["host_storage"]["samples"]
        )

    row = run_primitive_attempt(
        group,
        definition,
        supplement,
        lambda output: physical_command(definition, supplement, output),
        "calibration_report.json",
        validate,
        environment={"ASCEND_VISIBLE_DEVICES": ",".join(str(value) for value in definition["devices"])},
    )
    plan["status"] = "physical_captured" if row["status"] == "completed" else row["status"]
    if row["status"] != "completed":
        plan.update(failed_stage=supplement, attempt=row)
    return plan


def run_primitive_attempt(group, definition, stage, command_for_output, report_name, validate, *, environment=None):
    """One budget and durable ledger for physical and CPU primitive measurements."""
    attempts = physical_attempts(group)
    if reason := pending_group_capture(group.output_dir):
        return dict(status="capture_incomplete", stop_reason=reason)
    budget = group.physical_budget
    if budget is None:
        return dict(status="physical_budget_required")
    usage = physical_usage(attempts)
    io_bytes = definition["logical_io_bytes"][stage]
    remaining, limits = budget.available_for(usage, container_starts=1, logical_io_bytes=io_bytes)
    if limits:
        return dict(status="physical_budget_exhausted", limits=limits)
    root = group.output_dir / "physical_captures"
    root.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix=f"{stage}_", dir=root))
    command = command_for_output(output)
    report = output / "result" / report_name
    row = dict(
        stage=stage,
        definition=definition,
        report=str(repo_relative_path(report)),
        command=command,
        command_log=str(repo_relative_path(output / "command.log")),
        reserved_logical_io_bytes=io_bytes,
    )
    ledger = group.output_dir / "physical_capture_ledger.json"
    with recorded_capture(ledger, dict(attempts=attempts), row, physical_usage, kind="physical"):
        print(f"primitive {stage}: starting; remaining wall budget {remaining:.1f}s", flush=True)
        run_container_attempt(row, command, remaining, environment=environment)
        if row["status"] == "completed":
            try:
                row["sample_count"] = validate(report)
                if not row["sample_count"]:
                    raise ValueError("Primitive capture has no completed samples")
            except (OSError, ValueError, KeyError, TypeError) as error:
                row.update(status="invalid_capture_output", artifact_error=str(error))
    return row
