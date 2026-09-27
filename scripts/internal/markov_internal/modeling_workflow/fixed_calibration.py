"""Shared SGLang calibration experiments selected from unresolved cost evidence."""

from __future__ import annotations

import json
import math
from statistics import median
from typing import Any

from ..common.commands import command_tokens, replace_command_option
from ..common.io import load_json, write_json
from ..common.paths import repo_relative_path, require_repo_path
from ..contracts.forced_token.plan import int_list, load_forced_token_plan
from ..workload_template.schema import load_template
from ..workload_template.expand import expand_template, request_token_budget
from ..workload_template.cli import parse_workload_command
from .capture import calibration_inputs, capture_usage, matching_calibration_profiles
from .group import GroupRequest
from .planning.profile_runs import parse_server_command_tokens


CALIBRATION_NAME = "fixed_calibration"


def _base_tokens(group: GroupRequest) -> tuple[list[int], list[int], list[str]]:
    prompts: list[list[int]] = []
    sources: list[str] = []
    for workload in group.workload_ids:
        path = group.token_plan(workload)
        if path is not None:
            values = [row["origin_input_ids"] for row in load_forced_token_plan(path)["requests"]]
        else:
            source = next(item for item in group.sources if item.input_id == workload)
            window = source.workload_window
            report = load_json(window.report_path) if window else {}
            if "input_tokens" in report:
                if report.get("workload_id") != workload or report.get("status") != "completed":
                    raise ValueError("base input tokens require its completed workload report")
                values = [
                    report["input_tokens"][row["request_name"]]
                    for row in report["requests"]
                    if row["kind"] == "request"
                ]
                path = window.report_path
            else:
                args = parse_workload_command(load_json(source.config_path)["bench"]["command"])
                if args is None:
                    raise ValueError(f"base {workload} has no recorded token inputs; provide its token plan")
                path = require_repo_path(args.template)
                values = [list(row.prompt_token_ids) for row in expand_template(load_template(path), None).requests]
        if not values or any(not value or int_list(value) is None or min(value) < 0 for value in values):
            raise ValueError(f"base {workload} needs explicit non-negative input token IDs for calibration")
        prompts.extend(values)
        sources.append(str(repo_relative_path(path)))
    if not prompts:
        raise ValueError("fixed calibration requires non-empty base requests")
    pool = max(prompts, key=lambda values: (len(values), values))
    if len(set(pool)) < 2:
        raise ValueError("fixed calibration needs two distinct admitted token IDs")
    return pool, [len(prompt) for prompt in prompts], sources


def _workload(group: GroupRequest, *, release_only: bool = False) -> tuple[dict[str, Any], dict[str, Any]]:
    """Exercise cache transitions at the base page size, shared by all targets."""

    if group.physical is None:
        raise ValueError("fixed calibration requires platform physical calibration")
    page = int(group.sources[0].hicache_config["page_size"])
    pool, prompt_lengths, token_sources = _base_tokens(group)
    storage_batch_pages = int(group.physical["storage_batch_pages"])
    alignment = page
    payload_tokens = math.ceil(max(prompt_lengths) / alignment) * alignment
    payload_tokens = max(4 * page, min(payload_tokens, storage_batch_pages * page))
    short_tokens = 2 * alignment
    payload_pages = payload_tokens // page
    device_pages = payload_pages + 2
    host_pages = 2 * device_pages
    point = {
        "id": f"{CALIBRATION_NAME}_page_{page}",
        "page_size": page,
        "device_tokens": device_pages * page,
        "host_tokens": host_pages * page,
        "prefetch_threshold": page,
        "prefetch_capacity_limit_tokens": int(0.8 * (host_pages - device_pages) * page),
        "payload_pages": payload_pages,
        "device_pages": device_pages,
        "host_pages": host_pages,
    }
    pressure_count = math.ceil((device_pages + host_pages) / payload_pages) + 1
    if release_only:
        # Keep the common prompt encoding, but ordinary eviction need only
        # exceed device capacity, not also fill the host cache.
        pressure_count = math.ceil(device_pages / payload_pages) + 1
        point.update(id=f"release_calibration_page_{page}", write_policy="write_through_selective")

    symbols = sorted(set(pool))
    prefix_width = min(2, page)
    while len(symbols) ** prefix_width < 2 + pressure_count:
        prefix_width += 1
    if prefix_width > page:
        raise ValueError("base tokens cannot form enough distinct first pages for the shared pressure workload")
    definitions: dict[str, Any] = {}
    steps: list[dict[str, Any]] = []
    branch = 0

    def tokens(length: int) -> list[int]:
        nonlocal branch
        values = (pool * math.ceil(length / len(pool)))[:length]
        number = branch
        for index in reversed(range(prefix_width)):
            number, digit = divmod(number, len(symbols))
            values[index] = symbols[digit]
        branch += 1
        return values

    def define(name: str, length: int) -> None:
        values = tokens(length)
        definitions[name] = {
            "prompt_token_ids": values,
            "token_contract": {"anchor_tokens": alignment, "tail_tokens": len(values) - alignment},
        }

    def request(name: str, phase: str, *, measure: bool = False) -> None:
        step_id = f"request_{len([step for step in steps if step['kind'] == 'request'])}"
        steps.append({"id": step_id, "kind": "request", "request": name, "phase": phase, "measure": measure})
        if not measure:
            steps.append(
                {
                    "id": f"idle_after_{step_id}",
                    "kind": "barrier",
                    "scope": "hicache_idle",
                    "timeout_sec": 180,
                    "phase": "calibration_settle",
                    "measure": False,
                }
            )

    def checkpoint(name: str, requests: dict[str, str], expect: dict[str, str]) -> None:
        steps.append(
            dict(
                id=name,
                kind="checkpoint",
                phase="boundary",
                measure=False,
                assertions=[
                    dict(id=label, request=request, range="anchor", expect=expect)
                    for label, request in requests.items()
                ],
            )
        )

    define("saved_short", short_tokens)
    define("saved_long", payload_tokens)
    request("saved_short", "seed")
    if release_only:
        checkpoint("device_ready", {"seed_on_device": "saved_short"}, dict(device="all", host="none", storage="ignore"))
        formal_start = "request_1"
    request("saved_long", "seed", measure=release_only)
    for index in range(pressure_count):
        name = f"pressure_{index}"
        define(name, payload_tokens)
        request(name, "spill", measure=release_only)
    if not release_only:
        checkpoint(
            "storage_ready",
            {"short_on_storage": "saved_short", "long_on_storage": "saved_long"},
            dict(device="none", host="none", storage="all"),
        )
        formal_start = f"request_{len([step for step in steps if step['kind'] == 'request'])}"
        request("saved_short", "recovery", measure=True)
        request("saved_long", "recovery", measure=True)
    formal_end = next(step["id"] for step in reversed(steps) if step["kind"] == "request")
    # One token finishes in prefill, inserting each leaf only once. With decode,
    # unfinished + finished insertion reaches the selective backup threshold,
    # so eviction measures backup release instead of ordinary release.
    # Other calibration workloads retain a decode step for phase observations.
    output_tokens = 1 if release_only else 2

    template = {
        "id": CALIBRATION_NAME,
        "description": "Spill two payloads and recover them to measure prefetch control, shared by every target.",
        "defaults": {
            "sampling": {
                "max_new_tokens": output_tokens,
                "temperature": 0,
                "top_p": 1.0,
                "top_k": 1,
                "ignore_eos": True,
            },
            "request_timeout_sec": 600,
        },
        "fragments": {},
        "request_defs": definitions,
        "steps": steps,
        "formal_window": {"start_step": formal_start, "end_step": formal_end},
    }
    derivation = {
        "calibration_page_size": page,
        "base_prompt_tokens": {
            "minimum": min(prompt_lengths),
            "median": median(prompt_lengths),
            "maximum": max(prompt_lengths),
        },
        "storage_batch_pages": storage_batch_pages,
        "alignment_tokens": alignment,
        "short_tokens": short_tokens,
        "payload_tokens": payload_tokens,
        "capacities": [point],
        "pressure_requests_per_stage": pressure_count,
        "output_tokens_per_request": output_tokens,
        "token_input_sources": token_sources,
        "rule": (
            "use the base page size and distinct short/long payloads; pressure exceeds device+host capacity; "
            "other page sizes use the cost model's declared extrapolation, not additional endpoint captures"
        ),
    }
    if release_only:
        template.update(
            id="ordinary_release_calibration",
            description="Distinct first-use requests exceed device capacity and exercise ordinary eviction.",
        )
        derivation["rule"] = (
            "Unique first-use leaves exceed base-derived device capacity; no target inputs or storage recovery."
        )
    return template, {"derivation": derivation, "point": point}


def _config_spec(point: dict[str, Any], prefetch_policy: str = "wait_complete") -> dict[str, Any]:
    return {
        "configs": [
            {
                "id": point["id"],
                "resolved": {
                    "page_size": point["page_size"],
                    "device_pages": point["device_pages"],
                    "host_pages": point["host_pages"],
                },
                "policy": {
                    "write_policy": point.get("write_policy", "write_back"),
                    "prefetch_threshold": point["prefetch_threshold"],
                    "prefetch_stop_policy": prefetch_policy,
                    "prefetch_capacity_limit_tokens": point["prefetch_capacity_limit_tokens"],
                },
            }
        ]
    }


def _phase_workload(group: GroupRequest) -> tuple[dict[str, Any], dict[str, Any]]:
    """Vary new work and context without forcing any cache eviction or recovery."""
    pool, _, sources = _base_tokens(group)
    page = int(group.sources[0].hicache_config["page_size"])
    prefix = (pool * math.ceil(2 * page / len(pool)))[: 2 * page]
    symbols = sorted(set(pool))
    definitions = {"seed": {"prompt_token_ids": prefix, "token_contract": {"anchor_tokens": page, "tail_tokens": page}}}
    for index, length in enumerate((page + 1, 2 * page + 2, 4 * page + 3)):
        tail = (pool * math.ceil(length / len(pool)))[:length]
        # Two token positions distinguish three continuations even with two symbols.
        tail[:2] = [symbols[index // len(symbols)], symbols[index % len(symbols)]]
        values = prefix + tail
        definitions[f"extend_{index}"] = {
            "prompt_token_ids": values,
            "token_contract": {"anchor_tokens": page, "tail_tokens": len(values) - page},
        }
    steps = [
        {"id": "seed", "kind": "request", "request": "seed", "phase": "compute", "measure": False},
        {
            "id": "settle",
            "kind": "barrier",
            "scope": "hicache_idle",
            "timeout_sec": 180,
            "phase": "compute",
            "measure": False,
        },
        {
            "id": "prefix_ready",
            "kind": "checkpoint",
            "phase": "compute",
            "measure": False,
            "assertions": [
                {
                    "id": "prefix_on_device",
                    "request": "seed",
                    "range": "anchor",
                    "expect": {"device": "all", "host": "ignore", "storage": "ignore"},
                }
            ],
        },
    ]
    steps.extend(
        {"id": name, "kind": "request", "request": name, "phase": "compute", "measure": True}
        for name in definitions
        if name != "seed"
    )
    token_count = sum(len(value["prompt_token_ids"]) + 2 for value in definitions.values())
    device_pages = math.ceil(token_count / page) + 2
    point = dict(
        id=f"phase_calibration_page_{page}",
        page_size=page,
        device_pages=device_pages,
        host_pages=2 * device_pages,
        device_tokens=device_pages * page,
        host_tokens=2 * device_pages * page,
        prefetch_threshold=page,
        prefetch_capacity_limit_tokens=int(0.8 * device_pages * page),
    )
    template = dict(
        id="phase_calibration",
        description="One resident prefix and three continuations; no forced spill.",
        defaults={
            "sampling": {"max_new_tokens": 2, "temperature": 0, "top_p": 1.0, "top_k": 1, "ignore_eos": True},
            "request_timeout_sec": 600,
        },
        fragments={},
        request_defs=definitions,
        steps=steps,
        formal_window={"start_step": "extend_0", "end_step": "extend_2"},
    )
    return template, dict(
        point=point,
        derivation={
            "token_input_sources": sources,
            "rule": "Use the base page size; vary new tokens and context; reserve capacity for all requests.",
            "limitation": "Planned work does not guarantee observed kernel coverage or parameter identifiability.",
        },
    )


def materialize_fixed_calibration(
    group: GroupRequest, *, phase_only: bool = False, prefetch_policy: str = "wait_complete", release_only: bool = False
) -> dict[str, Any]:
    """Generate shared work from base inputs and the required execution program."""

    template, fields = _phase_workload(group) if phase_only else _workload(group, release_only=release_only)
    point = fields["point"]
    if not release_only and prefetch_policy == "best_effort":
        template["id"] = "hicache_stop_calibration"
        point["id"] = f"stop_calibration_page_{point['page_size']}"
    output = group.output_dir / "generated" / template["id"]
    template_path = output / "workload.json"
    specs_path = output / "config_specs.json"
    write_json(template_path, template)
    write_json(specs_path, _config_spec(point, prefetch_policy))
    counts = request_token_budget(load_template(template_path))

    # The admitted capture, not a later-edited experiment suite, owns the base environment.
    # This freshly loaded object is owned by this builder; no caller data is mutated.
    config = load_json(group.sources[0].config_path)
    server = config.pop("server")
    base_command = command_tokens(server["command"])
    base_flags = parse_server_command_tokens(base_command)

    def relative(path: Any) -> str:
        return str(repo_relative_path(path))

    for key in ("matrix", "experiments", "metadata", "name", "bench", "id"):
        config.pop(key, None)
    config["run_root"] = relative(group.output_dir / "calibration_profiles")
    if config.get("env", {}).get("SGLANG_STEP_TIMING_DIR"):
        config["env"]["SGLANG_STEP_TIMING_DIR"] = "{run_dir}/timing"
    files = {"template": relative(template_path), "config_specs": relative(specs_path)}
    extra = json.loads(base_flags.get("hicache_storage_backend_extra_config", "{}"))
    extra.update(
        prefetch_threshold=point["prefetch_threshold"],
        prefetch_timeout_base=2.0,
        prefetch_timeout_per_ki_token=0.0,
        prefetch_timeout_max=2.0,
    )
    changes = {
        "--page-size": point["page_size"],
        "--max-total-tokens": point["device_tokens"],
        "--max-prefill-tokens": point["device_tokens"],
        "--hicache-ratio": (point["host_tokens"] - point["page_size"] / 2) / point["device_tokens"],
        "--hicache-write-policy": point.get("write_policy", "write_back"),
        "--hicache-storage-prefetch-policy": prefetch_policy,
        "--hicache-storage-backend-extra-config": json.dumps(extra, separators=(",", ":")),
    }
    command = list(base_command)
    for option, value in changes.items():
        replace_command_option(command, option, str(value))
    server.update(command=command, startup_max_attempts=1)
    bench = [
        "python3",
        "scripts/bench/hicache_template_workload.py",
        "--template",
        relative(template_path),
        "--config-specs",
        relative(specs_path),
        "--config-id",
        point["id"],
        "--output-dir",
        "{bench_dir}/calibration",
        "--base-url",
        server["ready_url"].rsplit("/", 1)[0],
    ]
    config.update(
        name=f"{group.base_config}_{point['id']}_capture",
        continue_on_error=False,
        metadata={
            "purpose": "Shared calibration; target observations and scores are not inputs.",
            "profile_mode": "forced_token_capture",
            "calibration_base": group.base_config,
            "calibration_point": point["id"],
            "config_id": point["id"],
            "workload_id": template["id"],
        },
        server=server,
        bench={"command": [*bench, "--forced-token-mode", "capture", "--require-hicache-state"]},
        experiments=[{"id": f"{point['id']}_{template['id']}"}],
    )
    path = output / f"{point['id']}_capture.json"
    write_json(path, config)
    point["files"] = {"capture_suite": relative(path)}
    return {"name": template["id"], "files": files, "counts": counts, **fields}


def plan_fixed_calibration(group: GroupRequest, model_inputs: dict[str, Any]) -> dict[str, Any]:
    if model_inputs.get("status") == "ready":
        return {"status": "not_required", "reason": "ready_for_execution_checks"}
    missing = model_inputs.get("missing", [])
    components = {item["component"] for item in missing}
    if components != {"phase"}:
        return dict(
            status="no_suitable_experiment",
            missing=missing,
            remaining_profile_count=0,
            reason="No workload experiment is justified for these input gaps.",
        )
    definition = materialize_fixed_calibration(group, phase_only=True)
    ledger_path = group.output_dir / "capture_ledger.json"
    attempts = load_json(ledger_path)["attempts"] if ledger_path.exists() else []
    expected = calibration_inputs(definition["files"])
    successful = matching_calibration_profiles(attempts, expected)
    point = definition["point"]["id"]
    if any(row.get("calibration_point") != point for row in successful):
        raise ValueError("calibration ledger does not identify the selected experiment")
    # More repeats cannot supply an absent branch or another work coordinate.
    return {
        "status": "experiment_exhausted" if successful else "needs_calibration",
        "missing": missing,
        "experiment_scope": "phase",
        "definition": definition,
        "completed_profile_count": len(successful),
        "remaining_profile_count": int(not successful),
        "usage": capture_usage(attempts),
    }
