"""Standalone reproduction example: orchestrate existing public Markov commands."""

from __future__ import annotations

import argparse
from copy import deepcopy
import os
from pathlib import Path
import platform
import shutil
import subprocess
import time
from typing import Any
import urllib.request

from costs import prepare_costs
from inputs import (
    configuration_sets,
    experiment,
    load_json,
    manual_workloads,
    normal_channels,
    panel_layout,
    profile_budget,
    relative,
    replace_command_option,
    require_repo_path,
    sampling_inputs,
    source_jobs,
    target_config,
    write_json,
    write_profile_suite,
)


class CommandFailed(RuntimeError):
    """A public command returned nonzero; its output is retained in the named log."""


class Reproduction:
    """Own experiment paths, not a second profiler, predictor or scoring engine."""

    def __init__(self, recipe: dict[str, Any], root: Path) -> None:
        self.recipe = recipe
        self.root = root
        self.stage = "inputs"
        self.source_info: dict[Path, dict[str, Any]] = {}
        self.capture_results: dict[Path, dict[str, Any]] = {}

    def run(self, label: str, command: list[str], *, accepted: tuple[int, ...] = (0,)) -> int:
        self.stage = label
        print(f"[{label}]", flush=True)
        log = self.root / "logs" / f"{label}.log"
        log.parent.mkdir(parents=True, exist_ok=True)
        with log.open("w", encoding="utf-8") as stream:
            result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, check=False)
        if result.returncode not in accepted:
            raise CommandFailed(f"{label} failed ({result.returncode}); command={command!r}; log={relative(log)}")

        if result.returncode == 0:
            log.unlink()
        return result.returncode

    def build(self) -> None:
        for framework, tag in (("modeling", "ubuntu24.04"), ("sglang", "ubuntu22.04")):
            image = f"markov-trace-sim-{framework}:{tag}"
            if subprocess.run(["docker", "image", "inspect", image], capture_output=True, check=False).returncode:
                self.run(f"image_{framework}", ["scripts/build.sh", framework, "--image-only"])

        self.run("build_hook", ["scripts/build.sh", "sglang", "--hook-only"])
        self.run(
            "build_modeling",
            [
                "scripts/run.sh",
                "modeling",
                "--",
                "bash",
                "-lc",
                "cmake -S src/modeling/trace_graph -B build/modeling/trace_graph-release -G Ninja "
                "-DCMAKE_BUILD_TYPE=Release -DTRACE_GRAPH_DEBUG=OFF && "
                "cmake --build build/modeling/trace_graph-release --target trace_graph hicache_calibration_export -j2",
            ],
        )

    def download_dataset(self) -> Path:
        """Fetch the declared public corpus, without publishing a partial file."""

        destination = self.root / "inputs" / "sharegpt.json"
        if destination.exists():
            return destination

        self.stage = "download_dataset"
        print(f"[download_dataset] {self.recipe['dataset_url']}", flush=True)
        destination.parent.mkdir(parents=True, exist_ok=True)
        partial = destination.with_suffix(".download")
        with urllib.request.urlopen(self.recipe["dataset_url"], timeout=60) as response, partial.open("wb") as stream:
            shutil.copyfileobj(response, stream)
        partial.replace(destination)
        return destination

    def corpus(self, dataset: Path) -> dict[str, list[Path]]:
        """Select development, confirmation and independent data before prediction."""

        destination = self.root / "inputs" / "sharegpt.json"
        if dataset.resolve() != destination.resolve() and not destination.exists():
            destination.parent.mkdir(parents=True, exist_ok=True)
            partial = destination.with_suffix(".copy")
            shutil.copyfile(dataset, partial)
            partial.replace(destination)
        sets = {"manual": manual_workloads(self.recipe)}
        previous = []
        base_policy = load_json(require_repo_path(self.recipe["sharegpt_policy"]))
        for name in ("development", "confirmation", "calibration"):
            policy = deepcopy(base_policy)
            policy["exclude_selection_reports"] = list(previous)
            if name == "confirmation":
                policy.update(self.recipe["confirmation_policy"])
            elif name == "calibration":
                policy["seed"] = self.recipe["sampling"]["seed"]
            output = self.root / "inputs" / name
            policy_path = output / "policy.json"
            report_path = output / "selection_report.json"
            # Reuse declared inputs on an unchanged resumed experiment. This
            # is not a claim that old predictions validate current source code.
            if not report_path.exists():
                write_json(policy_path, policy)
                self.run(
                    f"select_{name}",
                    [
                        "scripts/run.sh",
                        "sglang",
                        "--",
                        "python3",
                        "scripts/bench/hicache_template_workload.py",
                        "--sharegpt-dataset",
                        relative(destination),
                        "--sharegpt-policy",
                        relative(policy_path),
                        "--tokenizer-path",
                        self.recipe["model_path"],
                        "--output-dir",
                        relative(output),
                    ],
                )

            report = load_json(report_path)
            paths = [require_repo_path(row["template"]) for row in report["workloads"].values()]
            if name != "development":
                for path in paths:
                    document = load_json(path)
                    document["id"] = f"{name}_{path.stem}"
                    write_json(path, document)
            sets[name] = paths
            previous.append(relative(report_path))
        return sets

    def capture_one(
        self, item: dict[str, Any], output: Path, *, bundle: Path | None = None, token_only: bool = False
    ) -> Path:
        """One serial public capture; resume only completed non-dry-run results."""

        suite = output / "request.json"
        run_id = load_json(suite)["run_id"] if suite.exists() else "capture"
        result_path = output / run_id / "suite_result.json"
        if result_path.exists():
            previous = load_json(result_path)
            if previous["status"] == "running":
                raise RuntimeError(f"Unsettled capture: {result_path}; stop/reconcile the existing run first")
            if previous["status"] != "completed" or previous["dry_run"]:
                run_id = f"capture_{time.time_ns()}"
                result_path = output / run_id / "suite_result.json"

        if not result_path.exists():
            metadata = {"profile_mode": "forced_token_capture"} if token_only else {}
            write_profile_suite(
                suite, [item], name=item["name"], run_id=run_id, run_root=relative(output), metadata=metadata
            )
            command = ["scripts/profile.sh", relative(suite)]
            if bundle is not None:
                command.extend(("--forced-token-bundle", relative(bundle)))

            label = f"capture_{item['name']}_{output.name}"
            if run_id != "capture":
                label += f"_{run_id}"
            self.run(label, command)

        result = load_json(result_path)
        if result["status"] != "completed" or result["dry_run"] or len(result["profile_manifests"]) != 1:
            raise RuntimeError(f"Capture did not produce exactly one completed manifest: {result_path}")

        manifest = require_repo_path(result["profile_manifests"][0])
        self.capture_results[manifest] = result
        self.source_info[manifest] = item["metadata"]
        return manifest

    def capture(self, suite: Path, output: Path, *, purpose: str, bundle: Path | None = None) -> list[Path]:
        manifests = []
        for item in source_jobs(suite):
            name = item["name"]
            command = item["bench"]["command"]
            selected_bundle = bundle
            if command[command.index("--forced-token-mode") + 1] == "capture":
                # Recording generated tokens is a separate, unprofiled pass.
                tokens = deepcopy(item)
                normal_channels(tokens)
                recorded = self.capture_one(tokens, output / name / "tokens", token_only=True)
                selected_bundle = require_repo_path(
                    self.capture_results[recorded]["generated_forced_token_bundle"]["path"]
                )
                replace_command_option(command, "--forced-token-mode", "replay")
                command.extend(("--forced-token-plan", "{forced_token_plan}"))

            manifests.append(self.capture_one(item, output / name / purpose, bundle=selected_bundle))
        return manifests

    def cpu_services(self, manifests: list[Path], workloads: list[Path], output: Path) -> list[Path]:
        # The public preparation contract selects one base configuration per group.
        by_config: dict[str, list[Path]] = {}
        for manifest in manifests:
            by_config.setdefault(self.source_info[manifest]["config_id"], []).append(manifest)

        services = []
        by_workload = {load_json(path)["id"]: path for path in workloads}

        for config, selected in by_config.items():
            destination = output / config
            request = destination / "request.json"
            write_json(
                request,
                {
                    "base_manifests": list(map(relative, selected)),
                    "cpu_service_pairs": [
                        {
                            "profile_manifest": relative(path),
                            "tp_size": len(self.recipe["devices"]),
                            "include_setup_requests": True,
                        }
                        for path in selected
                    ],
                    "cpu_service_capture_budget": profile_budget(
                        self.recipe,
                        [by_workload[self.source_info[path]["workload_id"]] for path in selected],
                        starts_per_workload=1,
                    ),
                    "resource_environment": self.recipe["resources"],
                    "output_dir": relative(destination),
                },
            )
            self.run(
                f"cpu_{output.parent.name}_{config}",
                ["scripts/model.sh", "prepare-hicache", "--group", relative(request), "--calibration-only"],
            )
            report = load_json(destination / "group_summary.json")
            if report["status"] != "source_inputs_prepared":
                raise RuntimeError(f"CPU preparation incomplete: {destination}")
            services.extend(require_repo_path(value) for value in report["cpu_service_files"])
        return services

    def independent_costs(self, workloads: dict[str, list[Path]]) -> Path:
        self.stage = "independent_costs"
        prompt = next(iter(load_json(workloads["calibration"][0])["request_defs"].values()))["prompt_token_ids"]
        manifests, services = self.sample_costs(self.recipe, prompt)
        for cohort in ("control_sampling", "write_sampling", "path_sampling"):
            if cohort not in self.recipe:
                continue

            recipe = deepcopy(self.recipe)
            recipe["sampling"].update(self.recipe[cohort])
            additional, corrections = self.sample_costs(recipe, prompt)
            manifests.extend(additional)
            services.extend(corrections)

        return prepare_costs(self.recipe, self.root, manifests, services, self.source_info, self.run)

    def sample_costs(self, recipe: dict[str, Any], prompt: list[int]) -> tuple[list[Path], list[Path]]:
        """Capture and CPU-pair one declared independent sampling cohort."""

        sampling = recipe["sampling"]
        sampling_root = self.root / "inputs" / sampling["measurement_scope"] / f"output{sampling['output_tokens']}"
        suite, workloads = sampling_inputs(recipe, sampling_root, prompt)

        manifests = self.capture(suite, self.root / "costs" / "captures", purpose="performance")
        services = self.cpu_services(manifests, workloads, self.root / "costs" / "captures")

        return manifests, services

    def bases(
        self, workloads: dict[str, list[Path]], layout: list[dict[str, Any]]
    ) -> tuple[list[Path], list[Path], Path]:
        specs = {spec["id"]: spec for spec in configuration_sets(self.recipe)["all"]}
        specs_path = self.root / "inputs" / "configs.json"
        write_json(specs_path, {"configs": list(specs.values())})
        selected = {}
        for panel in layout:
            for group in panel["groups"]:
                for path in workloads[group["workload_set"]]:
                    selected[group["base"], load_json(path)["id"]] = path

        # Record each workload once at an already-required base, without profiling.
        first = {}
        for (base, workload), path in selected.items():
            first.setdefault(workload, (base, path))
        bundle = self.root / "inputs" / "forced_token_bundle.json"
        plans = {}
        for name, (base, path) in first.items():
            item = experiment(self.recipe, specs[base], path, specs_path, normal=True)
            command = item["bench"]["command"]
            replace_command_option(command, "--forced-token-mode", "capture")
            offset = command.index("--forced-token-plan")
            del command[offset : offset + 2]
            captured = self.capture_one(item, self.root / "tokens" / name, token_only=True)
            declared = require_repo_path(self.capture_results[captured]["generated_forced_token_bundle"]["path"])
            plan_entry = load_json(declared)["plans"][name]
            destination = bundle.parent / "forced_token_plans" / f"{name}.json"
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(declared.parent / plan_entry["path"], destination)
            plans[name] = {"path": str(destination.relative_to(bundle.parent))}
        write_json(bundle, {"plans": plans})

        suite = self.root / "inputs" / "sources.json"
        write_profile_suite(
            suite,
            [experiment(self.recipe, specs[base], path, specs_path) for (base, _), path in selected.items()],
        )
        manifests = self.capture(suite, self.root / "bases" / "profiles", purpose="base", bundle=bundle)
        services = self.cpu_services(manifests, list(selected.values()), self.root / "bases" / "cpu")
        return manifests, services, bundle

    def predictions(
        self,
        workloads: dict[str, list[Path]],
        layout: list[dict[str, Any]],
        package: Path,
        manifests: list[Path],
        services: list[Path],
    ) -> dict[str, list[Path]]:
        specs = {item["id"]: item for item in configuration_sets(self.recipe)["all"]}
        cost_by_source = {require_repo_path(load_json(path)["source_manifest"]): path for path in services}
        outputs = {}
        for panel in layout:
            outputs[panel["name"]] = []
            for index, group in enumerate(panel["groups"], start=1):
                ids = {load_json(path)["id"] for path in workloads[group["workload_set"]]}
                selected = [
                    path
                    for path in manifests
                    if self.source_info[path]["config_id"] == group["base"]
                    and self.source_info[path]["workload_id"] in ids
                ]
                output = self.root / "predictions" / panel["name"] / str(index)
                request = output / "group_request.json"
                write_json(
                    request,
                    {
                        "base_manifests": [relative(source) for source in selected],
                        "cpu_service_costs": [relative(cost_by_source[source]) for source in selected],
                        "target_configs": [target_config(self.recipe, specs[name]) for name in group["targets"]],
                        "cost_model": relative(package),
                        "resource_environment": self.recipe["resources"],
                        "output_dir": relative(output),
                    },
                )
                command = [
                    "scripts/model.sh",
                    "prepare-hicache",
                    "--group",
                    relative(request),
                    "--model-run-jobs",
                    str(self.recipe["model_run_jobs"]),
                    "--trace-threads",
                    str(self.recipe["trace_threads"]),
                    "--trace-file-threads",
                    str(self.recipe["trace_file_threads"]),
                ]
                if self.recipe["experiment_reports"]:
                    command.append("--experiment-reports")

                self.run(f"predict_{panel['name']}_{index}", command)
                outputs[panel["name"]].append(output / "predictions")
        return outputs

    def completed_predictions(self, layout: list[dict[str, Any]], package: Path) -> dict[str, list[Path]]:
        """Resume measurements only; the caller keeps predictor and costs unchanged."""

        outputs = {}
        for panel in layout:
            outputs[panel["name"]] = []
            for index, group in enumerate(panel["groups"], start=1):
                output = self.root / "predictions" / panel["name"] / str(index)
                request = load_json(output / "group_request.json")
                report = load_json(output / "predictions" / "workflow_summary.json")["prediction"]
                expected = {
                    (group["base"], target, work) for target in group["targets"] for work in group["workload_ids"]
                }
                observed = {
                    (row["source_config_id"], row["target_config"], row["workload_id"]) for row in report["cells"]
                }
                if (
                    require_repo_path(request["cost_model"]).resolve() != package.resolve()
                    or observed != expected
                    or len(report["cells"]) != len(expected)
                    or any(row["status"] != "EXECUTED" or row["http_e2e_us"] <= 0 for row in report["cells"])
                ):
                    raise RuntimeError(
                        f"Normal-only resume requires complete predictions and the selected costs: {output}"
                    )

                outputs[panel["name"]].append(output / "predictions")
        return outputs

    def normal_measurements(
        self, workloads: dict[str, list[Path]], layout: list[dict[str, Any]], bundle: Path
    ) -> dict[tuple[str, str], list[Path]]:
        """Only called after every selected predictor has completed successfully."""

        specs = {item["id"]: item for item in configuration_sets(self.recipe)["all"]}
        specs_path = self.root / "inputs" / "configs.json"
        conditions = {}
        for panel in layout:
            for group in panel["groups"]:
                for path in workloads[group["workload_set"]]:
                    for config in [group["base"], *group["targets"]]:
                        conditions[config, load_json(path)["id"]] = path
        results = {}
        failed = []
        for (config, name), path in conditions.items():
            results[config, name] = []
            for repetition in range(1, self.recipe["normal_repetitions"] + 1):
                output = self.root / "normal" / config / name / str(repetition)
                item = experiment(self.recipe, specs[config], path, specs_path, normal=True)
                try:
                    results[config, name].append(self.capture_one(item, output, bundle=bundle))
                except CommandFailed:
                    failed.append(f"{config}/{name}/{repetition}")
                    print(f"[normal_failed] {failed[-1]}; retained failure, continuing other captures", flush=True)

        if failed:
            self.stage = "normal_http_incomplete"
            raise RuntimeError(f"Normal HTTP samples are incomplete: {failed}; inspect failures before --resume-normal")
        return results

    def evaluate(
        self,
        workloads: dict[str, list[Path]],
        layout: list[dict[str, Any]],
        predictions: dict[str, list[Path]],
        normals: dict[tuple[str, str], list[Path]],
    ) -> list[dict[str, Any]]:
        reports = []
        for panel in layout:
            output = self.root / "evaluation" / panel["name"]
            command = ["scripts/model.sh", "evaluate", "--normal-http", "--output-dir", relative(output)]
            for directory in predictions[panel["name"]]:
                command.extend(("--prediction-dir", relative(directory)))
            selected = set()
            for group in panel["groups"]:
                for path in workloads[group["workload_set"]]:
                    for config in [group["base"], *group["targets"]]:
                        selected.update(normals[config, load_json(path)["id"]])
            for manifest in sorted(selected):
                command.extend(("--target-profile-manifest", relative(manifest)))
            self.run(f"evaluate_{panel['name']}", command, accepted=(0, 2))
            report = load_json(output / "gate_summary.json")
            if (
                report["status"] not in {"PASS", "MODEL_LIMITATION"}
                or report["cell_count"] != panel["cross_count"]
                or report["full_e2e"]["cell_count"] != panel["cross_count"]
            ):
                raise RuntimeError(f"incomplete evaluation: {output}")
            reports.append(
                {
                    "panel": panel["name"],
                    "status": report["status"],
                    "report": relative(output / "gate_summary.json"),
                    **{key: report["full_e2e"][key] for key in ("cell_count", "wape", "p90_ape", "max_ape")},
                }
            )
        return reports


def resources(recipe: dict[str, Any], root: Path) -> dict[str, str]:
    """Describe this allocation; callers can supply a more precise declaration."""

    return {
        "device": f"Ascend devices {recipe['devices']} on {platform.node()}",
        "host": f"{platform.machine()}, NUMA {recipe['numa_nodes']}, CPUs {recipe['cpu_sets']}",
        "storage": f"filesystem device {os.stat(root).st_dev} on {platform.node()}",
        "implementation": "SGLang checkout, Ascend eager, kernel_ascend/page_first_direct/file",
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--recipe", type=Path, default=Path(__file__).with_name("recipe.json"))
    parser.add_argument(
        "--dataset", type=Path, help="Use a local ShareGPT JSON instead of downloading the declared corpus."
    )
    parser.add_argument("--output-dir", type=Path, default=Path("data/reproductions/hicache"))
    parser.add_argument("--model-path", help="Model path visible at the same location on host and in SGLang Docker.")
    parser.add_argument("--resources", type=Path, help="Optional explicit device/host/storage/implementation JSON.")
    parser.add_argument("--model-run-jobs", type=int)
    parser.add_argument("--cost-model", type=Path, help="Reuse an explicitly selected independent cost package.")
    parser.add_argument(
        "--resume-normal",
        action="store_true",
        help="Resume only normal captures and scoring; existing predictions and the selected costs must be unchanged.",
    )
    parser.add_argument(
        "--experiment-reports",
        action="store_true",
        help="Also generate per-cell source replay and structure diagnostics.",
    )
    parser.add_argument(
        "--dry-run", action="store_true", help="Write the experiment plan only; no Docker, build, download or NPU."
    )
    parser.add_argument(
        "--skip-build", action="store_true", help="Use already-built images, hook and modeling binaries."
    )
    args = parser.parse_args(argv)
    if args.resume_normal and args.cost_model is None:
        parser.error("--resume-normal requires the unchanged --cost-model used by the completed predictions")

    os.chdir(args.repo.resolve())
    root = require_repo_path(args.output_dir)
    relative(root)
    recipe = load_json(require_repo_path(args.recipe))
    recipe["experiment_reports"] = args.experiment_reports
    supplied_cost_model = require_repo_path(args.cost_model) if args.cost_model else None
    if args.model_path:
        recipe["model_path"] = args.model_path
    if args.model_run_jobs is not None and args.model_run_jobs < 1:
        parser.error("--model-run-jobs must be positive")
    if args.model_run_jobs:
        recipe["model_run_jobs"] = args.model_run_jobs
    if len(recipe["devices"]) != 2 or recipe["devices"][1] != recipe["devices"][0] + 1:
        parser.error("this reproduction recipe requires two consecutive Ascend devices (TP=2)")
    if len(recipe["cpu_sets"]) != 2 or len(recipe["numa_nodes"]) <= max(recipe["devices"]):
        parser.error("declare per-rank CPU sets and a NUMA map covering the selected device IDs")
    root.mkdir(parents=True, exist_ok=True)
    recipe["resources"] = load_json(args.resources) if args.resources else resources(recipe, root)
    layout = panel_layout(recipe)
    source_conditions = {
        (group["base"], workload) for panel in layout for group in panel["groups"] for workload in group["workload_ids"]
    }
    target_conditions = {
        (config, workload)
        for panel in layout
        for group in panel["groups"]
        for config in [group["base"], *group["targets"]]
        for workload in group["workload_ids"]
    }
    plan = {
        "panels": layout,
        "cross_count": sum(panel["cross_count"] for panel in layout),
        "normal_repetitions": recipe["normal_repetitions"],
        "base_profile_conditions": len(source_conditions),
        "normal_conditions": len(target_conditions),
        "normal_runs": len(target_conditions) * recipe["normal_repetitions"],
        "cost_model": relative(supplied_cost_model) if supplied_cost_model else None,
        "stages": [
            "build",
            "corpus_selection",
            "load_independent_cost_model" if supplied_cost_model else "independent_costs",
            "base_profiles_and_cpu",
            "all_predictions",
            "normal_http",
            "evaluation",
        ],
        "target_measurements_opened_only_after_predictions": True,
        "historical_numerical_results_restored": False,
        "recipe": recipe,
    }
    if args.resume_normal:
        plan["stages"] = ["corpus_selection", "reuse_completed_predictions", "normal_http", "evaluation"]
    write_json(root / "experiment_plan.json", plan)
    if args.dry_run:
        print(f"plan={relative(root / 'experiment_plan.json')} cross={plan['cross_count']}; no execution")
        return 0

    dataset = args.dataset.resolve() if args.dataset else None
    if dataset is not None and not dataset.is_file():
        parser.error("the explicitly supplied --dataset file does not exist")
    if not (Path(recipe["model_path"]) / "config.json").is_file():
        parser.error("the model config/weights must be available on the host and mounted in the SGLang container")

    runner = Reproduction(recipe, root)
    started = time.monotonic()
    summary: dict[str, Any] = {
        "status": "running",
        "pid": os.getpid(),
        "started_at_sec": time.time(),
        "cross_count": plan["cross_count"],
        "resume_normal": args.resume_normal,
        "panels": [],
    }
    write_json(root / "run_summary.json", summary)
    try:
        if not args.skip_build and not args.resume_normal:
            runner.build()
        if dataset is None:
            dataset = runner.download_dataset()
        workloads = runner.corpus(dataset)
        package = supplied_cost_model if supplied_cost_model else runner.independent_costs(workloads)
        if args.resume_normal:
            predictions = runner.completed_predictions(layout, package)
            bundle = root / "inputs" / "forced_token_bundle.json"
        else:
            manifests, services, bundle = runner.bases(workloads, layout)
            predictions = runner.predictions(workloads, layout, package, manifests, services)
        normals = runner.normal_measurements(workloads, layout, bundle)
        summary["panels"] = runner.evaluate(workloads, layout, predictions, normals)
        summary["status"] = (
            "completed"
            if all(row["status"] == "PASS" for row in summary["panels"])
            else "completed_with_model_limitations"
        )
        summary["cost_model"] = relative(package)
        lines = [
            "# HiCache 重建结果",
            "",
            "完整 HTTP 正式窗口；未排除 gap、未归属成本或 Prefill/Decode。",
            "",
            "| 面板 | cross 格数 | WAPE | P90 | 最大误差 | 状态 |",
            "| --- | ---: | ---: | ---: | ---: | --- |",
        ]
        lines.extend(
            f"| {row['panel']} | {row['cell_count']} | {row['wape']:.3%} | {row['p90_ape']:.3%} | "
            f"{row['max_ape']:.3%} | {row['status']} |"
            for row in summary["panels"]
        )
        lines.extend(
            [
                "",
                "正常测量只在全部预测完成后执行；评分不补采、不训练、不修改预测。",
                "本次从新采集重建面板，不恢复已删除的历史 token、参数或分数。",
                "逐格源回放与结构报告仅在 --experiment-reports 下生成；未另采 target 结构，不能把性能通过当成整图结构验证。",
                "成本覆盖、外推及保留等待以各格正式报告为准；模型或测量缺失不填零。",
                "",
            ]
        )
        (root / "results.md").write_text("\n".join(lines), encoding="utf-8")
        print(f"{summary['status']}: {plan['cross_count']} cross cells; {relative(root / 'run_summary.json')}")
        return 0 if summary["status"] == "completed" else 2
    except BaseException as error:
        summary.update(status="failed", stopped_at=runner.stage, error=f"{type(error).__name__}: {error}")
        raise
    finally:
        summary["wall_seconds"] = time.monotonic() - started
        write_json(root / "run_summary.json", summary)


if __name__ == "__main__":
    raise SystemExit(main())
