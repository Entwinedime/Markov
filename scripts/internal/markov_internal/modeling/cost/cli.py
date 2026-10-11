"""Explicit observation import, offline model training and coverage inspection."""

from __future__ import annotations

import argparse
from pathlib import Path

from ...common.io import load_json, write_json
from ...common.paths import require_repo_path
from .database import CostDatabase, OperationObservation
from .models import estimate
from .package import import_evidence, import_profiles, train


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    actions = parser.add_subparsers(dest="action", required=True)
    importer = actions.add_parser(
        "import", help="admit declared measurements or explicitly selected historical profiles"
    )
    importer.add_argument("--database", type=Path, required=True)
    inputs = importer.add_mutually_exclusive_group(required=True)
    inputs.add_argument("--observations", type=Path, help="JSON with operator observation rows")
    inputs.add_argument("--profile-manifest", type=Path, action="append")
    inputs.add_argument("--evidence", type=Path, help="JSON with explicit physical_io/control_costs measurements")
    importer.add_argument("--cpu-service-cost", type=Path, action="append", default=[])
    importer.add_argument(
        "--include-setup-requests",
        action="store_true",
        help="include recorded setup requests in independent profile measurements; does not change evaluation windows",
    )
    importer.add_argument("--capture-aliases", type=Path, help="explicit manifest-to-original-capture identity map")
    importer.add_argument(
        "--replace-captures",
        action="store_true",
        help="replace all derived measurements of incoming captures; provide complete inputs and retrain exported packages",
    )
    importer.add_argument(
        "--resource-environment",
        type=Path,
        help="explicit device/host/storage/implementation declaration for selected historical profiles",
    )
    importer.add_argument("--output-dir", type=Path, required=True)

    trainer = actions.add_parser("train", help="fit offline from one declared deployment and operation forms")
    trainer.add_argument("--database", type=Path, required=True)
    trainer.add_argument("--request", type=Path, required=True)
    trainer.add_argument("--output", type=Path, required=True)

    inspector = actions.add_parser(
        "inspect", help="read stored coverage or estimate declared work using a trained model"
    )
    inspector.add_argument("--database", type=Path)
    inspector.add_argument("--model", type=Path)
    inspector.add_argument("--work", type=Path, help="operation descriptors to inspect against --model")
    inspector.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    if args.action == "import" and args.resource_environment and not args.profile_manifest:
        parser.error("--resource-environment annotates explicitly selected --profile-manifest inputs only")
    if args.action == "import" and args.include_setup_requests and not args.profile_manifest:
        parser.error("--include-setup-requests requires --profile-manifest")

    if args.action == "import":
        database = CostDatabase(require_repo_path(args.database))
        output = require_repo_path(args.output_dir)
        if args.evidence:
            rows = load_json(require_repo_path(args.evidence))["evidence"]
            result = import_evidence(database, rows, replace_captures=args.replace_captures)
        elif args.observations:
            rows = [
                OperationObservation.from_raw(row)
                for row in load_json(require_repo_path(args.observations))["observations"]
            ]
            result = database.import_measurements(rows, [], replace_captures=args.replace_captures)
        else:
            result = import_profiles(
                database,
                [require_repo_path(path) for path in args.profile_manifest],
                output,
                [require_repo_path(path) for path in args.cpu_service_cost],
                load_json(require_repo_path(args.capture_aliases)) if args.capture_aliases else {},
                load_json(require_repo_path(args.resource_environment)) if args.resource_environment else None,
                include_setup_requests=args.include_setup_requests,
                replace_captures=args.replace_captures,
            )
        write_json(output / "import_summary.json", result)
    elif args.action == "train":
        result = train(CostDatabase(require_repo_path(args.database)), load_json(require_repo_path(args.request)))
        write_json(require_repo_path(args.output), result)
    else:
        if args.database:
            result = CostDatabase(require_repo_path(args.database)).summary()
        elif args.model and args.work:
            models = load_json(require_repo_path(args.model))["operator_models"]
            indexed = {(model["operation"], model["implementation"], model["timing_scope"]): model for model in models}
            results = []
            for work in load_json(require_repo_path(args.work))["operations"]:
                model = indexed.get((work["operation"], work["implementation"], work["timing_scope"]))
                if model is not None:
                    inspection = estimate(model, work["features"])
                else:
                    scopes = sorted(
                        item["timing_scope"]
                        for item in models
                        if (item["operation"], item["implementation"]) == (work["operation"], work["implementation"])
                    )
                    inspection = {
                        "status": "missing_data",
                        "reason": "missing_timing_scope" if scopes else "missing_operation",
                        "available_timing_scopes": scopes,
                    }

                results.append(dict(work=work, cost=inspection))
            result = {
                "status": "missing_data" if any(row["cost"]["status"] != "ready" for row in results) else "ready",
                "scope": "declared work cost queries, not graph construction or E2E readiness",
                "operations": results,
            }
        else:
            parser.error("inspect needs --database or both --model and --work")
        write_json(require_repo_path(args.output), result)

    return 2 if result.get("status") == "missing_data" else 0


if __name__ == "__main__":
    raise SystemExit(main())
