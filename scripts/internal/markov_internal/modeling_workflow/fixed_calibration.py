"""One target-independent SGLang calibration workload over fixed domain endpoints."""

from __future__ import annotations

import copy
import json
import math
from statistics import median
from typing import Any

from ..common.commands import command_tokens
from ..common.io import load_json, write_json
from ..common.paths import repo_relative_path
from ..contracts.forced_token.plan import load_forced_token_plan
from ..profiling.suite import matrix_entries
from .capture import calibration_inputs, capture_usage, completed_profile
from .group import GroupRequest


CALIBRATION_NAME = "fixed_calibration"


def _server_command(command: list[str], changes: dict[str, Any]) -> list[str]:
    """Replace declared calibration flags while retaining the base environment."""

    changes = dict(changes)
    result: list[str] = []
    index = 0
    while index < len(command):
        option = command[index].split("=", 1)[0]
        end = index + 1
        if option.startswith("--"):
            while end < len(command) and not command[end].startswith("--"):
                end += 1
        if option in changes:
            result.extend((option, str(changes.pop(option))))
        else:
            result.extend(command[index:end])
        index = end
    for option, value in changes.items():
        result.extend((option, str(value)))
    return result


def _base_tokens(group: GroupRequest) -> tuple[list[int], list[int], list[str]]:
    prompts: list[list[int]] = []
    sources: list[str] = []
    for workload in group.workload_ids:
        path = group.token_plan(workload)
        if path is None:
            raise ValueError(f"fixed calibration requires admitted base tokens for {workload}")
        plan = load_forced_token_plan(path)
        prompts.extend(row["origin_input_ids"] for row in plan["requests"])
        sources.append(str(repo_relative_path(path)))
    if not prompts:
        raise ValueError("fixed calibration requires non-empty base requests")
    pool = max(prompts, key=lambda values: (len(values), values))
    if len(set(pool)) < 2:
        raise ValueError("fixed calibration needs two distinct admitted token IDs")
    return pool, [len(prompt) for prompt in prompts], sources


def calibration_page_sizes(group: GroupRequest) -> tuple[list[int], list[int]]:
    """Return the fixed endpoint set and its platform-declared page domain."""

    geometry = int(group.physical["kv_geometry"]["kv_bytes_per_token_per_rank"])
    points = group.physical["service_models"]["load"]["page_bandwidth_points"]
    page_sizes = sorted({int(point["page_bytes"]) // geometry for point in points})
    if not page_sizes or any(size <= 0 for size in page_sizes):
        raise ValueError("physical calibration has no usable page-size domain")
    endpoints = page_sizes if len(page_sizes) == 1 else [page_sizes[0], page_sizes[-1]]
    return endpoints, page_sizes


def _workload(group: GroupRequest) -> tuple[dict[str, Any], dict[str, Any]]:
    """Create one request sequence shared unchanged by both page endpoints."""

    if group.physical is None:
        raise ValueError("fixed calibration requires platform physical calibration")
    endpoints, calibrated_page_sizes = calibration_page_sizes(group)
    pool, prompt_lengths, token_sources = _base_tokens(group)
    storage_batch_pages = int(group.physical["storage_batch_pages"])
    alignment = math.lcm(*endpoints)
    payload_tokens = math.ceil(max(prompt_lengths) / alignment) * alignment
    payload_tokens = max(2 * alignment, min(payload_tokens, storage_batch_pages * min(endpoints)))
    short_tokens = 2 * alignment
    point_fields = []
    for page in endpoints:
        payload_pages = payload_tokens // page
        device_pages = payload_pages + 2
        host_pages = 2 * device_pages
        point_fields.append({
            "id": f"{CALIBRATION_NAME}_page_{page}",
            "page_size": page,
            "device_tokens": device_pages * page,
            "host_tokens": host_pages * page,
            "prefetch_threshold": page,
            "prefetch_capacity_limit_tokens": int(0.8 * (host_pages - device_pages) * page),
            "payload_pages": payload_pages,
            "device_pages": device_pages,
            "host_pages": host_pages,
        })
    pressure_count = max(
        math.ceil((point["device_pages"] + point["host_pages"]) / point["payload_pages"]) + 1
        for point in point_fields
    )

    symbols = sorted(set(pool))
    definitions: dict[str, Any] = {}
    steps: list[dict[str, Any]] = []
    branch = 0

    def tokens(length: int) -> list[int]:
        nonlocal branch
        if branch >= len(symbols) ** 2:
            raise ValueError("fixed calibration exhausted independent token prefixes")
        values = (pool * math.ceil(length / len(pool)))[:length]
        values[:2] = [symbols[branch // len(symbols)], symbols[branch % len(symbols)]]
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

    define("saved_short", short_tokens)
    define("saved_long", payload_tokens)
    request("saved_short", "seed")
    request("saved_long", "seed")
    for index in range(pressure_count):
        name = f"pressure_{index}"
        define(name, payload_tokens)
        request(name, "spill")
    steps.append(
        {
            "id": "storage_ready",
            "kind": "checkpoint",
            "phase": "boundary",
            "measure": False,
            "assertions": [
                {
                    "id": "short_on_storage",
                    "request": "saved_short",
                    "range": "anchor",
                    "expect": {"device": "none", "host": "none", "storage": "all"},
                },
                {
                    "id": "long_on_storage",
                    "request": "saved_long",
                    "range": "anchor",
                    "expect": {"device": "none", "host": "none", "storage": "all"},
                },
            ],
        }
    )
    formal_start = f"request_{len([step for step in steps if step['kind'] == 'request'])}"
    request("saved_short", "recovery", measure=True)
    request("saved_long", "recovery", measure=True)
    for index in range(pressure_count):
        name = f"rewrite_pressure_{index}"
        define(name, payload_tokens)
        request(name, "rewrite", measure=True)
    formal_end = next(step["id"] for step in reversed(steps) if step["kind"] == "request")

    template = {
        "id": CALIBRATION_NAME,
        "description": "One spill, recovery and rewrite workload shared by every target and page endpoint.",
        "defaults": {
            "sampling": {
                "max_new_tokens": 1,
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
    requests = [step for step in steps if step["kind"] == "request"]
    derivation = {
        "physical_page_size_domain": calibrated_page_sizes,
        "calibration_page_size_endpoints": endpoints,
        "base_prompt_tokens": {
            "minimum": min(prompt_lengths),
            "median": median(prompt_lengths),
            "maximum": max(prompt_lengths),
        },
        "storage_batch_pages": storage_batch_pages,
        "alignment_tokens": alignment,
        "short_tokens": short_tokens,
        "payload_tokens": payload_tokens,
        "endpoint_capacities": [dict(point) for point in point_fields],
        "pressure_requests_per_stage": pressure_count,
        "token_input_sources": token_sources,
        "target_inputs": [],
        "rule": (
            "run the same token workload at both endpoints declared by the platform page domain; "
            "payload is aligned to both endpoints and bounded by the storage batch limit; pressure exceeds "
            "device+host capacity; prefetch limit follows SGLang's 80% host-minus-device headroom"
        ),
    }
    return template, {
        "derivation": derivation,
        "points": point_fields,
        "requests_per_run": len(requests),
        "tokens_per_run": sum(len(definitions[step["request"]]["prompt_token_ids"]) + 1 for step in requests),
    }


def _config_spec(fields: dict[str, Any]) -> dict[str, Any]:
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
                    "write_policy": "write_back",
                    "prefetch_threshold": point["prefetch_threshold"],
                    "prefetch_stop_policy": "wait_complete",
                    "prefetch_capacity_limit_tokens": point["prefetch_capacity_limit_tokens"],
                },
            }
            for point in fields["points"]
        ]
    }


def materialize_fixed_calibration(group: GroupRequest) -> dict[str, Any]:
    """Write deterministic inputs; target declarations are deliberately absent."""

    template, fields = _workload(group)
    output = group.output_dir / "generated" / CALIBRATION_NAME
    template_path = output / "workload.json"
    specs_path = output / "config_specs.json"
    write_json(template_path, template)
    write_json(specs_path, _config_spec(fields))

    suite = load_json(group.profile_suite)
    base_server = copy.deepcopy(matrix_entries(suite["matrix"], "servers")[group.base_config]["server"])
    base_command = command_tokens(base_server["command"])

    def relative(path: Any) -> str:
        return str(repo_relative_path(path))
    common = {key: copy.deepcopy(value) for key, value in suite.items() if key not in {"matrix", "experiments", "metadata", "name"}}
    files = {"template": relative(template_path), "config_specs": relative(specs_path)}
    for point in fields["points"]:
        server = copy.deepcopy(base_server)
        extra = (
            json.loads(base_command[base_command.index("--hicache-storage-backend-extra-config") + 1])
            if "--hicache-storage-backend-extra-config" in base_command
            else {}
        )
        extra.update(prefetch_threshold=point["prefetch_threshold"], prefetch_timeout_base=2.0,
                     prefetch_timeout_per_ki_token=0.0, prefetch_timeout_max=2.0)
        changes = {
            "--page-size": point["page_size"],
            "--max-total-tokens": point["device_tokens"],
            "--max-prefill-tokens": point["device_tokens"],
            "--hicache-ratio": (point["host_tokens"] - point["page_size"] / 2) / point["device_tokens"],
            "--hicache-write-policy": "write_back",
            "--hicache-storage-prefetch-policy": "wait_complete",
            "--hicache-storage-backend-extra-config": json.dumps(extra, separators=(",", ":")),
        }
        server.update(command=_server_command(base_command, changes), startup_max_attempts=1)
        bench = ["python3", "scripts/bench/hicache_template_workload.py", "--template", relative(template_path),
                 "--config-specs", relative(specs_path), "--config-id", point["id"], "--output-dir",
                 "{bench_dir}/calibration", "--base-url", server["ready_url"].rsplit("/", 1)[0]]
        point_files = {}
        for mode in ("capture", "replay"):
            config = copy.deepcopy(common)
            argv = [*bench, "--forced-token-mode", mode]
            argv.extend(["--require-hicache-state"] if mode == "capture"
                        else ["--forced-token-plan", "{forced_token_plan}"])
            config.update(
                name=f"{group.base_config}_{point['id']}_{mode}", continue_on_error=False,
                metadata={"purpose": "Fixed endpoint calibration; targets and scores are not inputs.",
                          "profile_mode": "forced_token_capture" if mode == "capture" else "fixed_calibration_replay",
                          "calibration_base": group.base_config, "calibration_point": point["id"]},
                matrix={"servers": [{"id": point["id"], "server": server}],
                        "inputs": [{"id": CALIBRATION_NAME, "bench": {"command": argv}}]},
            )
            path = output / f"{point['id']}_{mode}.json"
            write_json(path, config)
            point_files["capture_suite" if mode == "capture" else "profile_suite"] = relative(path)
        point["files"] = point_files
    return {"name": CALIBRATION_NAME, "files": files, **fields}


def plan_fixed_calibration(group: GroupRequest) -> dict[str, Any]:
    from .planning.profile_runs import ProfileRunDiscovery

    definition = materialize_fixed_calibration(group)
    ledger_path = group.output_dir / "capture_ledger.json"
    attempts = load_json(ledger_path)["attempts"] if ledger_path.exists() else []
    expected = calibration_inputs(definition["files"])
    different = [row for row in attempts if "calibration_inputs" in row and calibration_inputs(row["calibration_inputs"]) != expected]
    successful = [row for row in attempts if completed_profile(row) and calibration_inputs(row["calibration_inputs"]) == expected]
    pages = {point["id"]: point["page_size"] for point in definition["points"]}
    counts = {point: 0 for point in pages}
    for source in ProfileRunDiscovery((), tuple(group.fixed_calibration_manifests)).discover():
        matches = [point for point, page in pages.items() if int(source.hicache_config["page_size"]) == page]
        if len(matches) != 1:
            raise ValueError("fixed calibration profile is outside the platform endpoint domain")
        counts[matches[0]] += 1
    for row in successful:
        point = row.get("calibration_point")
        if point not in counts:
            raise ValueError("fixed calibration ledger does not identify an endpoint")
        counts[point] += 1
    usage = capture_usage(attempts)
    remaining = {point: max(0, group.budget.repeats - count) for point, count in counts.items()}
    status = "input_conflict" if different else (
        "ready" if not any(remaining.values()) else "needs_calibration"
    )
    return {
        "status": status,
        "definition": definition,
        "requested_repeats_per_endpoint": group.budget.repeats,
        "completed_profiles_by_endpoint": counts,
        "remaining_profiles_by_endpoint": remaining,
        "completed_profile_count": sum(counts.values()),
        "remaining_profile_count": sum(remaining.values()),
        "next_point": next((point for point, count in remaining.items() if count), None),
        "usage": usage,
        "conflicting_attempt_count": len(different),
        "target_inputs": [],
    }
