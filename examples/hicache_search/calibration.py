"""Collect one declared shared cost extension before searching any target."""

from __future__ import annotations

import argparse
from dataclasses import asdict
import json
import math
from pathlib import Path
import sqlite3
import statistics
import time

from markov.adapters.sglang.capture import capture_environment
from markov.adapters.sglang.sampling import capture_command, failed_load_capture, region_workload
from markov.artifacts.capture import CaptureSettings, DeviceCommand
from markov.artifacts.performance import Measurement, PerformanceData
from markov.artifacts.request import Deployment, positive_integer, read_json
from markov.artifacts.writing import write_json
from markov.workflow.device_task import DeviceTask
from markov.workflow.performance import maintain_performance


def commands(settings: dict) -> list[DeviceCommand]:
    deployment = Deployment.parse(settings["deployment"])
    if (deployment.framework, deployment.runtime) != ("sglang", "ascend"):
        raise ValueError("This shared calibration uses the SGLang/Ascend native lifecycle")

    recipe = settings["calibration"]
    maximum = positive_integer(recipe["max_context_tokens"], "max_context_tokens")
    pages = recipe["page_tokens"]
    if not isinstance(pages, list) or not pages or len(set(pages)) != len(pages):
        raise ValueError("page_tokens must be a nonempty list of distinct positive integers")
    for page in pages:
        positive_integer(page, "page_tokens")
    for key in ("layers", "kv_heads", "head_dim"):
        positive_integer(recipe["geometry"][key], key)

    result, io_cases = [], []
    for page in recipe["page_tokens"]:
        workload = region_workload(maximum, page, ["prefill", "decode"])
        capacity = sum(
            math.ceil((len(r["input_ids"]) + r["max_new_tokens"]) / page) * page for r in workload["requests"]
        )
        configuration = {
            "page_size": page,
            "max_total_tokens": capacity,
            "max_prefill_tokens": math.ceil(maximum / page) * page,
            "enable_hierarchical_cache": True,
            "hicache_ratio": 1.0,
            "hicache_write_policy": "write_back",
            "hicache_io_backend": "kernel_ascend",
            "hicache_mem_layout": "page_first_direct",
        }
        result.append(capture_command(deployment, configuration, workload, "sglang_regions"))
        maximum_pages = math.ceil(maximum / page)
        io_cases.extend(
            {"page_tokens": page, "pages": pages, "segments": segments}
            for pages, segments in (
                (1, 1),
                (math.ceil(maximum_pages / 2), 1),
                (maximum_pages, 1),
                (maximum_pages, maximum_pages),
            )
        )

    result.append(
        DeviceCommand(
            "markov.adapters.sglang.cache_io",
            ("--recipe", "/capture/capture_input.json", "--output", "/capture/data"),
            {
                "device": 0,
                "geometry": recipe["geometry"],
                "warmup": 1,
                "repeats": 3,
                "operations": ["backup", "load"],
                "cases": io_cases,
            },
            "data/capture.json",
            "cache_io",
            environment=capture_environment(False),
            measurement_adapter="sglang_cache_io",
            required_measurement_ports=("load_consumer_increment_ns",),
        )
    )
    result.extend(failed_load_capture(deployment, page, math.ceil(maximum / page)) for page in recipe["page_tokens"])
    return result


def run(settings_path: Path, output: Path, models_path: Path, database: Path, plan_only: bool) -> dict:
    settings = read_json(settings_path)
    jobs = commands(settings)
    capture = CaptureSettings.parse({**settings["capture"], "budget": settings["calibration_budget"]})
    if len(capture.devices) != settings["deployment"]["tensor_parallel"]:
        raise ValueError("Region calibration needs one visible device per tensor-parallel rank")
    count = {
        "starts": len(jobs),
        "requests": sum(c.requests for c in jobs),
        "input_tokens": sum(c.input_tokens for c in jobs),
        "output_tokens": sum(c.output_tokens for c in jobs),
    }
    if any(count[key] > capture.budget[key] for key in count):
        raise ValueError(f"The fixed shared recipes exceed their declared budget: {count}")

    output.mkdir(parents=True, exist_ok=True)
    declaration = {
        "scope": "Independent fixed recipes, shared by every target; no target or base timing inputs",
        "work": count,
        "budget": capture.budget,
        "commands": [asdict(c) for c in jobs],
    }
    # Paths name mounts, not data loaded from those mounts.
    declaration["commands"] = json.loads(json.dumps(declaration["commands"], default=str))
    if (output / "declaration.json").exists() and read_json(output / "declaration.json") != declaration:
        raise ValueError("The fixed round already belongs to a different declaration; use a new output")
    write_json(output / "declaration.json", declaration)
    if plan_only:
        return {"status": "planned", **count, "declaration": str(output / "declaration.json")}

    imports, anchors, training_anchors = [], {}, set()
    selected = read_json(models_path)
    if selected["deployment"] != settings["deployment"]:
        raise ValueError("The shared anchors belong to a different deployment")

    families = {"sglang_regions", "sglang_cache_io", "sglang_cache_controls"}
    keys = ("operation", "implementation", "timing_scope")
    responses = set()
    connection = sqlite3.connect(database.resolve().as_uri() + "?mode=ro", uri=True)
    try:
        for model in selected["models"]:
            for source in model["sources"]:
                row = connection.execute(
                    "SELECT c.identity,c.adapter,c.context,c.metadata FROM capture_locations l "
                    "JOIN captures c ON l.capture=c.identity WHERE l.path=?",
                    (source,),
                ).fetchone()
                if row is None:
                    raise ValueError(f"A selected independent source is absent from the library: {source}")
                anchors.setdefault(row[0], (source, *row[1:]))
                if row[1] in families:
                    training_anchors.add(row[0])
                    responses.add(tuple(model[key] for key in keys))
    finally:
        connection.close()

    task = DeviceTask(capture, output / "sampling")
    task.recover()
    for command in jobs:
        artifact = task.completed_capture(command)
        if artifact is None:
            captured = task.run(command)
            if captured["status"] != "completed":
                return captured
            artifact = Path(captured[command.result_key])
        imports.append({"adapter": command.measurement_adapter, "capture": str(artifact)})

    begin = time.monotonic()
    try:
        # Stored facts keep their original measurement meaning; do not reparse old raw formats.
        connection = sqlite3.connect(database.resolve().as_uri() + "?mode=ro", uri=True)
        store = PerformanceData(output / "performance.sqlite")
        try:
            for identity, (source, adapter, context, metadata) in anchors.items():
                rows = connection.execute(
                    "SELECT position,quantity,operation,implementation,timing_scope,features,duration_ns,warmup "
                    "FROM measurements WHERE capture=? ORDER BY position,quantity",
                    (identity,),
                )
                samples = (Measurement(*row[:5], json.loads(row[5]), row[6], bool(row[7])) for row in rows)
                store.import_capture(Path(source), adapter, json.loads(context), json.loads(metadata), samples)
        finally:
            connection.close()
            store.close()

        maintenance = {
            "database": str(output / "performance.sqlite"),
            "deployment": settings["deployment"],
            "models": str(models_path.resolve()),
            "imports": imports,
            "training": [row[0] for identity, row in anchors.items() if identity in training_anchors]
            + [row["capture"] for row in imports],
            "responses": [dict(zip(keys, kind)) for kind in sorted(responses)],
            "output": str(output / "selected_models.json"),
        }
        path = output / "maintenance.json"
        write_json(path, maintenance)
        result = maintain_performance(path)
    finally:
        task.record_processing(time.monotonic() - begin)

    if task.remaining()["wall_sec"] < 0:
        raise ValueError("Shared maintenance exceeded the cumulative time budget")
    retained = sum(path.stat().st_size for path in output.rglob("*") if path.is_file())
    if retained > capture.budget["retained_mib"] * 1024**2:
        raise ValueError("The shared captures and published library exceed their retained-space budget")
    result["usage"] = {key: capture.budget[key] - task.remaining()[key] for key in count}
    result["usage"].update(wall_sec=capture.budget["wall_sec"] - task.remaining()["wall_sec"], retained_bytes=retained)
    write_json(output / "result.json", result)
    return result


def bandwidth_reference(settings_path: Path, output: Path, plan_only: bool) -> dict:
    output.mkdir(parents=True, exist_ok=True)
    settings = read_json(settings_path)
    recipe = settings["calibration"]
    command = DeviceCommand(
        "markov.adapters.sglang.cache_io",
        ("--recipe", "/capture/capture_input.json", "--output", "/capture/data"),
        {
            "device": 0,
            "geometry": recipe["geometry"],
            "warmup": 1,
            "repeats": 3,
            "operations": ["bandwidth"],
            "cases": [
                {"page_tokens": page, "pages": math.ceil(recipe["max_context_tokens"] / page), "segments": 1}
                for page in recipe["page_tokens"]
            ],
        },
        "data/capture.json",
        "bandwidth_reference",
        environment=capture_environment(False),
        required_measurement_ports=("device_to_host_data_ns", "host_to_device_data_ns", "storage_shared_data_ns"),
    )
    # One additional weightless start; temporary native KV files are removed by their owner.
    budget = {
        **settings["calibration_budget"],
        "starts": settings["calibration_budget"]["starts"] + 1,
        "retained_mib": 8192,
    }
    capture = CaptureSettings.parse({**settings["capture"], "budget": budget})
    declaration = {
        "command": asdict(command),
        "cumulative_budget": budget,
        "final_retained_mib": settings["calibration_budget"]["retained_mib"],
    }
    write_json(output / "bandwidth_declaration.json", declaration)
    if plan_only:
        return {"status": "planned", "extra_starts": 1, "extra_http_requests": 0, "cumulative_budget": budget}

    task = DeviceTask(capture, output / "sampling")
    task.recover()
    artifact = task.completed_capture(command)
    if artifact is None:
        result = task.run(command)
        if result["status"] != "completed":
            return result
        artifact = Path(result[command.result_key])
    rows = [json.loads(line) for line in (artifact.parent / "measurements.jsonl").read_text().splitlines()]
    rows = [row for row in rows if not row["warmup"]]
    directions = ("device_to_host", "host_to_device", "host_to_storage", "storage_to_host", "storage_shared")
    rates = {}
    for direction in directions:
        values = []
        for page in recipe["page_tokens"]:
            selected = [row for row in rows if row["page_tokens"] == page]
            values.append(
                statistics.median(
                    row["storage_shared_bytes" if direction == "storage_shared" else "transfer_bytes"]
                    * 1e9
                    / row[direction + "_data_ns"]
                    for row in selected
                )
            )
        rates[direction] = min(values)
    result = {
        "status": "completed",
        "bytes_per_sec": rates,
        "capture": str(artifact),
        "selection": "Minimum of the three page-geometry medians, no target observations",
        "scope": "Per-rank native copy data-path NPU events; group buffered file data operations and concurrent read/write. Not physical PCIe or cold-SSD specifications.",
    }
    write_json(output / "bandwidth_reference.json", result)
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--settings", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--models", type=Path, required=True)
    parser.add_argument("--database", type=Path, required=True)
    parser.add_argument("--plan", action="store_true")
    parser.add_argument(
        "--bandwidth-reference",
        action="store_true",
        help="Extend the same shared ledger with one direct data-path reference",
    )
    args = parser.parse_args()
    result = (
        bandwidth_reference(args.settings, args.output.resolve(), args.plan)
        if args.bandwidth_reference
        else run(args.settings, args.output.resolve(), args.models, args.database, args.plan)
    )
    print(json.dumps(result, ensure_ascii=False, indent=2))
    if result["status"] not in ("completed", "planned"):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
