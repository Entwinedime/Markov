"""Rebuild independent measurements using the declared reproduction recipe."""

from __future__ import annotations

import json
import gzip

from fnmatch import fnmatchcase
from pathlib import Path
from typing import Any, Callable
import math
from statistics import median

from inputs import load_json, relative, require_repo_path, write_json


def model_declarations(inventory: dict[str, Any], rules: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Expand a declared mechanism recipe, not a search over measured errors.

    Only operation identities and feature names are read. Values, sample spread,
    target observations and fitting errors cannot select the model form.
    Every measured coordinate remains a coverage condition unless explicitly
    declared as an extrapolation coordinate by the recipe.
    """

    result = []
    for group in inventory["groups"]:
        if group["timing_scope"].startswith("profiled_"):
            continue
        rule = next(
            item
            for item in rules
            if fnmatchcase(group["operation"], item["operation"])
            and fnmatchcase(group["implementation"], item.get("implementation", "*"))
            and fnmatchcase(group["timing_scope"], item["timing_scope"])
            and ("requires_feature" not in item or item["requires_feature"] in group["features"])
        )
        spec = {
            key: value for key, value in rule.items() if key not in {"operation", "timing_scope", "requires_feature"}
        }
        spec.update(
            operation=group["operation"],
            implementation=group["implementation"],
            timing_scope=group["timing_scope"],
            coverage_features=sorted(set(group["features"]) - set(rule["features"])),
        )
        result.append(spec)
    return result


def control_measurements(
    root: Path,
    manifests: list[Path],
    service_by_source: dict[Path, Path],
    source_info: dict[Path, dict[str, Any]],
    run: Callable[..., int],
) -> list[tuple[str, dict[str, Any]]]:
    """Validate control evidence before the expensive bulk graph import."""

    rows = []
    for index, source in enumerate(manifests):
        service = service_by_source[source]
        # This example creates a fresh database; public import uses the declared
        # manifest path as its capture identity, with no historical aliases.
        capture = relative(source)
        name = source_info[source]["workload_id"]
        release = name.endswith("_release")
        page_size = int(name.split("_page", 1)[1].split("_", 1)[0])
        operations = (
            ["release_host"]
            if release
            else ["prefetch_query", "prefetch_wait", "write_host", "load_submission", "load_index", "layer_wait"]
        )
        operations = source_info[source].get("cost_operations", operations)
        output = root / "costs" / "controls" / str(index + 1)
        command = [
            "scripts/model.sh",
            "export-hicache-operation-costs",
            "--profile-manifest",
            relative(source),
            "--cpu-service-cost",
            relative(service),
            "--page-size",
            str(page_size),
            "--output-dir",
            relative(output),
        ]
        for operation in operations:
            command.extend(("--operation", operation))
        paths = [output / f"{operation}.json" for operation in operations]
        documents = [load_json(path) for path in paths] if all(path.exists() for path in paths) else []
        if not documents or any(
            require_repo_path(document["source_manifest"]) != source
            or require_repo_path(document["cpu_service_file"]) != service
            for document in documents
        ):
            run(f"control_{index + 1}", command)
            documents = [load_json(path) for path in paths]

        rows.extend((capture, document) for document in documents)

    return rows


def prepare_costs(
    recipe: dict[str, Any],
    root: Path,
    manifests: list[Path],
    services: list[Path],
    source_info: dict[Path, dict[str, Any]],
    run: Callable[..., int],
) -> Path:
    """Maintain one independent database/package, never using prediction bases."""

    measurement_root = root / "costs" / recipe["sampling"]["measurement_scope"]
    database = measurement_root / "measurements.sqlite"
    inventory = root / "costs" / "coverage.json"
    if inventory.exists() and (root / "costs" / "control_import" / "import_summary.json").exists():
        return publish_costs(recipe, root, database, inventory, run)

    service_by_source = {require_repo_path(load_json(path)["source_manifest"]): path for path in services}
    controls = control_measurements(root, manifests, service_by_source, source_info, run)
    runtime_import = measurement_root / "runtime_import"
    resources = root / "costs" / "resources.json"
    write_json(resources, recipe["resources"])
    command = [
        "scripts/model.sh",
        "cost-db",
        "import",
        "--database",
        relative(database),
        "--resource-environment",
        relative(resources),
        "--include-setup-requests",
        "--output-dir",
        relative(runtime_import),
    ]
    for path in manifests:
        command.extend(("--profile-manifest", relative(path)))
    for path in services:
        command.extend(("--cpu-service-cost", relative(path)))
    # A resumed experiment consumes its already-admitted independent facts.
    # Model training below still runs again. Changed sampling or extraction
    # semantics require a new output directory, as with capture reuse.
    if not (runtime_import / "import_summary.json").exists():
        run("runtime_import", command)

    inventory = measurement_root / "initial_coverage.json"
    if not inventory.exists():
        run(
            "initial_coverage",
            [
                "scripts/model.sh",
                "cost-db",
                "inspect",
                "--database",
                relative(database),
                "--output",
                relative(inventory),
            ],
        )
    # The public inventory owns the canonical deployment description.
    deployment = load_json(inventory)["groups"][0]["deployment"]
    bootstrap = root / "costs" / "bootstrap_training.json"
    physical_evidence = root / "costs" / "measurements" / "database_evidence.json"
    bootstrap_training = {
        "deployment": deployment,
        "models": [
            {
                "operation": "device.event.record",
                "implementation": "*",
                "timing_scope": "device_service",
                "features": [],
                "form": "constant",
            }
        ],
    }
    if physical_evidence.exists():
        bootstrap_training["io_evidence"] = [row["capture"] for row in load_json(physical_evidence)["evidence"]]

    write_json(
        bootstrap,
        bootstrap_training,
    )

    request = root / "costs" / "performance_request.json"
    physical = {**recipe["physical_capture"], "devices": recipe["devices"], "cpu_sets": "|".join(recipe["cpu_sets"])}
    anchor = next(path for path in manifests if source_info[path]["workload_id"].endswith("_release"))
    physical_request = (
        {"evidence": relative(physical_evidence)}
        if physical_evidence.exists()
        else {
            "profile_manifests": [relative(anchor)],
            "cpu_service_costs": [relative(service_by_source[anchor])],
            "include_setup_requests": True,
            "resource_environment": recipe["resources"],
            "physical_capture": physical,
            "physical_services": [
                "physical/prefetch_stages",
                "physical/load",
                "physical/write_device_to_host",
                "physical/write_host_to_storage_existing",
                "service/write_host_to_storage_new",
            ],
        }
    )
    write_json(
        request,
        {
            "database": relative(database),
            "training": relative(bootstrap),
            "output_dir": relative(root / "costs" / "measurements"),
            # All runtime facts are already in the shared database. Physical
            # sampling needs one same-deployment anchor, not another extraction
            # of every graph. The single-token release workload is sufficient.
            **physical_request,
        },
    )
    # This pass prepares facts and I/O. A missing bootstrap operator is allowed
    # only here; the final publication below must pass the normal readiness gate.
    run("cost_measurements", ["scripts/model.sh", "prepare-costs", "--request", relative(request)], accepted=(0, 2))
    prepared = load_json(root / "costs" / "measurements" / "performance_summary.json")
    if prepared["status"] not in {"ready", "missing_data"}:
        raise RuntimeError(f"independent measurements failed: {prepared['status']}; see {request.parent}")

    rows = []

    sampling = recipe["sampling"]
    for kind in ("idle-sync", "eviction-cpu"):
        output = root / "costs" / kind
        command = [
            "scripts/model.sh",
            "calibrate-hicache",
            kind,
            "--output-dir",
            relative(output),
            "--cpu-sets",
            "|".join(recipe["cpu_sets"]),
            "--batch",
            str(sampling["primitive_batch"]),
            "--repeats",
            str(sampling["primitive_repeats"]),
        ]
        if kind == "idle-sync":
            command.extend(("--devices", ",".join(map(str, recipe["devices"]))))
        else:
            command.extend(("--heap-sizes", ",".join(map(str, sampling["heap_sizes"]))))
        path = output / ("idle_sync.json" if kind == "idle-sync" else "eviction_cpu.json")
        if not path.exists() or load_json(path)["status"] != "measured":
            run(kind, command)
        measured = load_json(path)
        if kind == "idle-sync":
            rows.extend(
                dict(row, capture="cpu_primitive:" + relative(path), manifest=relative(path), deployment=deployment)
                for row in measured["operator_observations"]
            )
        else:
            for sample in measured["samples"]:
                size = sample["heap_size"]
                duration_us = median(pair["active_us"] - pair["baseline_us"] for pair in sample["pairs"])
                if not math.isfinite(duration_us) or duration_us <= 0:
                    raise ValueError(f"Nonpositive or invalid primitive measurement: {path}, heap={size}")
                rows.append(
                    {
                        "capture": "cpu_primitive:" + relative(path),
                        "manifest": relative(path),
                        "deployment": deployment,
                        "position": f"rank:{sample['rank']}:drain:{size}",
                        "operation": "eviction.locked_candidate",
                        "implementation": "lru_heap",
                        "timing_scope": "thread_cpu_service",
                        "features": {"pop_count": float(size), "sum_log2_heap": math.lgamma(size + 1) / math.log(2)},
                        "duration_us": duration_us,
                    }
                )

    observations = root / "costs" / "control_observations.jsonl"
    with observations.open("w") as output:
        for capture, document in controls:
            path = require_repo_path(document["operator_observations_file"])
            opener = gzip.open if path.suffix == ".gz" else open
            with opener(path, "rt", encoding="utf-8") as measured:
                for line in measured:
                    row = dict(json.loads(line), capture=capture, manifest=capture, deployment=deployment)
                    output.write(json.dumps(row, ensure_ascii=False) + "\n")
        for row in rows:
            output.write(json.dumps(row, ensure_ascii=False) + "\n")
    run(
        "control_import",
        [
            "scripts/model.sh",
            "cost-db",
            "import",
            "--database",
            relative(database),
            "--observations",
            relative(observations),
            "--output-dir",
            relative(root / "costs" / "control_import"),
        ],
    )
    inventory = root / "costs" / "coverage.json"
    run(
        "cost_inventory",
        ["scripts/model.sh", "cost-db", "inspect", "--database", relative(database), "--output", relative(inventory)],
    )
    return publish_costs(recipe, root, database, inventory, run)


def publish_costs(recipe: dict[str, Any], root: Path, database: Path, inventory: Path, run: Callable[..., int]) -> Path:
    """Train current declarations from already admitted independent facts."""

    coverage = load_json(inventory)
    package = load_json(root / "costs" / "measurements" / "cost_model.json")
    models = model_declarations(coverage, recipe["sampling"]["response_rules"])
    for shared in recipe["sampling"].get("shared_response_models", []):
        scopes = shared["timing_scope"]
        if isinstance(scopes, str):
            scopes = [scopes]

        models = [
            model
            for model in models
            if not (
                model["operation"] == shared["operation"]
                and model["timing_scope"] in scopes
                and fnmatchcase(model["implementation"], shared["implementation"])
            )
        ]
        models.append(shared)

    training = root / "costs" / "training.json"
    write_json(
        training,
        {
            "deployment": coverage["groups"][0]["deployment"],
            "models": models,
            "io_evidence": package["io_training"]["captures"],
            "excluded_manifests": [],
        },
    )
    final_request = root / "costs" / "publish_request.json"
    write_json(
        final_request,
        {
            "database": relative(database),
            "training": relative(training),
            "output_dir": relative(root / "costs" / "prepared"),
        },
    )
    run("cost_publication", ["scripts/model.sh", "prepare-costs", "--request", relative(final_request)])
    return root / "costs" / "prepared" / "cost_model.json"
