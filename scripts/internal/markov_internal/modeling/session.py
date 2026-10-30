"""Container-side local prediction stream; no service, acquisition or evaluation."""

from __future__ import annotations

import argparse
from contextlib import redirect_stdout
import json
from pathlib import Path
import sys

from markov_internal.common.io import load_json
from markov_internal.common.paths import require_repo_path, running_in_modeling_container
from markov_internal.modeling.context import PredictionInputs, WorkflowOptions
from markov_internal.modeling.predictor import PredictionSession
from markov_internal.modeling.types import TargetHiCacheConfig


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--request", type=Path, required=True)
    args = parser.parse_args()
    if not running_in_modeling_container():
        parser.error("prediction sessions run inside the modeling container")
    raw = load_json(require_repo_path(args.request))
    inputs = PredictionInputs(
        tuple(Path(value) for value in raw["source_manifests"]),
        Path(raw["cost_model"]),
        tuple(Path(value) for value in raw["cpu_service_costs"]),
        raw["resource_environment"],
    )
    options = WorkflowOptions.from_inputs(inputs, Path(raw["directory"]), **raw["execution"])
    with PredictionSession(options) as session:
        for line in sys.stdin:
            request = json.loads(line)
            try:
                targets = tuple(TargetHiCacheConfig.from_raw(value) for value in request["targets"])
                output = require_repo_path(request["output_dir"])
                with redirect_stdout(sys.stderr):
                    summary = session.predict(targets, output)
                code = 0 if options.dry_run or summary["prediction"]["status"] == "EXECUTED" else 2
                response = {"event": "batch_completed", "return_code": code}
            except Exception as error:
                print(f"Prediction batch failed: {error}", file=sys.stderr, flush=True)
                response = {"event": "batch_completed", "return_code": 1, "error": str(error)}
            print(json.dumps(response), flush=True)


if __name__ == "__main__":
    main()
