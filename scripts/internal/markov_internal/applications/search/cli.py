"""Search a declared finite HiCache space with the same model as direct prediction."""

from __future__ import annotations

import argparse
from pathlib import Path
import time

from markov_internal.applications.search.request import SearchRequest
from markov_internal.applications.search.workflow import run_search
from markov_internal.common.io import write_json_atomic
from markov_internal.common.paths import require_repo_path, running_in_modeling_container


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--request", required=True, type=Path)
    parser.add_argument(
        "--dry-run", action="store_true", help="Save the plan only; no preparation, training, capture or prediction."
    )
    parser.add_argument(
        "--resume",
        action="store_true",
        help="Reuse this comparison's completed cells; inputs, code and costs must be unchanged.",
    )
    args = parser.parse_args(argv)
    if running_in_modeling_container():
        parser.error("search coordinates preparation and modeling from the host")

    request = SearchRequest.load(require_repo_path(args.request))
    state = run_search(request, dry_run=args.dry_run, resume=args.resume)
    if (
        not args.dry_run
        and request.confirmation["enabled"]
        and state.get("best_candidates")
        and state["stop_reason"] != "interrupted"
    ):
        # The engine never imports target measurement or scoring paths.
        from markov_internal.applications.search.confirmation import confirm

        started = time.monotonic()
        state["confirmation"] = {"status": "CONFIRMING", "enabled": True}
        write_json_atomic(request.output_dir / "search_summary.json", state)
        try:
            state["confirmation"] = confirm(request, state)
        except BaseException as error:
            state["confirmation"] = {
                "status": "INTERRUPTED" if isinstance(error, KeyboardInterrupt) else "FAILED",
                "error": str(error),
            }
            raise
        finally:
            elapsed = time.monotonic() - started
            state["confirmation_wall_seconds"] = state.get("confirmation_wall_seconds", 0.0) + elapsed
            state["total_wall_seconds"] += elapsed
            write_json_atomic(request.output_dir / "search_summary.json", state)

    print(f"search={state['status']} configurations={request.space.count}", flush=True)
    if not args.dry_run:
        print(
            f"stop={state['stop_reason']} charged_predictions={state['charged_predictions']} best={state.get('best_candidates', [])}"
        )
        for candidate in state.get("candidates", []):
            value = candidate["score_sec"]
            print(
                f"  {candidate['name']}: {value:.6f} s"
                if value is not None
                else f"  {candidate['name']}: not fully predicted"
            )
        print(f"report={request.output_dir / 'search_summary.json'}")
        if request.confirmation["enabled"]:
            confirmation = state["confirmation"]
            print(f"confirmation={confirmation['status']} recommendation={confirmation.get('recommended_target')}")
    if state.get("stop_reason") == "interrupted":
        return 130
    if (
        not args.dry_run
        and request.confirmation["enabled"]
        and state["confirmation"]["status"] not in {"MEASURED", "CONFIRMED_IMPROVEMENT", "NO_CONFIRMED_IMPROVEMENT"}
    ):
        return 2

    return 0 if args.dry_run or state.get("successful_configurations", 0) else 2


if __name__ == "__main__":
    raise SystemExit(main())
