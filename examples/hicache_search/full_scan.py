"""Predict an existing workload over a declared space in bounded, resumable batches."""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import fcntl
from pathlib import Path
import sys
import time

from markov.artifacts.request import PredictionRequest, read_json
from markov.artifacts.writing import write_json
from markov.instrumentation.containers import container_state
from markov.workflow.search import SearchPolicy

from compare import Comparison


class FullScan:
    def __init__(self, path: Path):
        self.path = path.resolve()
        self.root = self.path.parent
        self.settings = read_json(path)
        self.owner = Comparison(path, self.root, 2)
        self.policy = SearchPolicy.parse({"prediction": "unused", **read_json(self.root / "settings.json")["policy"]})
        self.stop = self.owner.stopping

    def batches(self, scene: str, phase: str) -> list[tuple[str, list[str]]]:
        request = PredictionRequest.load(Path(self.settings["requests"][scene]))
        if request.base.manifest is None or request.models is None or request.sampling_budget is not None:
            raise ValueError("Full scans require explicit existing base and models, without automatic sampling")
        selected = self.settings["representatives"]
        remaining = [target.name for target in request.targets if target.name not in selected]
        if phase == "representatives":
            return [("representatives", selected)]
        return [
            (f"batch_{offset // 48:03d}", remaining[offset : offset + 48]) for offset in range(0, len(remaining), 48)
        ]

    def run_batch(self, scene: str, label: str, names: list[str], deadline: float) -> dict:
        directory = self.root / scene / label
        directory.mkdir(parents=True, exist_ok=True)
        request = read_json(Path(self.settings["requests"][scene]))
        targets = {target["name"]: target for target in request["targets"]}
        path = directory / "request.json"
        request["targets"] = [targets[name] for name in names]
        request["execution"]["output"] = str(directory)
        state = directory / "result.json"
        report = directory / "prediction/report.json"
        cid = directory / "prediction/container.id"
        current = container_state(cid)
        if current is not None and current["Running"]:
            raise ValueError(f"Prediction is still active; retain its handle: {cid}")
        if state.exists():
            result = read_json(state)
            if result["status"] != "completed":
                raise ValueError(f"Inspect the previous failed batch before retrying: {state}")
            if read_json(path) != request:
                raise ValueError("Saved batch belongs to different inputs")
            return result
        if report.exists():
            raise ValueError(f"Recover the existing report and its timing before resuming: {report}")
        write_json(path, request)
        entry = self.owner.execute(
            {
                "name": f"{scene}/{label}",
                "command": [sys.executable, "-m", "markov", "predict", "--request", str(path)],
            },
            deadline,
        )
        if entry["status"] != "completed" and not report.exists():
            write_json(state, entry)
            return entry

        prediction = read_json(report)
        if "cells" not in prediction:
            write_json(state, entry)
            return entry
        if len(prediction["cells"]) != len(names) or {cell["name"] for cell in prediction["cells"]} != set(names):
            raise ValueError("Prediction report does not cover this batch")
        cells = []
        for cell in prediction["cells"]:
            formal = [row for row in cell["requests"] if row["formal"]]
            summary = {
                key: cell[key]
                for key in (
                    "name",
                    "status",
                    "formal_http_sec",
                    "resource_reservation",
                    "cost_gaps",
                    "issue",
                    "coverage",
                    "simulation_sec",
                )
            }
            summary["work"] = {
                "formal_prefill_tokens": sum(row["prefill_tokens"] for row in formal),
                "formal_reused_prefix_tokens": sum(
                    row["device_hit_pages"] * cell["configuration"]["page_size"] for row in formal
                ),
                "formal_loaded_pages": sum(row["loaded_pages"] for row in formal),
                "formal_failed_load_attempts": sum(row["failed_load_attempts"] for row in formal),
                "formal_accepted_pages": sum(row.get("prefetch", {}).get("accepted_pages", 0) for row in formal),
                "formal_poll_rounds": sum(row.get("prefetch", {}).get("poll_rounds", 0) for row in formal),
                "all_backup_submissions": cell["backup_submissions"],
                "all_load_submissions": cell["load_submissions"],
                "all_storage": cell.get("storage"),
                "all_host_evicted_pages": cell.get("host_evicted_pages"),
                "qualification": "Reused prefixes include those made available by loading; initial per-tier hits are not separately observed. Formal request sums are separate from all-work counters including preparation and drain; logical work, not physical traffic.",
            }
            cells.append(summary)
        entry.update(
            invocation_status=entry["status"],
            status="completed",
            prediction_status=prediction["status"],
            cells=cells,
            report=str(report),
            source_prepare_sec=prediction["source_prepare_sec"],
            simulation_sec=sum(cell["simulation_sec"] for cell in cells),
            process_peak_rss_bytes=prediction["process_peak_rss_bytes"],
            report_bytes=report.stat().st_size,
            limitations=prediction["limitations"],
            cost_assumptions=prediction["cost_assumptions"],
        )
        write_json(state, entry)
        return entry

    def scene(self, scene: str, phase: str, deadline: float) -> dict:
        batches = []
        for label, names in self.batches(scene, phase):
            if self.stop.is_set() or time.monotonic() >= deadline:
                break
            memory = {row.split(":")[0]: int(row.split()[1]) for row in Path("/proc/meminfo").read_text().splitlines()}
            if memory["MemAvailable"] < 0.2 * memory["MemTotal"] + 20 * 1024**2:
                raise ValueError("Host memory headroom is insufficient; no new batch started")
            result = self.run_batch(scene, label, names, deadline)
            batches.append({key: value for key, value in result.items() if key != "cells"})
            write_json(self.root / scene / f"{phase}.json", {"batches": batches})
            print(f"{scene}/{label}: {result['status']} ({len(names)} configurations)", flush=True)
            if result["status"] != "completed":
                raise RuntimeError(f"Prediction invocation failed: {result}")
            unresolved = [
                cell for cell in result["cells"] if cell["status"] not in {"predicted", "resource_infeasible"}
            ]
            if unresolved:
                write_json(self.root / scene / "unresolved.json", unresolved)
                raise ValueError(f"Structural/cost gaps require review: {scene}, {len(unresolved)} configurations")
            used = sum(path.stat().st_size for path in self.root.rglob("*") if path.is_file())
            if used > 16 * 1024**3:
                raise ValueError("Retained output budget exhausted; stop dispatching")
        return {"scene": scene, "batches": batches}

    def run(self, phase: str) -> None:
        usage_path = self.root / "usage.json"
        usage = read_json(usage_path)
        if phase == "scan":
            for scene in self.settings["requests"]:
                previous = read_json(self.root / scene / "representatives/result.json")
                if len(previous["cells"]) != len(self.settings["representatives"]) or any(
                    cell["status"] != "predicted" for cell in previous["cells"]
                ):
                    raise ValueError("Complete representative coverage before full dispatch")
        begin = time.monotonic()
        remaining = self.settings["wall_limit_sec"] - usage["wall_sec"]
        phase_limit = min(2700, remaining) if phase == "representatives" else remaining - 1800
        if phase_limit <= 0:
            raise ValueError("Execution wall budget exhausted")
        pool = ThreadPoolExecutor(max_workers=2)
        try:
            ordered = sorted(
                self.settings["requests"], key=lambda scene: self.settings["formal_requests"][scene], reverse=True
            )
            futures = [pool.submit(self.scene, scene, phase, begin + phase_limit) for scene in ordered]
            for future in as_completed(futures):
                future.result()
        finally:
            self.stop.set()
            with self.owner.active_lock:
                active = list(self.owner.active.values())
            for process in active:
                self.owner.stop(process)
            pool.shutdown(wait=True, cancel_futures=True)
            usage["wall_sec"] += time.monotonic() - begin
            usage["invocations"].append(
                {
                    "phase": phase,
                    "wall_sec": time.monotonic() - begin,
                    "outer_tree_peak_bytes": int(Path("/sys/fs/cgroup/memory/memory.max_usage_in_bytes").read_text()),
                }
            )
            write_json(usage_path, usage)

    def report(self) -> dict:
        summary = []
        for scene, request_path in self.settings["requests"].items():
            request = PredictionRequest.load(Path(request_path))
            cells, batches = [], []
            directory = self.root / scene
            for path in sorted(directory.glob("*/result.json")):
                batch = read_json(path)
                if batch["status"] != "completed":
                    continue
                cells.extend(batch["cells"])
                batches.append({key: value for key, value in batch.items() if key != "cells"})
            if len(cells) != len({cell["name"] for cell in cells}):
                raise ValueError("Duplicate configurations across saved batches")
            ranked = self.policy.rank({"deployment": self.settings["deployment"], "cells": cells})
            ranked["expected"] = len(request.targets)
            ranked["evaluated"] = len(cells)
            if len(cells) != len(request.targets):
                ranked["status"] = "incomplete"
            ranked.update(scene=scene, batches=batches, work={cell["name"]: cell["work"] for cell in cells})
            write_json(directory / "results.json", ranked)
            summary.append(
                {
                    key: ranked[key]
                    for key in (
                        "scene",
                        "status",
                        "expected",
                        "evaluated",
                        "statuses",
                        "baseline",
                        "recommended",
                        "worst_within_budget",
                    )
                }
            )
        result = {
            "scenes": summary,
            "status": "completed" if all(row["status"] == "completed" for row in summary) else "incomplete",
        }
        write_json(self.root / "results.json", result)
        return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--phase", choices=("representatives", "scan", "report"), required=True)
    args = parser.parse_args()
    experiment = FullScan(args.input)
    with (experiment.root / "full_scan.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        if args.phase == "report":
            print(experiment.report())
        else:
            experiment.run(args.phase)


if __name__ == "__main__":
    main()
