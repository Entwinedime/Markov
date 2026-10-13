"""Plan and prepare same-capture CPU correction for source or independent profiles."""

from __future__ import annotations

from pathlib import Path
import subprocess
from typing import Any

from ..common.paths import ROOT_DIR, require_repo_path, repo_relative_path
from ..common.io import load_json
from ..modeling.source_dag.profiles import ProfileRunRef, cpu_service_inputs, parse_server_command_flags


def cpu_service_plan(sources: tuple[ProfileRunRef, ...], inputs: dict[str, Any], output_dir: Path) -> dict[str, Any]:
    declarations = inputs.get("cpu_service_pairs", [])
    budget = inputs.get("cpu_service_capture_budget")
    selected = {source.manifest_path.resolve() for source in sources}
    existing = cpu_service_inputs(
        [require_repo_path(value) for value in inputs.get("cpu_service_costs", [])], tuple(selected)
    )
    if "cpu_service_pairs" not in inputs and budget is not None:
        declarations = [
            dict(
                profile_manifest=str(source.manifest_path),
                tp_size=int(parse_server_command_flags(source.run_dir / "server_cmd.txt").get("tp_size", "1")),
            )
            for source in sources
            if source.manifest_path.resolve() not in existing
        ]
    seen, pairs = set(existing), []
    for declaration in declarations:
        source = require_repo_path(declaration["profile_manifest"]).resolve()
        light = (
            require_repo_path(declaration["light_manifest"]).resolve() if declaration.get("light_manifest") else None
        )
        if source not in selected or source in seen or light == source:
            raise ValueError("CPU pairs require distinct captures and one selected source per pair")
        tp = declaration["tp_size"]
        if isinstance(tp, bool) or not isinstance(tp, int) or tp <= 0:
            raise ValueError("CPU pair TP size must be a positive integer")
        if (light is not None and not light.is_file()) or not source.is_file():
            raise ValueError("CPU pair capture manifest is missing")
        seen.add(source)
        pairs.append(
            dict(
                light_manifest=str(repo_relative_path(light)) if light else None,
                profile_manifest=str(repo_relative_path(source)),
                tp_size=tp,
                output_dir=str(repo_relative_path(output_dir / "cpu_service" / str(len(pairs) + 1))),
                include_setup_requests=bool(declaration.get("include_setup_requests", False)),
            )
        )
    if declarations and seen != selected:
        raise ValueError("Declared CPU pairs must cover all selected profiles")
    bundle = inputs.get("forced_token_bundle")
    return dict(
        status="planned" if pairs or existing else "not_requested",
        pairs=pairs,
        capture_budget=budget,
        forced_token_bundle=bundle,
        existing_services=[str(repo_relative_path(path)) for path in existing.values()],
        capture_root=str(repo_relative_path(output_dir / "cpu_captures")),
    )


def prepare_cpu_services(plan: dict[str, Any], *, dry_run: bool = False) -> list[Path]:
    outputs = [require_repo_path(path) for path in plan.get("existing_services", [])]
    expected = len(outputs) + len(plan["pairs"])
    for pair in plan["pairs"]:
        if pair["light_manifest"] is None:
            # Existing corrections and pairs do not load the capture path.
            from .cpu_capture import capture_light_base

            light = capture_light_base(pair, plan, dry_run=dry_run)
            if light is None:
                return outputs

            pair["light_manifest"] = str(repo_relative_path(light))
        output = require_repo_path(pair["output_dir"]) / "cpu_service.json"
        measurements = output.parent / "cpu_measurements.json"
        if output.is_file() and measurements.is_file():
            previous = load_json(measurements)
            service = load_json(output)
            prepared = previous.get("preparation", {})
            if (
                require_repo_path(previous["source_manifest"]).resolve()
                == require_repo_path(pair["profile_manifest"]).resolve()
                and require_repo_path(previous["light_manifest"]).resolve()
                == require_repo_path(pair["light_manifest"]).resolve()
                and prepared.get("tp_size") == pair["tp_size"]
                and prepared.get("correct_recorder") is True
                and prepared.get("include_setup_requests", False) == pair.get("include_setup_requests", False)
                and service.get("light_manifest") is not None
                and require_repo_path(service["light_manifest"]).resolve()
                == require_repo_path(pair["light_manifest"]).resolve()
            ):
                cpu_service_inputs([output], (require_repo_path(pair["profile_manifest"]).resolve(),))
                outputs.append(output)
                continue
        if dry_run:
            continue
        command = [str(ROOT_DIR / "scripts/model.sh"), "prepare-cpu-service", "--correct-recorder"]
        if pair.get("include_setup_requests", False):
            command.append("--include-setup-requests")
        for key in ("light_manifest", "profile_manifest", "tp_size", "output_dir"):
            command.extend(("--" + key.replace("_", "-"), str(pair[key])))
        subprocess.run(command, cwd=ROOT_DIR, check=True)
        cpu_service_inputs([output], (require_repo_path(pair["profile_manifest"]).resolve(),))
        outputs.append(output)
    plan["status"] = "needs_preparation" if len(outputs) < expected else "prepared" if outputs else "not_requested"
    return outputs
