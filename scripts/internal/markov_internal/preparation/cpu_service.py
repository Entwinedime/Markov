"""Plan and prepare same-capture CPU correction for source or independent profiles."""

from __future__ import annotations

from dataclasses import asdict, dataclass
from pathlib import Path
import subprocess
from typing import Any

from markov_internal.common.commands import positive_int
from markov_internal.common.paths import ROOT_DIR, require_repo_path, repo_relative_path
from markov_internal.common.io import load_json
from markov_internal.modeling.source_dag.profiles import (
    ProfileRunRef,
    cpu_service_inputs,
    cpu_service_metadata,
    parse_server_command_flags,
)
from markov_internal.preparation.capture import ProfileBudget


@dataclass(frozen=True)
class CpuServicePair:
    profile_manifest: Path
    light_manifest: Path | None
    tp_size: int
    include_setup_requests: bool = False


@dataclass(frozen=True)
class CpuServiceInputs:
    """Declared same-capture corrections, shared by application and maintenance.

    None means pairs were not declared; an explicit empty tuple disables automatic
    pair selection. Paths are resolved at this boundary, not in each consumer.
    """

    costs: tuple[Path, ...]
    pairs: tuple[CpuServicePair, ...] | None
    capture_budget: ProfileBudget | None

    @classmethod
    def parse(cls, raw: dict[str, Any]) -> CpuServiceInputs:
        pairs = None
        if "cpu_service_pairs" in raw:
            parsed = []
            for declaration in raw["cpu_service_pairs"]:
                tp = declaration["tp_size"]
                if isinstance(tp, bool) or not isinstance(tp, int) or tp <= 0:
                    raise ValueError("CPU pair TP size must be a positive integer")

                parsed.append(
                    CpuServicePair(
                        require_repo_path(declaration["profile_manifest"]).resolve(),
                        require_repo_path(declaration["light_manifest"]).resolve()
                        if declaration.get("light_manifest")
                        else None,
                        tp,
                        bool(declaration.get("include_setup_requests", False)),
                    )
                )
            pairs = tuple(parsed)

        budget = raw.get("cpu_service_capture_budget")
        return cls(
            tuple(require_repo_path(value) for value in raw.get("cpu_service_costs", [])),
            pairs,
            ProfileBudget.parse(budget) if budget is not None else None,
        )


def cpu_service_plan(
    sources: tuple[ProfileRunRef, ...],
    inputs: CpuServiceInputs,
    output_dir: Path,
    *,
    forced_token_bundle: str | None,
) -> dict[str, Any]:
    selected = {source.manifest_path.resolve() for source in sources}
    existing = cpu_service_inputs(list(inputs.costs), tuple(selected))
    declarations = inputs.pairs or ()
    if inputs.pairs is None and inputs.capture_budget is not None:
        declarations = tuple(
            CpuServicePair(
                profile_manifest=source.manifest_path.resolve(),
                light_manifest=None,
                tp_size=positive_int(parse_server_command_flags(source.run_dir / "server_cmd.txt").get("tp_size", "1")),
            )
            for source in sources
            if source.manifest_path.resolve() not in existing
        )

    seen, pairs = set(existing), []
    for declaration in declarations:
        source, light = declaration.profile_manifest, declaration.light_manifest
        if source not in selected or source in seen or light == source:
            raise ValueError("CPU pairs require distinct captures and one selected source per pair")
        if (light is not None and not light.is_file()) or not source.is_file():
            raise ValueError("CPU pair capture manifest is missing")

        seen.add(source)
        pairs.append(
            dict(
                light_manifest=str(repo_relative_path(light)) if light else None,
                profile_manifest=str(repo_relative_path(source)),
                tp_size=declaration.tp_size,
                output_dir=str(repo_relative_path(output_dir / "cpu_service" / str(len(pairs) + 1))),
                include_setup_requests=declaration.include_setup_requests,
            )
        )
    if declarations and seen != selected:
        raise ValueError("Declared CPU pairs must cover all selected profiles")

    return dict(
        status="planned" if pairs or existing else "not_requested",
        pairs=pairs,
        capture_budget=asdict(inputs.capture_budget) if inputs.capture_budget is not None else None,
        forced_token_bundle=forced_token_bundle,
        existing_services=[str(repo_relative_path(path)) for path in existing.values()],
        capture_root=str(repo_relative_path(output_dir / "cpu_captures")),
    )


def prepare_cpu_services(plan: dict[str, Any], *, dry_run: bool = False) -> list[Path]:
    outputs = [require_repo_path(path) for path in plan.get("existing_services", [])]
    expected = len(outputs) + len(plan["pairs"])
    for pair in plan["pairs"]:
        if pair["light_manifest"] is None:
            # Existing corrections and pairs do not load the capture path.
            from markov_internal.preparation.cpu_capture import capture_light_base

            light = capture_light_base(pair, plan, dry_run=dry_run)
            if light is None:
                return outputs

            pair["light_manifest"] = str(repo_relative_path(light))
        output = require_repo_path(pair["output_dir"]) / "cpu_service.json"
        measurements = output.parent / "cpu_measurements.json"
        if output.is_file() and measurements.is_file():
            previous = load_json(measurements)
            service = cpu_service_metadata(output)
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
