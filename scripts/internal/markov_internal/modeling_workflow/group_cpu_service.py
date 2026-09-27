"""Prepare source-bound CPU service once per base, shared by all its targets."""

from __future__ import annotations

from pathlib import Path
import subprocess

from ..common.paths import ROOT_DIR, require_repo_path, repo_relative_path
from ..common.io import load_json
from .context import cpu_service_inputs
from .cpu_capture import capture_light_base
from .planning.profile_runs import parse_server_command_flags


def cpu_service_plan(group) -> dict:
    declarations = group.raw.get("cpu_service_pairs", [])
    budget = group.raw.get("cpu_service_capture_budget")
    sources = {source.manifest_path.resolve() for source in group.sources}
    existing = cpu_service_inputs(
        [require_repo_path(value) for value in group.raw.get("cpu_service_costs", [])], tuple(sources)
    )
    if "cpu_service_pairs" not in group.raw and budget is not None:
        declarations = [
            dict(
                profile_manifest=str(source.manifest_path),
                tp_size=int(parse_server_command_flags(source.run_dir / "server_cmd.txt").get("tp_size", "1")),
            )
            for source in group.sources
            if source.manifest_path.resolve() not in existing
        ]
    seen, pairs = set(existing), []
    for declaration in declarations:
        source = require_repo_path(declaration["profile_manifest"]).resolve()
        light = (
            require_repo_path(declaration["light_manifest"]).resolve() if declaration.get("light_manifest") else None
        )
        if source not in sources or source in seen or light == source:
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
                output_dir=str(repo_relative_path(group.output_dir / "cpu_service" / str(len(pairs) + 1))),
            )
        )
    if declarations and seen != sources:
        raise ValueError("Declared CPU pairs must cover all selected group sources")
    bundle = group.raw.get("forced_token_bundle")
    return dict(
        status="planned" if pairs or existing else "not_requested",
        pairs=pairs,
        capture_budget=budget,
        forced_token_bundle=bundle,
        existing_services=[str(repo_relative_path(path)) for path in existing.values()],
        capture_root=str(repo_relative_path(group.output_dir / "cpu_captures")),
    )


def prepare_group_cpu_service(plan: dict, *, dry_run: bool = False) -> list[Path]:
    outputs = [require_repo_path(path) for path in plan.get("existing_services", [])]
    expected = len(outputs) + len(plan["pairs"])
    for pair in plan["pairs"]:
        if pair["light_manifest"] is None:
            light = capture_light_base(pair, plan, dry_run=dry_run)
            if light is None:
                return outputs

            pair["light_manifest"] = str(repo_relative_path(light))
        output = require_repo_path(pair["output_dir"]) / "cpu_service.json"
        measurements = output.parent / "cpu_measurements.json"
        if output.is_file() and measurements.is_file():
            previous = load_json(measurements)
            prepared = previous.get("preparation", {})
            if (
                require_repo_path(previous["source_manifest"]).resolve()
                == require_repo_path(pair["profile_manifest"]).resolve()
                and require_repo_path(previous["light_manifest"]).resolve()
                == require_repo_path(pair["light_manifest"]).resolve()
                and prepared.get("tp_size") == pair["tp_size"]
                and prepared.get("correct_recorder") is True
            ):
                cpu_service_inputs([output], (require_repo_path(pair["profile_manifest"]).resolve(),))
                outputs.append(output)
                continue
        if dry_run:
            continue
        command = [str(ROOT_DIR / "scripts/model.sh"), "prepare-cpu-service", "--correct-recorder"]
        for key in ("light_manifest", "profile_manifest", "tp_size", "output_dir"):
            command.extend(("--" + key.replace("_", "-"), str(pair[key])))
        subprocess.run(command, cwd=ROOT_DIR, check=True)
        cpu_service_inputs([output], (require_repo_path(pair["profile_manifest"]).resolve(),))
        outputs.append(output)
    plan["status"] = "needs_preparation" if len(outputs) < expected else "prepared" if outputs else "not_requested"
    return outputs
