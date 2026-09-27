"""Prepare same-base forward CPU measurements from explicit capture assets."""

from __future__ import annotations

import json
from pathlib import Path

from ...common.io import load_json
from ...common.manifest import workload_report_path
from ...common.paths import require_repo_path, repo_relative_path
from ...common.trace import load_chrome_trace_events
from ..planning.profile_runs import parse_server_command_flags
from .base_pair import validate_base_pair, validate_forward_work
from .step_timing import bind_forward_steps, compare_forward_cpu, observe_forward_cpu
from .host_timing import observe_host_cpu, pair_host_cpu
from .input_timing import bind_input_calls, observe_input_cpu, pair_input_cpu
from .scheduler_timing import observe_scheduler_cpu, pair_scheduler_cpu
from .cpu_scopes import synchronization_scopes, validate_host_ranges
from .layer_timing import observe_layer_cpu, pair_layer_cpu


def _capture(manifest_path: Path, tp_size: int) -> dict:
    manifest = load_json(manifest_path)
    if manifest["status"] != "completed" or manifest.get("dry_run") or manifest.get("collection_errors"):
        raise ValueError("CPU pair requires completed successful captures")
    root = manifest_path.parent
    flags = parse_server_command_flags(root / "server_cmd.txt")
    if int(flags.get("tp_size", "1")) != tp_size:
        raise ValueError("CPU pair rank coverage differs from the captured server TP declaration")
    config = load_json(require_repo_path(manifest["config_path"]))
    report = load_json(workload_report_path(manifest))
    plan = load_json(require_repo_path(report["forced_token"]["plan_path"]))
    timing_dir = require_repo_path(config["env"]["SGLANG_STEP_TIMING_DIR"].replace("{run_dir}", str(root)))
    return dict(manifest=manifest, config=config, report=report, plan=plan, timing_dir=timing_dir)


def _timer_events(directory: Path):
    for path in sorted(directory.glob("*.jsonl")):
        with path.open(encoding="utf-8") as stream:
            for line in stream:
                yield json.loads(line)


def _trace_events(manifest: dict, channel: str = "ld_preload"):
    entries = (
        manifest["sidecar"]["python_probe_files"]
        if channel == "python_probe"
        else manifest["trace"]["ld_preload_trace_files"]
    )
    for entry in entries:
        path = require_repo_path(entry["path"])
        yield from load_chrome_trace_events(path)


def prepare_forward_cpu_pair(
    light_manifest: Path,
    profiled_manifest: Path,
    tp_size: int,
    *,
    correct_recorder: bool = False,
) -> dict:
    """Read only the two base captures; do not select targets or fit corrections."""
    manifests = tuple(require_repo_path(path).resolve() for path in (light_manifest, profiled_manifest))
    if manifests[0] == manifests[1]:
        raise ValueError("CPU pair requires distinct light and profiled captures")
    captures = tuple(_capture(path, tp_size) for path in manifests)
    validate_base_pair(*(tuple(capture[key] for capture in captures) for key in ("config", "report", "plan")))
    steps, timers = [], []
    for manifest, capture in zip(manifests, captures):
        records = list(_timer_events(capture["timing_dir"]))
        if correct_recorder and not timers:
            from .recorder_correction import clean_reference

            records = clean_reference(records)
        timers.append(records)
        rows = bind_forward_steps(capture["report"], records, range(tp_size))
        steps.append(dict(source_manifest=str(repo_relative_path(manifest)), rows=rows))
    validate_forward_work(steps[0]["rows"], steps[1]["rows"])
    observed, hosts, inputs, schedulers, layers = [], [], [], [], []
    layer_enabled = [c["config"].get("env", {}).get("SGLANG_LAYER_WAIT_TIMING") == "1" for c in captures]
    if layer_enabled[0] != layer_enabled[1]:
        raise ValueError("Layer timing must be enabled in both base captures")
    for capture_index, (step, capture, records) in enumerate(zip(steps, captures, timers)):
        sync = synchronization_scopes(_trace_events(capture["manifest"]))
        observed.append(observe_forward_cpu(step, sync))
        if layer_enabled[0]:
            layers.append(observe_layer_cpu(step["rows"], records, sync))
        hosts.append(observe_host_cpu(step, capture["report"], records, sync))
        schedulers.append(
            observe_scheduler_cpu(
                step, hosts[-1], capture["report"], records, sync, measure_boundary_results=capture_index == 0
            )
        )
        input_calls = bind_input_calls(capture["report"], records, range(tp_size))
        inputs.append(observe_input_cpu(input_calls, records, sync))
    host_pair = pair_host_cpu(*hosts, _trace_events(captures[1]["manifest"], "python_probe"))
    scheduler_pair = pair_scheduler_cpu(*schedulers)
    comparison = compare_forward_cpu(*observed)
    layer_pair = pair_layer_cpu(*layers, comparison, steps[1]["source_manifest"]) if layers else None
    host_rows = host_pair["rows"] + (layer_pair["rows"] if layer_pair else [])
    host_rows.extend(scheduler_pair["rows"])
    host_rows.extend(pair_input_cpu(*inputs))
    validate_host_ranges(host_rows)

    measurements = {
        "light_manifest": str(repo_relative_path(manifests[0])),
        "source_manifest": str(repo_relative_path(manifests[1])),
        "profiled_steps": steps[1],
        "comparison": comparison,
        "pairing": {
            "host": {key: value for key, value in host_pair.items() if key != "rows"},
            "scheduler": {key: value for key, value in scheduler_pair.items() if key != "rows"},
        },
        "host_measurements": {
            "scope": "Same-source host, scheduler and input budgets, nonoverlapping ranges",
            "source_manifest": steps[1]["source_manifest"],
            "rows": host_rows,
        },
    }
    if correct_recorder:
        from .recorder_correction import source_writes

        formal = captures[1]["report"]["formal_window"]
        begin, end = [round(formal[key] * 1_000_000) for key in ("formal_begin_ms", "formal_end_ms")]
        measurements["recorder_writes"] = source_writes(timers[1], begin, end)
        measurements["observer_correction"] = dict(
            scope="Measured light recorder CPU inside paired envelopes only", unmeasured_overhead_removed=False
        )

    return measurements
