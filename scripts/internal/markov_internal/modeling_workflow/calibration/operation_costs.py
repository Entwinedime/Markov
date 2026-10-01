"""Export shared operation costs after same-capture CPU overhead correction."""

from __future__ import annotations

import argparse
import subprocess
from collections import defaultdict
from pathlib import Path
from statistics import mean
from tempfile import TemporaryDirectory

from ...common.io import load_json, write_json
from ...common.paths import require_repo_path, repo_relative_path
from ...modeling.workload import controlled_request_window, discover_workload_window
from ..context import DiagnosticLevel
from ..planning.profile_runs import discover_profile_runs
from .operation_sampling import index_costs

FIELDS = ("main_us", "main_residual_us", "after_us", "after_residual_us", "worker_us", "dispatch_us", "device_us")


def summarize(audit: dict, operation: str, page_size: int) -> dict:
    if audit.get("cost_basis") != "paired_cpu_service" or not audit.get("cpu_service_file"):
        raise ValueError("Formal operation calibration requires paired CPU service correction")
    groups = defaultdict(list)
    for row in audit["rows"]:
        if row["status"] != "observed":
            raise ValueError("Incomplete operation evidence: " + row.get("reason", "unknown"))
        key = (row["rank"], row["phase"]) if operation == "layer_wait" else row["rank"]
        groups[key].append(row)
    if not groups:
        raise ValueError("Independent calibration contains no operation samples")
    result = dict(
        role="fixed_calibration",
        operation=operation,
        source_manifest=audit["source_manifest"],
        cpu_service_file=audit["cpu_service_file"],
        cost_basis=audit["cost_basis"],
        estimator="Per-rank independent operation means after measured CPU/recorder correction; no target fitting.",
        limitations="Unmeasured residuals, worker dispatch and device costs retained. CPU reductions are allocated within measured intervals, not identified per instruction. Extrapolation beyond sampled sizes is unvalidated.",
    )
    if operation == "layer_wait":
        result["rows"] = [
            dict(rank=rank, phase=phase, samples=len(rows), **{field: mean(r[field] for r in rows) for field in FIELDS})
            for (rank, phase), rows in sorted(groups.items())
        ]
        return result
    result["measured_page_size"] = page_size
    ranks = []
    for rank, rows in sorted(groups.items()):
        if operation == "load_index":
            samples = [index_costs(r) for r in rows]
            operations = {}
            for kind in samples[0]["operations"]:
                first = samples[0]["operations"][kind]
                operations[kind] = {
                    key: mean(s["operations"][kind][key] for s in samples) for key in first if key != "submission_name"
                }
                operations[kind]["submission_name"] = first["submission_name"]
            ranks.append(
                dict(
                    rank=rank,
                    measured_tokens=sorted(r["tokens"] for r in rows),
                    operations=operations,
                    tail_us=mean(s["tail_us"] for s in samples),
                    tail_residual_us=mean(s["tail_residual_us"] for s in samples),
                )
            )
        elif operation == "load_submission":
            samples = [r["costs"] for r in rows]
            ranks.append(
                dict(
                    rank=rank,
                    measured_bytes=[r["bytes"] for r in rows],
                    costs={
                        kind: {field: mean(s[kind][field] for s in samples) for field in samples[0][kind]}
                        for kind in samples[0]
                    },
                )
            )
        else:
            raise ValueError("Unknown operation " + operation)
    result["ranks"] = ranks
    if operation == "load_submission":
        result.update(backend="kernel_ascend", layout="page_first_direct")
    return result


def main():
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
    if require_repo_path(load_json(service)["source_manifest"]).resolve() != source.resolve():
        raise ValueError("CPU correction belongs to another capture")
    window = discover_workload_window({}, source)
    if window is None or window.source != "workload_report.formal_window":
        raise ValueError("Explicit formal calibration window required")
    # Setup requests also measure legal control branches. Their costs are
    # independent calibration evidence, not an extension of the scored window.
    window = controlled_request_window(window)
    modes = dict(layer_wait="audit-layer-wait", load_submission="audit-load-submission", load_index="audit-load")
    operations = list(dict.fromkeys(args.operation or modes))
    modes["write_host"] = "write-host"
    modes["release_host"] = "release-host"
    modes["prefetch_query"] = "prefetch-query"
    if "prefetch_wait" in operations:
        (profile,) = discover_profile_runs((source,))
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
            program = operation in ("prefetch_wait", "prefetch_query", "write_host", "release_host")
            observed = load_json(audit)
            if args.diagnostics == DiagnosticLevel.FULL.value and not program:
                write_json(output / (operation + "_audit.json"), observed)
            costs = observed if program else summarize(observed, operation, args.page_size)
            costs["source_manifest"] = str(repo_relative_path(source))
            costs["cpu_service_file"] = str(repo_relative_path(service))
            write_json(audit, costs)
            # Publish the current extraction atomically; a failed attempt
            # leaves the previous complete cost untouched, not a partial file.
            audit.replace(output / (operation + ".json"))
        result.check_returncode()
    print("Shared operation costs exported from paired independent calibration")


if __name__ == "__main__":
    main()
