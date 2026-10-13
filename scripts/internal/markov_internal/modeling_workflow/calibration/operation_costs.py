"""Export shared operation costs after same-capture CPU overhead correction."""

from __future__ import annotations

import argparse
import subprocess
from pathlib import Path
from tempfile import TemporaryDirectory
from typing import Any

from ...common.io import load_json, write_json
from ...common.paths import require_repo_path, repo_relative_path
from ...modeling.source_dag.profiles import discover_profile_runs
from ...modeling.workload import controlled_request_window, discover_workload_window
from ..context import DiagnosticLevel


def summarize(audit: dict[str, Any], operation: str, page_size: int) -> dict[str, Any]:
    """Publish measured scopes; Python does not interpret nodes or dependencies."""

    if not audit["operator_observations"]:
        raise ValueError("Independent calibration contains no usable operation observations")

    result = dict(audit, role="fixed_calibration", operation=operation)
    rows = result.pop("rows", [])
    result["unavailable_samples"] = [row for row in rows if row["status"] not in {"observed", "ready"}]
    if operation != "layer_wait":
        result["measured_page_size"] = page_size
    if operation == "load_submission":
        result.update(backend="kernel_ascend", layout="page_first_direct")

    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile-manifest", type=Path, required=True)
    parser.add_argument("--cpu-service-cost", type=Path, required=True)
    parser.add_argument("--page-size", type=int, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument(
        "--operation",
        action="append",
        choices=(
            "layer_wait",
            "load_submission",
            "load_index",
            "prefetch_wait",
            "prefetch_query",
            "write_host",
            "release_host",
        ),
        help="Export only the requested shared operations; repeat to select more than one.",
    )
    parser.add_argument(
        "--exporter", type=Path, default=Path("build/modeling/trace_graph-release/hicache_calibration_export")
    )
    parser.add_argument("--diagnostics", choices=[level.value for level in DiagnosticLevel], default="off")
    args = parser.parse_args()
    source, service, output, binary = map(
        require_repo_path, (args.profile_manifest, args.cpu_service_cost, args.output_dir, args.exporter)
    )
    if args.page_size <= 0:
        raise ValueError("Page size must be positive")
    correction = load_json(service)
    if require_repo_path(correction["source_manifest"]).resolve() != source.resolve():
        raise ValueError("CPU correction belongs to another capture")
    (profile,) = discover_profile_runs((source,))
    if args.page_size != profile.hicache_config["page_size"]:
        raise ValueError("Operation page size must match the declared capture configuration")
    window = discover_workload_window({}, source)
    if window is None or window.source != "workload_report.formal_window":
        raise ValueError("Explicit formal calibration window required")
    # Setup requests can measure extra branches only when their CPU overhead
    # was paired too. A formal-window correction cannot certify that prelude.
    measured_window = correction.get("measurement_window")
    if measured_window is not None and measured_window["scope"] == "all_controlled_requests":
        window = controlled_request_window(window)
    if measured_window is not None and (
        measured_window["start_us"] > window.start_ns // 1000 or measured_window["end_us"] < window.end_ns // 1000
    ):
        raise ValueError("CPU correction does not cover the operation export window")
    modes = dict(layer_wait="audit-layer-wait", load_submission="audit-load-submission", load_index="audit-load")
    operations = list(dict.fromkeys(args.operation or modes))
    modes["write_host"] = "write-host"
    modes["release_host"] = "release-host"
    modes["prefetch_query"] = "prefetch-query"
    if "prefetch_wait" in operations:
        modes["prefetch_wait"] = "prefetch-wait:" + profile.hicache_config["prefetch_policy"]
    output.mkdir(parents=True, exist_ok=True)
    # Parse and build once; the backend isolates each operation's graph changes.
    # Publish complete earlier exports even if a later operation cannot be extracted.
    with TemporaryDirectory(prefix="operations_", dir=output) as directory:
        scratch = Path(directory)
        command = [str(binary), str(source), str(window.start_ns // 1000), str(window.end_ns // 1000)]
        for index, operation in enumerate(operations):
            command.extend((str(scratch / (operation + ".json")), modes[operation]))
            if index == 0:
                command.append(str(service))
        result = subprocess.run(command, check=False)
        for operation in operations:
            audit = scratch / (operation + ".json")
            if result.returncode != 0 and not audit.exists():
                break
            observed = load_json(audit)
            if args.diagnostics == DiagnosticLevel.FULL.value:
                write_json(output / (operation + "_audit.json"), observed)
            costs = summarize(observed, operation, args.page_size)
            costs["source_manifest"] = str(repo_relative_path(source))
            costs["cpu_service_file"] = str(repo_relative_path(service))
            costs["measurement_manifests"] = [str(repo_relative_path(source)), correction["light_manifest"]]
            costs["measurement_window"] = {
                "scope": "all_controlled_requests"
                if measured_window is not None and measured_window["scope"] == "all_controlled_requests"
                else "formal_workload",
                "start_us": window.start_ns // 1000,
                "end_us": window.end_ns // 1000,
            }
            write_json(audit, costs)
            # Publish the current extraction atomically; a failed attempt
            # leaves the previous complete cost untouched, not a partial file.
            audit.replace(output / (operation + ".json"))
        result.check_returncode()
    print("Shared operation costs exported from paired independent calibration")


if __name__ == "__main__":
    main()
