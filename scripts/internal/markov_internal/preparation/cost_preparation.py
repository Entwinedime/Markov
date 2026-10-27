"""Offline cost-package publication, shared by maintenance and group preparation."""

from __future__ import annotations

from dataclasses import replace
from pathlib import Path
from tempfile import TemporaryDirectory
from typing import Any, TYPE_CHECKING

from markov_internal.common.io import load_json, write_json
from markov_internal.common.paths import ROOT_DIR, repo_relative_path
from markov_internal.common.process import run_command

if TYPE_CHECKING:
    from markov_internal.preparation.scenario import ScenarioRequest


def import_cost_measurements(database: Path, output_dir: Path, arguments: list[str]) -> int:
    """Import declared independent inputs through the same maintenance boundary.

    Callers select measurements and retain the compact import report. This step
    neither captures devices, trains models nor decides prediction readiness.
    """

    return run_command(
        [
            str(ROOT_DIR / "scripts/model.sh"),
            "cost-db",
            "import",
            "--database",
            str(repo_relative_path(database)),
            "--output-dir",
            str(repo_relative_path(output_dir)),
            *arguments,
        ]
    ).returncode


def train_group_cost_model(group: ScenarioRequest, request: dict[str, Any]) -> ScenarioRequest | None:
    """Train from independent data, excluding each base's actual capture lineage.

    Return a group using its newly written HiCache package, or None if none was
    published. Partial operator coverage may satisfy actual execution demands.
    The caller's input package, database and training request are not replaced.
    No trace import, device capture or target evaluation occurs here.
    """

    selected = {
        **request,
        "excluded_manifests": sorted(
            set(request.get("excluded_manifests", []))
            | {str(repo_relative_path(source.manifest_path)) for source in group.sources}
        ),
    }
    model_path = train_cost_model(str(repo_relative_path(group.cost_database)), selected, group.output_dir)
    if model_path is None or "hicache" not in load_json(model_path):
        return None

    return replace(group, cost_model=model_path)


def train_cost_model(database: str, request: dict[str, Any], output_dir: Path) -> Path | None:
    """Publish this attempt's full or partial package; never reuse stale output.

    Imports and captures are separate steps. A partial package is usable only
    for its measured operations, not evidence of E2E readiness.
    """

    request_path = output_dir / "cost_training.json"
    write_json(request_path, request)
    model_path = output_dir / "cost_model.json"

    # An unsuccessful command must not reuse an older package. Return code 2
    # can mean a published subset, so distinguish it from no current output.
    with TemporaryDirectory(prefix="cost_training_", dir=output_dir) as directory:
        trained = Path(directory) / "cost_model.json"
        code = run_command(
            [
                str(ROOT_DIR / "scripts/model.sh"),
                "cost-db",
                "train",
                "--database",
                database,
                "--request",
                str(repo_relative_path(request_path)),
                "--output",
                str(repo_relative_path(trained)),
            ]
        ).returncode
        if code not in {0, 2} or not trained.is_file():
            return None

        trained.replace(model_path)

    return model_path
