"""Run the declared HiCache panels through the product's existing workflows."""

from __future__ import annotations

import argparse
from collections import defaultdict
from dataclasses import asdict, replace
import fcntl
import io
import json
from pathlib import Path
import subprocess
import tempfile
import time
import urllib.request

from markov.artifacts.request import Deployment, PredictionRequest, read_json, resolve_path
from markov.artifacts.workload import Workload
from markov.artifacts.writing import write_json
from markov.workflow.capture import InferenceCapture
from markov.workflow.device_task import DeviceTask
from markov.workflow.performance import select_models
from markov.workflow.prediction import predict

from report import write_report
from inputs import conversation_prefix


EXAMPLE = Path(__file__).resolve().parent
PROJECT = EXAMPLE.parents[1]


class Matrix:
    def __init__(self, args: argparse.Namespace):
        self.args = args
        self.recipe = read_json(args.recipe)
        self.deployment = Deployment.parse({**self.recipe["deployment"], "model": str(args.model.absolute())})
        self.output = args.output.resolve()
        self.database = args.database.resolve() if args.database else self.output / "performance.sqlite"
        self.reuse = read_json(args.reuse) if args.reuse else {"source": {}, "normal": {}}
        self.reuse_directory = args.reuse.resolve().parent if args.reuse else self.output
        self.capture = dict(self.recipe["capture"])
        if args.devices is not None:
            self.capture["devices"] = args.devices
        self.repetitions = args.repetitions if args.repetitions is not None else self.recipe["normal_repetitions"]
        if self.repetitions < 1:
            raise ValueError("Normal repetitions must be positive")

        self.configurations = {
            name: {**self.recipe["common_configuration"], **configuration}
            for name, configuration in self.recipe["configurations"].items()
        }
        sets = {**self.recipe["configuration_sets"], "all": list(self.configurations)}
        self.panels = list(self.recipe["panels"]) if args.panel == "all" else [args.panel]
        self.cells, self.groups = [], defaultdict(set)
        self.workload_sets = set()
        for panel in self.panels:
            begin = len(self.cells)
            for group in self.recipe["panels"][panel]["groups"]:
                self.workload_sets.add(group["workloads"])
                for workload in self.recipe["workload_sets"][group["workloads"]]:
                    for base in group["bases"]:
                        targets = [target for target in sets[group["targets"]] if target != base]
                        self.groups[(base, workload)].update(targets + [base])
                        self.cells.extend(
                            {"panel": panel, "base": base, "workload": workload, "target": target} for target in targets
                        )
            if len(self.cells) - begin != self.recipe["panels"][panel]["expected_cross"]:
                raise ValueError(f"The declared {panel} layout does not match its cross count")
        for name in set(self.configurations) | {workload for _, workload in self.groups}:
            if name in {".", ".."} or Path(name).name != name:
                raise ValueError("Experiment identifiers must be single path components")

    def progress(self, phase: str, completed: int, total: int, current: str = "") -> None:
        value = {"phase": phase, "completed": completed, "total": total, "current": current}
        write_json(self.output / "progress.json", value)
        print(f"[{time.strftime('%H:%M:%S')}] {phase} {completed}/{total} {current}", flush=True)

    def plan(self) -> dict:
        targets = {(cell["target"], cell["workload"]) for cell in self.cells} | set(self.groups)
        return {
            "panels": self.panels,
            "cross_cells": len(self.cells),
            "self_cells": len(self.groups),
            "base_profiles": len(self.groups),
            "normal_conditions": len(targets),
            "normal_repetitions": self.repetitions,
            "normal_measurements": len(targets) * self.repetitions,
            "additional_structure_profiles": len(targets - set(self.groups)) if self.args.structure else 0,
            "deployment": asdict(self.deployment),
            "devices": self.capture["devices"],
            "database": str(self.database),
            "output": str(self.output),
            "qualification": "Declared tasks only; no hardware or input-domain qualification.",
        }

    def prepare_inputs(self) -> None:
        directory = self.output / "inputs"
        directory.mkdir(parents=True, exist_ok=True)
        if all((directory / f"{workload}.json").is_file() for _, workload in self.groups):
            return

        with tempfile.TemporaryDirectory(prefix="matrix_inputs_", dir=self.output) as temporary:
            dataset = self.args.dataset
            if self.workload_sets & {"development", "confirmation"} and dataset is None:
                dataset = Path(temporary) / "corpus.json"
                self.progress("download_inputs", 0, 1)
                with urllib.request.urlopen(self.recipe["dataset_url"], timeout=60) as response:
                    records = conversation_prefix(
                        io.TextIOWrapper(response, encoding="utf-8"), self.recipe["sharegpt"]["scan_conversations"]
                    )
                write_json(dataset, records)
            args = [
                "docker",
                "run",
                "--rm",
                "--runtime",
                "runc",
                "--network",
                "none",
                "--memory",
                "8g",
                "-e",
                "TORCH_DEVICE_BACKEND_AUTOLOAD=0",
                "-e",
                "PYTHONPATH=/opt/markov/python",
            ]
            mounts = [
                (EXAMPLE, "/example", False),
                (self.args.recipe.resolve(), "/example/recipe.json", False),
                (PROJECT / "python", "/opt/markov/python", False),
                (self.args.model.resolve(), "/model", False),
                (directory, "/inputs", True),
            ]
            if dataset is not None:
                mounts.append((dataset.resolve(), "/corpus.json", False))
            for source, target, writable in mounts:
                args += ["--mount", f"type=bind,source={source},target={target}" + ("" if writable else ",readonly")]
            args += [
                "--entrypoint",
                "python3",
                self.capture["image"],
                "/example/inputs.py",
                "--recipe",
                "/example/recipe.json",
                "--model",
                "/model",
                "--output",
                "/inputs",
                "--sets",
                *sorted(self.workload_sets),
            ]
            if dataset is not None:
                args += ["--dataset", "/corpus.json"]
            with (directory / "preparation.log").open("w", encoding="utf-8") as log:
                subprocess.run(args, stdout=log, stderr=subprocess.STDOUT, check=True)

    def request(self, base: str, workload: str, output: Path, budget_name: str) -> PredictionRequest:
        path = self.output / "requests" / f"{budget_name}_{workload}_{base}.json"
        path.parent.mkdir(parents=True, exist_ok=True)
        work = Workload.load(self.output / "inputs" / f"{workload}.json")
        budget = dict(self.recipe[budget_name])
        budget.update(
            requests=len(work.requests) * budget["starts"],
            input_tokens=sum(len(row.input_ids) for row in work.requests) * budget["starts"],
            output_tokens=sum(row.max_new_tokens for row in work.requests) * budget["starts"],
        )
        write_json(
            path,
            {
                "deployment": asdict(self.deployment),
                "base": {"configuration": self.configurations[base]},
                "workload": str(self.output / "inputs" / f"{workload}.json"),
                "targets": [{"name": base, "configuration": {}}],
                "performance": {"database": str(self.database)},
                "execution": {
                    "output": str(output),
                    "kernel_cache_initial_state": "empty",
                    "memory_limit_gib": self.recipe["prediction_memory_limit_gib"],
                    "capture": {**self.capture, "budget": budget},
                },
            },
        )
        return PredictionRequest.load(path)

    def reused_output(self, kind: str, base: str, workload: str, repetition: int = 0) -> Path | None:
        value = self.reuse.get(kind, {}).get(workload, {}).get(base)
        if value is None or (kind == "normal" and repetition >= len(value)):
            return None
        path = resolve_path(self.reuse_directory, value[repetition] if kind == "normal" else value)
        return resolve_path(path.parent, read_json(path)["execution"]["output"])

    def prediction_request(self, base: str, workload: str, manifest: str, models: Path | None) -> PredictionRequest:
        directory = self.output / ("predictions" if models else "preparation") / workload / base
        directory.mkdir(parents=True, exist_ok=True)
        path = directory / "request.json"
        raw = {
            "deployment": asdict(self.deployment),
            "base": {"manifest": manifest},
            "workload": str(self.output / "inputs" / f"{workload}.json"),
            "targets": [
                {"name": target, "configuration": self.configurations[target]}
                for target in sorted(self.groups[(base, workload)])
            ],
            "performance": {"models": str(models)}
            if models
            else {"database": str(self.database), "sampling_budget": self.recipe["sampling_budget"]},
            "execution": {
                "output": str(directory),
                "kernel_cache_initial_state": "empty",
                "memory_limit_gib": self.recipe["prediction_memory_limit_gib"],
                "record_graph": bool(models and self.args.structure),
            },
        }
        if models is None:
            raw["execution"]["capture"] = {**self.capture, "budget": self.recipe["sampling_budget"]}
        write_json(path, raw)
        return PredictionRequest.load(path)

    def prepare(self) -> None:
        begin = time.monotonic()
        self.prepare_inputs()
        entries = []
        # Larger declared workloads prepare shared domains first, without target timings.
        lengths = {
            workload: max(
                len(row.input_ids) for row in Workload.load(self.output / "inputs" / f"{workload}.json").requests
            )
            for workload in {workload for _, workload in self.groups}
        }
        groups = sorted(
            self.groups,
            key=lambda key: -lengths[key[1]],
        )
        for base, workload in groups:
            self.progress("prepare", len(entries), len(groups), f"{workload}/{base}")
            output = self.reused_output("source", base, workload) or self.output / "sources" / workload / base
            request = self.request(base, workload, output, "source_budget")
            entry = {"base": base, "workload": workload}
            try:
                source = InferenceCapture(request, record_regions=True).run(reuse=True)
                entry["source"] = source
                if source["status"] in {"completed", "reused"}:
                    prepared = predict(self.prediction_request(base, workload, source["manifest"], None))
                    entry["preparation"] = {
                        "status": prepared["status"],
                        "report": str(self.output / "preparation" / workload / base / "prediction/report.json"),
                    }
            except (OSError, ValueError, subprocess.SubprocessError) as error:
                entry["error"] = str(error)
            entries.append(entry)
            write_json(self.output / "preparation.json", {"entries": entries, "wall_sec": time.monotonic() - begin})
        self.progress("prepared", len(entries), len(groups))

    def recover(self) -> None:
        # Recover known owners before revisiting an earlier group; an orphaned
        # container may still be exporting even after its devices become idle.
        owners = []
        for base, workload in self.groups:
            output = self.reused_output("source", base, workload) or self.output / "sources" / workload / base
            owners.append((base, workload, output, output / "base", "source_budget"))
            sampling = self.output / "preparation" / workload / base / "performance" / "sampling"
            owners.append((base, workload, output, sampling, "sampling_budget"))

        conditions = {(cell["target"], cell["workload"]) for cell in self.cells} | set(self.groups)
        for base, workload in sorted(conditions):
            for repetition in range(self.repetitions):
                output = self.reused_output(
                    "normal", base, workload, repetition
                ) or self.output / "normal" / workload / base / str(repetition + 1)
                owners.append((base, workload, output, output / "normal", "normal_budget"))
            output = self.output / "target_profiles" / workload / base
            owners.append((base, workload, output, output / "base", "source_budget"))

        for base, workload, output, directory, budget in owners:
            state = directory / "capture_state.json"
            if not state.exists() or not any(row["status"] == "running" for row in read_json(state)["attempts"]):
                continue
            request = self.request(base, workload, output, "source_budget" if budget == "sampling_budget" else budget)
            settings = request.execution.capture
            if budget == "sampling_budget":
                settings = replace(settings, budget=self.recipe[budget])
            print(f"Recovering the existing device task: {directory}", flush=True)
            DeviceTask(settings, directory).recover()

    def predict(self) -> None:
        prepared = read_json(self.output / "preparation.json")["entries"]
        by_group = {(row["base"], row["workload"]): row for row in prepared}
        models = select_models(
            self.deployment, None, self.database, self.output / "shared_performance", allow_empty=True
        )
        entries = []
        begin = time.monotonic()
        for base, workload in self.groups:
            self.progress("predict", len(entries), len(self.groups), f"{workload}/{base}")
            source = by_group.get((base, workload), {}).get("source", {})
            entry = {"base": base, "workload": workload, "status": "source_unavailable"}
            if source.get("status") in {"completed", "reused"}:
                request = self.prediction_request(base, workload, source["manifest"], models)
                try:
                    result = predict(request)
                    entry.update(
                        status=result["status"],
                        report=str(request.execution.output / "prediction/report.json"),
                        manifest=source["manifest"],
                    )
                except (OSError, ValueError, subprocess.SubprocessError) as error:
                    entry.update(status="prediction_failed", error=str(error))
            entries.append(entry)
            write_json(
                self.output / "predictions.json",
                {"entries": entries, "models": str(models), "wall_sec": time.monotonic() - begin},
            )
        self.progress("predicted", len(entries), len(self.groups))

    def measure(self) -> None:
        conditions = {(cell["target"], cell["workload"]) for cell in self.cells} | set(self.groups)
        entries = []
        begin = time.monotonic()
        for base, workload in sorted(conditions):
            entry = {"configuration": base, "workload": workload, "observations": [], "errors": []}
            for repetition in range(self.repetitions):
                self.progress(
                    "measure",
                    len(entries) * self.repetitions + repetition,
                    len(conditions) * self.repetitions,
                    f"{workload}/{base}/{repetition + 1}",
                )
                output = self.reused_output(
                    "normal", base, workload, repetition
                ) or self.output / "normal" / workload / base / str(repetition + 1)
                request = self.request(base, workload, output, "normal_budget")
                try:
                    result = InferenceCapture(request, profiling=False).run(reuse=True)
                    if result["status"] in {"completed", "reused"}:
                        entry["observations"].append(result["observation"])
                    else:
                        entry["errors"].append(result)
                except (OSError, ValueError, subprocess.SubprocessError) as error:
                    entry["errors"].append(str(error))
                write_json(
                    self.output / "measurements.json",
                    {"entries": entries + [entry], "wall_sec": time.monotonic() - begin},
                )
            entries.append(entry)
        self.progress("measured", len(conditions) * self.repetitions, len(conditions) * self.repetitions)

    def diagnostics(self) -> None:
        # Target wrappers and observations are loaded only after formal prediction.
        from markov.workflow.evaluation import evaluate
        from markov.workflow.replay import replay

        prepared = read_json(self.output / "preparation.json")["entries"]
        profiles = {
            (row["base"], row["workload"]): row["source"]["manifest"]
            for row in prepared
            if row.get("source", {}).get("status") in {"completed", "reused"}
        }
        if self.args.source_reports:
            for (base, workload), manifest in profiles.items():
                directory = self.output / "source_reports" / workload / base
                path = self.output / "requests" / f"source_replay_{workload}_{base}.json"
                path.parent.mkdir(parents=True, exist_ok=True)
                write_json(
                    path,
                    {
                        "manifest": manifest,
                        "output": str(directory),
                        "memory_limit_gib": self.recipe["diagnostic_memory_limit_gib"],
                    },
                )
                replay(path)
        if not self.args.structure:
            return
        for group in read_json(self.output / "predictions.json")["entries"]:
            if "report" not in group:
                continue
            report = read_json(Path(group["report"]))
            for cell in report.get("cells", []):
                if cell["status"] != "predicted":
                    continue
                key = cell["name"], group["workload"]
                if key not in profiles:
                    output = self.output / "target_profiles" / key[1] / key[0]
                    request = self.request(*key, output, "source_budget")
                    result = InferenceCapture(request, record_regions=True).run(reuse=True)
                    if result["status"] not in {"completed", "reused"}:
                        continue
                    profiles[key] = result["manifest"]
                directory = self.output / "structure" / group["workload"] / group["base"] / cell["name"]
                path = self.output / "requests" / f"evaluate_{group['workload']}_{group['base']}_{cell['name']}.json"
                write_json(
                    path,
                    {
                        "prediction": group["report"],
                        "target": cell["name"],
                        "manifest": profiles[key],
                        "output": str(directory),
                        "memory_limit_gib": self.recipe["diagnostic_memory_limit_gib"],
                    },
                )
                evaluate(path)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--recipe", type=Path, default=EXAMPLE / "recipe.json")
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--devices", type=int, nargs="+")
    parser.add_argument("--database", type=Path)
    parser.add_argument("--reuse", type=Path, help="Explicit source/normal request paths; no directory discovery.")
    parser.add_argument("--dataset", type=Path)
    parser.add_argument("--panel", choices=("original", "expanded", "confirmation", "all"), default="original")
    parser.add_argument("--phase", choices=("plan", "prepare", "predict", "measure", "report", "all"), default="all")
    parser.add_argument("--repetitions", type=int)
    parser.add_argument(
        "--structure", action="store_true", help="Record target graphs and evaluate separate target profiles."
    )
    parser.add_argument("--source-reports", action="store_true", help="Run the optional detailed source replay.")
    args = parser.parse_args()
    matrix = Matrix(args)
    if args.phase == "plan":
        print(json.dumps(matrix.plan(), ensure_ascii=False, indent=2), flush=True)
        return

    matrix.output.mkdir(parents=True, exist_ok=True)
    with (matrix.output / "matrix.lock").open("a") as lease:
        fcntl.flock(lease, fcntl.LOCK_EX | fcntl.LOCK_NB)
        matrix.recover()
        write_json(matrix.output / "plan.json", matrix.plan())
        if args.phase in {"all", "prepare"}:
            matrix.prepare()
        if args.phase in {"all", "predict"}:
            matrix.predict()
        if args.phase in {"all", "measure"}:
            matrix.measure()
        if args.phase in {"all", "report"}:
            if args.structure or args.source_reports:
                matrix.diagnostics()
            result = write_report(matrix.output, matrix.cells, matrix.groups)
            complete = result["cross"]["scored"] == result["cross"]["expected"]
            matrix.progress(
                "finished" if complete else "incomplete", result["cross"]["scored"], result["cross"]["expected"]
            )
            if not complete:
                raise SystemExit(1)


if __name__ == "__main__":
    main()
