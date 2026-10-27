"""Target-independent deployed-TP DMA and storage sampling with a separate budget."""

from __future__ import annotations

from dataclasses import asdict, dataclass, replace
import ast
from functools import cached_property
import math
import os
from pathlib import Path
import tempfile
from typing import Any, Callable

from markov_internal.common.io import load_json, write_json
from markov_internal.common.commands import nonnegative_int, positive_int
from markov_internal.common.paths import ROOT_DIR, repo_relative_path, require_repo_path
from markov_internal.contracts.forced_token.plan import load_forced_token_plan
from markov_internal.preparation.calibration.options import format_cpu_set, parse_cpu_sets, parse_integer_csv
from markov_internal.modeling.cost.io_contract import DMA_SERVICES
from markov_internal.modeling.cost.physical import read_physical_measurements
from markov_internal.preparation.calibration.prefetch_service import prefetch_experiments, prefetch_io_bytes
from markov_internal.preparation.profile_capture import profile_token_plan
from markov_internal.preparation.capture import (
    PhysicalBudget,
    pending_group_capture,
    recorded_capture,
    run_container_attempt,
)
from markov_internal.preparation.physical_calibration import derive_kv_geometry
from markov_internal.modeling.source_dag.profiles import ProfileRunRef, parse_server_command_flags
from markov_internal.preparation.runtime_dma_calibration import build_operation_plan


@dataclass(frozen=True)
class PhysicalCaptureInputs:
    """Sampling deployment and domain; contains no targets or prediction state."""

    capture: dict[str, Any]
    calibration: dict[str, Any] | None
    resources: dict[str, str] | None
    forced_token_bundle: str | None
    sources: tuple[ProfileRunRef, ...]
    output_dir: Path
    physical_budget: PhysicalBudget | None

    @cached_property
    def physical(self) -> dict[str, Any] | None:
        declared = admitted_physical_calibration(self.calibration)
        return admitted_physical_calibration(captured_physical_declaration(self)) or declared or platform_inputs(self)


def admitted_physical_calibration(declaration: dict[str, Any] | None) -> dict[str, Any] | None:
    """Admit raw measurement sources and geometry; fitting belongs to the cost layer."""

    if declaration is None:
        return None
    if not declaration.get("measurement_sources") or not declaration.get("measurement_description"):
        raise ValueError("physical calibration requires original measurement sources and their scope")
    for source in declaration["measurement_sources"]:
        if not require_repo_path(source).is_file():
            raise ValueError(f"physical calibration source is unavailable: {source}")

    report = load_json(require_repo_path(declaration["report"]))
    for field in ("target_workload_trace_used", "target_e2e_used"):
        if report.get(field) is not False:
            raise ValueError(f"physical calibration must declare {field}=false before any cost projection")

    admitted = {key: report[key] for key in ("kv_geometry", "resource_lanes", "storage_batch_pages")}
    admitted["measurement_scope"] = report.get("measurement_scope") or {}
    admitted["measurement_sources"] = list(declaration["measurement_sources"])
    admitted["measurement_description"] = str(declaration["measurement_description"])
    for field in ("target_workload_trace_used", "target_e2e_used"):
        admitted[field] = report[field]

    return admitted


def physical_attempts(group: PhysicalCaptureInputs) -> list[dict[str, Any]]:
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


def physical_context(group: PhysicalCaptureInputs) -> dict[str, Any]:
    """Admission uses workspace inputs; modeling need not mount model metadata/weights."""
    from markov_internal.modeling.deployment import source_environment

    return {
        "sampling": _io_sampling(group.capture),
        "existing_physical": group.calibration,
        "environment": source_environment(group.sources[0], group.resources),
        "page_token_sizes": _declared_page_sizes(group),
    }


def _io_sampling(spec: dict[str, Any]) -> dict[str, Any]:
    """I/O measurements do not depend on the budget or the separate CPU experiment."""
    return {key: value for key, value in spec.items() if key not in {"budget", "eviction_cpu"}}


def platform_inputs(group: PhysicalCaptureInputs) -> dict[str, Any] | None:
    """Derive deployment metadata when needed; no saved state or timing samples.

    Unsupported or unavailable model metadata returns None. Sampling placement
    and measured service coefficients are deliberately not prerequisites.
    """

    definition = physical_definition(group, service="metadata")
    if definition["limitations"]:
        return None

    return dict(
        kv_geometry=definition["geometry"],
        storage_batch_pages=definition["storage_batch_pages"],
        resource_lanes={"storage_read": "scope", "storage_write": "scope"},
        measurement_sources=[],
        measurement_description="Deployment metadata only; no service cost has been measured",
        measurement_scope=dict(
            storage_scope_devices=definition["devices"],
            storage_scope_numa_nodes=definition["numa_nodes"] or [None] * len(definition["devices"]),
        ),
        target_workload_trace_used=False,
        target_e2e_used=False,
    )


def _declared_page_sizes(group: PhysicalCaptureInputs) -> list[int]:
    """Return the platform sampling domain without consulting prediction targets."""

    value = group.capture.get("page_token_sizes")
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
    group: PhysicalCaptureInputs, *, service: str, directions: tuple[str, ...] = tuple(DMA_SERVICES)
) -> dict[str, Any]:
    """Use deployment geometry, declared page paths and base inputs, never target labels."""
    spec = group.capture
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
    token_plans = [profile_token_plan(source, group.forced_token_bundle) for source in group.sources]
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
            and (stage != "dma" or set(definition["directions"]) <= set(previous["directions"]))
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


def captured_physical_declaration(group: PhysicalCaptureInputs) -> dict[str, Any] | None:
    # Without a sampling declaration, only explicitly supplied measurements
    # apply. Deployment metadata is derived separately and is not evidence.
    if not group.capture.get("page_token_sizes"):
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


def capture_physical(
    group: PhysicalCaptureInputs, *, dry_run: bool, required_components: set[str] | frozenset[str]
) -> dict[str, Any]:
    """Acquire requested services serially, preserving each completed supplement."""
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

    if "page_token_sizes" not in group.capture:
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
            group = replace(group)

    if budget_denied is not None:
        budget_denied["physical_capture_usage"] = result["physical_capture_usage"]
        return budget_denied
    return result


def _capture_physical_service(
    group: PhysicalCaptureInputs, supplement: str, *, directions: tuple[str, ...], dry_run: bool
) -> dict[str, Any]:
    attempts = physical_attempts(group)
    definition = physical_definition(group, service=supplement, directions=directions)
    plan = {"status": "needs_physical_calibration", "bootstrap": definition}
    if definition["limitations"]:
        plan["stop_reason"] = "physical_sampler_limitations"
        return plan
    declaration = captured_physical_declaration(group) or group.calibration
    if declaration is None:
        # The sampler accepts a file input. Write the current metadata here,
        # only for an actual I/O demand; never recover it as reusable state.
        path = group.output_dir / "platform_inputs.json"
        write_json(path, group.physical)
        declaration = {"report": str(repo_relative_path(path))}
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

        # Admission checks measurements, not whether they fit a chosen model.
        # Reports retain earlier sources; this attempt owns only its new file.
        read_physical_measurements(observed, definition["input_context"]["environment"])
        if supplement == "dma":
            return len(load_json(report.parent / "runtime_dma_calibration.json")["samples"])
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


def run_primitive_attempt(
    group: PhysicalCaptureInputs,
    definition: dict[str, Any],
    stage: str,
    command_for_output: Callable[[Path], list[str]],
    report_name: str,
    validate: Callable[[Path], int],
    *,
    environment: dict[str, str] | None = None,
) -> dict[str, Any]:
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
