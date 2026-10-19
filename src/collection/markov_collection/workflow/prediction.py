"""Persisted prediction workflow and its isolated, container-owned CPU processing."""

from __future__ import annotations

import argparse
import fcntl
import json
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any

from markov_collection.artifacts.json_file import read_json, write_json
from markov_collection.workflow.collection import CaptureProcessingFailed, CollectionWorkflow
from markov_collection.workflow.comparison import ServingComparison
from markov_collection.workflow.modeling_job import ModelingJob, ModelingJobFailed
from markov_collection.workflow.performance import PerformanceImport
from markov_collection.workflow.run_directory import RunDirectory
from markov_collection.workflow.summary import RunSummary
from markov_collection.workflow.validation import ValidationWorkflow


class PredictionProcessing:
    def __init__(self, project: Path, directory: Path, catalogs: Path) -> None:
        self.project = project
        self.directory = directory
        self.catalogs = catalogs

    def run(self) -> dict[str, Any]:
        # This lock belongs to the container, so a stopped host cannot start a
        # second writer for the same round. Published results belong to this round.
        with (self.directory / "processing.lock").open("a", encoding="utf-8") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)

            try:
                return read_json(self.directory / "result.json")
            except FileNotFoundError:
                pass

            root = Path(__file__).resolve().parents[4]
            result = subprocess.run(
                [
                    str(root / "build/markov/markov"),
                    "predict",
                    "--project",
                    str(self.project),
                    "--catalog-directory",
                    str(self.catalogs),
                ],
                check=True,
                stdout=subprocess.PIPE,
                text=True,
            )

            prediction = json.loads(result.stdout)
            write_json(self.directory / "result.json", prediction)
            return prediction


class PredictionWorkflow:
    def __init__(self, collection: CollectionWorkflow) -> None:
        self.collection = collection
        self.environment = collection.environment

    def predict(
        self,
        project: Path,
        *,
        validation_budget_sec: int | None = None,
        model_path: Path | None = None,
        devices: list[int] | None = None,
    ) -> dict[str, Any]:
        bundle = self.collection.prepare(project, "prediction", "transfer")
        inputs = bundle["project"]
        # Overrides become part of the one normalized input snapshot. All
        # actual planning and execution still pass through the C++ reader.
        if model_path is not None:
            inputs["model"] = {"path": str(model_path), "tokenizer_path": str(model_path)}
        if devices is not None:
            for item in inputs["configurations"]:
                if item["environment"] is None:
                    raise ValueError("device overrides require a declared deployment environment")
                item["environment"]["devices"] = devices
        run = RunDirectory.create(
            self.environment.host_path(inputs["output"]["directory"]),
            inputs["configurations"][0]["framework"],
            inputs["model"]["path"],
        )
        directory = run.root
        print(f"Run directory: {directory}", file=sys.stderr, flush=True)
        input_directory = run.inputs
        input_directory.mkdir(parents=True)
        for filename, configuration in bundle["configurations"].items():
            write_json(input_directory / filename, configuration)
        for filename, workload in bundle["request_workloads"].items():
            write_json(input_directory / filename, workload)
        write_json(input_directory / "project.json", inputs)

        state: dict[str, Any] = {
            "id": run.identity,
            "project": str(project),
            "task_status": "planned",
            "status": "needs_data",
            "collection": {
                role: {"consumed_wall_time_ns": 0, **permission} for role, permission in inputs["collection"].items()
            },
            "attempts": [],
            "capture": None,
            "comparison": None,
            "summary": None,
            "validation": None,
            "prediction": {"round": 0, "result": None},
            "representatives": {},
            "active_representative": None,
            "performance_imports": {},
            "performance_plan": None,
            "performance_sampling": {},
            "active_performance": None,
        }

        if validation_budget_sec is not None:
            state["validation"] = {"budget_per_combination_sec": validation_budget_sec, "observations": None}
            state["collection"]["validation"] = {
                "allow_run": True,
                "max_wall_time_sec": validation_budget_sec
                * len(inputs["configurations"])
                * len(inputs["request_workloads"]),
                "consumed_wall_time_ns": 0,
            }

        write_json(directory / "run.json", state)
        return self.resume(directory)

    def _result(self, directory: Path, state: dict[str, Any]) -> dict[str, Any]:
        result: dict[str, Any] = {}
        if state["prediction"]["result"] is not None:
            result = read_json(directory / state["prediction"]["result"])

        live, active_ns = self.collection.reconcile(state)
        if result:
            result["collection_performed"] = bool(state["attempts"])
            result["collection_usage"] = {
                role: {"consumed_wall_time_ns": permission["consumed_wall_time_ns"]}
                for role, permission in state["collection"].items()
            }

        prediction_job = self._prediction_job(directory, state)
        prediction_live = prediction_job is not None and prediction_job.running()

        return {
            **result,
            **state,
            "run": str(directory),
            "live_attempt": live,
            "active_wall_time_ns": active_ns,
            "prediction_live": prediction_live,
            "performance_live": self._performance_live(directory, state),
        }

    def _prediction_job(self, directory: Path, state: dict[str, Any]) -> ModelingJob | None:
        ordinal = state["prediction"]["round"]
        if ordinal == 0:
            return None

        run = RunDirectory(directory, state["id"])
        task = f"prediction-{ordinal:03d}"
        return ModelingJob(self.environment, run.container(task), run.work("prediction", ordinal), run.log(task))

    def status(self, directory: Path) -> dict[str, Any]:
        state = read_json(directory / "run.json")
        return self._result(directory, state)

    def compare(self, directory: Path, manifests: list[Path]) -> dict[str, Any]:
        with (directory / "run.lock").open("a", encoding="utf-8") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            state = read_json(directory / "run.json")
            if state["task_status"] != "finished":
                raise ValueError("comparison requires a finished prediction task")
            comparison = ServingComparison(
                self.environment, directory / "inputs/project.json", directory / state["prediction"]["result"]
            ).run(manifests)
            relative = Path("results/comparison.json")
            write_json(directory / relative, comparison)
            state["comparison"] = str(relative)
            if state["validation"] is not None:
                # Exploration includes independent comparison in its outcome.
                # Imported ports do not rewrite native attempts or their budget.
                state["status"] = "complete" if comparison["status"] == "complete" else "needs_data"
                if state["status"] == "complete":
                    state.pop("reason", None)
                else:
                    state["reason"] = "independent_validation_incomplete"
            write_json(directory / "run.json", state)
            self._summarize(directory, state)

        return {
            "run": str(directory),
            "comparison": str(directory / relative),
            "status": comparison["status"],
            "combinations": [
                {key: value for key, value in row.items() if key not in {"requests", "gaps"}}
                for row in comparison["combinations"]
            ],
        }

    def _performance_live(self, directory: Path, state: dict[str, Any]) -> bool:
        for manifest, imported in state.get("performance_imports", {}).items():
            if imported["cost_models"] is None:
                fitting = PerformanceImport(
                    self.environment,
                    self.environment.host_path(manifest),
                    RunDirectory(directory, state["id"]),
                    imported["ordinal"],
                )
                if fitting.job.running():
                    return True
        return False

    def resume(
        self,
        directory: Path,
        *,
        budgets: dict[str, int] | None = None,
        template_assets: Path | None = None,
        performance_manifests: list[Path] | None = None,
        cost_models: list[Path] | None = None,
    ) -> dict[str, Any]:
        revisions = (
            bool(budgets) or template_assets is not None or performance_manifests is not None or cost_models is not None
        )
        with (directory / "run.lock").open("a", encoding="utf-8") as lock:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                result = {**self.status(directory), "workflow_busy": True}
                if revisions:
                    result["requested_changes_applied"] = False
                return result

            state = read_json(directory / "run.json")
            self._apply_input_revision(directory, state)
            live, _ = self.collection.reconcile(state)
            write_json(directory / "run.json", state)
            prediction_job = self._prediction_job(directory, state)
            prediction_live = prediction_job is not None and prediction_job.running()
            if live or prediction_live or self._performance_live(directory, state):
                result = self._result(directory, state)
                if revisions:
                    result["requested_changes_applied"] = False
                return result

            # Complete a recorded representative's handoff before replacement;
            # its imported asset must not override the revision.
            active = state["active_representative"]
            completed = bool(state["attempts"]) and state["attempts"][-1]["status"] == "completed"
            if (
                revisions
                and completed
                and active is not None
                and len(state["attempts"]) > state["capture"]["attempt_index"]
            ):
                self._collect_representative(directory, state)
            active_performance = state["active_performance"]
            if revisions and completed and active_performance is not None:
                if len(state["attempts"]) > state["capture"]["attempt_index"]:
                    self._collect_performance(directory, state)

            if revisions:
                self._revise_assets(
                    directory, state, budgets or {}, template_assets, performance_manifests, cost_models
                )

            capture = state["capture"]
            if capture is not None and (
                state["active_representative"] is not None
                or state["active_performance"] is not None
                or (state["task_status"] == "validating" and capture["role"] == "validation")
            ):
                self.collection.retry_native(directory, state)

            if (
                state["task_status"] == "finished"
                and state["active_representative"] is None
                and state["active_performance"] is None
            ):
                self._cleanup(directory, state)
                self._summarize(directory, state)
                return self._result(directory, state)

            try:
                if state["task_status"] == "validating":
                    self._validate(directory, state)
                else:
                    self._continue(directory, state)
            except KeyboardInterrupt:
                # Preserve the current owner's phase. Resume reconciles its
                # live or terminal job before selecting new work.
                state["reason"] = "prediction_workflow_interrupted"
                write_json(directory / "run.json", state)
                self._summarize(directory, state)
            except Exception as error:
                state.update(status="failed", reason=str(error))
                write_json(directory / "run.json", state)
                self._summarize(directory, state)
                raise

            return self._result(directory, state)

    def _revise_assets(
        self,
        directory: Path,
        state: dict[str, Any],
        budgets: dict[str, int],
        template_assets: Path | None,
        performance_manifests: list[Path] | None,
        cost_models: list[Path] | None,
    ) -> None:
        for role, seconds in budgets.items():
            if role not in state["collection"]:
                raise ValueError(f"this task has no {role} budget")
            if seconds <= 0 or seconds * 1_000_000_000 < state["collection"][role]["consumed_wall_time_ns"]:
                raise ValueError(f"{role} total budget must be positive and cover its consumed time")

        validation = state["validation"]
        if validation is not None and (state["task_status"] == "validating" or validation["observations"] is not None):
            if set(budgets) - {"validation"} or any(
                value is not None for value in (template_assets, performance_manifests, cost_models)
            ):
                raise ValueError("prediction is fixed for validation; asset changes require a new exploration")
            for role, seconds in budgets.items():
                state["collection"][role].update(allow_run=True, max_wall_time_sec=seconds)
            state["task_status"] = "validating"
            write_json(directory / "run.json", state)
            return

        templates = None
        declared = [(path, "performance") for path in performance_manifests or []]
        if template_assets is not None:
            templates = read_json(template_assets)
            if not isinstance(templates, list):
                raise ValueError("template assets must be an array")
            for asset in templates:
                if not isinstance(asset, dict) or asset.keys() - {"manifest", "node_durations"}:
                    raise ValueError("template assets require manifest and optional node_durations")
                manifest = (template_assets.parent / asset["manifest"]).resolve()
                declared.append((manifest, "base"))
                asset["manifest"] = self.environment.container_path(manifest)
                if asset.get("node_durations") is not None:
                    asset["node_durations"] = self.environment.container_path(
                        (template_assets.parent / asset["node_durations"]).resolve()
                    )
                else:
                    asset["node_durations"] = None
        for path, role in declared:
            if read_json(path)["role"] != role:
                raise ValueError(f"resume import requires role={role}: {path}")

        project_path = directory / "inputs/project.json"
        project = read_json(project_path)
        for role, seconds in budgets.items():
            state["collection"][role].update(allow_run=True, max_wall_time_sec=seconds)
            if role != "validation":
                project["collection"][role] = {"allow_run": True, "max_wall_time_sec": seconds}
        if templates is not None:
            project["assets"]["templates"] = templates
        if performance_manifests is not None:
            manifests = [self.environment.container_path(path) for path in performance_manifests]
            # A completed automatic handoff belongs to this task. Replacing
            # declared imports must not discard its measured data or fitted job.
            for sampling in state["performance_sampling"].values():
                manifest = sampling.get("manifest")
                if sampling["status"] in ("captured", "no_candidates") and manifest not in manifests:
                    manifests.append(manifest)
            project["assets"]["performance_manifests"] = manifests

        if cost_models is not None:
            project["assets"]["cost_models"] = [self.environment.container_path(path) for path in cost_models]

        # Quotas do not change simulation inputs or ownership of an unfinished
        # capture. Explicit resume retries that same handoff with the new budget.
        if all(value is None for value in (template_assets, performance_manifests, cost_models)):
            write_json(project_path, project)
            if (
                state["task_status"] == "finished"
                or state["active_representative"] is not None
                or state["active_performance"] is not None
            ):
                state["task_status"] = "selecting" if state["prediction"]["result"] is not None else "planned"
            state.pop("reason", None)
            write_json(directory / "run.json", state)
            return

        # A remaining selection has no attempt. Revised assets and budgets must
        # be considered before deciding whether this group still needs capture.
        active = state["active_representative"]
        if active is not None:
            del state["representatives"][active]
            state["active_representative"] = None
        active_performance = state["active_performance"]
        if active_performance is not None:
            del state["performance_sampling"][active_performance]
            state["active_performance"] = None
        state["performance_plan"] = None

        # No result from the previous inputs may be presented as this revision.
        # The pending input lives in the same atomic ledger as its invalidation;
        # a stopped writer finishes this handoff before any further work.
        state["comparison"] = None
        state["summary"] = None
        state["prediction"]["result"] = None
        state["pending_input"] = project

        state.update(task_status="planned", status="needs_data")
        state.pop("reason", None)

        write_json(directory / "run.json", state)
        self._apply_input_revision(directory, state)

    def _apply_input_revision(self, directory: Path, state: dict[str, Any]) -> None:
        project = state.get("pending_input")
        if project is None:
            return

        for name in ("prediction.json", "comparison.json", "summary.md", "sampling-plan.json"):
            (directory / "results" / name).unlink(missing_ok=True)
        write_json(directory / "inputs/project.json", project)
        del state["pending_input"]
        write_json(directory / "run.json", state)

    def _performance_import(self, directory: Path, state: dict[str, Any], manifest: str) -> list[str]:
        imports = state["performance_imports"]
        if manifest not in imports:
            imports[manifest] = {"ordinal": len(imports) + 1, "cost_models": None}
            write_json(directory / "run.json", state)

        imported = imports[manifest]
        if imported["cost_models"] is None:
            print(f"Fitting performance source {imported['ordinal']}", file=sys.stderr, flush=True)
            result = PerformanceImport(
                self.environment,
                self.environment.host_path(manifest),
                RunDirectory(directory, state["id"]),
                imported["ordinal"],
            ).run()
            imported["cost_models"] = result["cost_models"]
            imported["units"] = result["units"]
            write_json(directory / "run.json", state)
            # The task ledger now owns the candidate references; a fit receipt
            # is no longer needed for recovery after this handoff.
            RunDirectory(directory, state["id"]).consume_work("fitting", imported["ordinal"])
        return imported["cost_models"]

    def _performance_models(self, directory: Path, state: dict[str, Any], inputs: dict[str, Any]) -> list[str]:
        models = list(inputs["assets"]["cost_models"])
        for manifest in inputs["assets"]["performance_manifests"]:
            # Explicit candidates retain priority; the same fitted inputs are
            # shared by all combinations and later representative rounds.
            for model in self._performance_import(directory, state, manifest):
                if model not in models:
                    models.append(model)
        return models

    def _simulate(self, directory: Path, state: dict[str, Any]) -> dict[str, Any]:
        run = RunDirectory(directory, state["id"])
        ordinal = state["prediction"]["round"]
        if state["task_status"] != "predicting":
            ordinal += 1
            print(f"Predicting combinations, round {ordinal}", file=sys.stderr, flush=True)
        round_directory = run.work("prediction", ordinal)
        if state["task_status"] != "predicting":
            # A host may stop after creating the directory and before saving the
            # round identity; no worker has been launched at this point.
            round_directory.mkdir(parents=True, exist_ok=True)
            inputs = read_json(directory / "inputs/project.json")
            inputs["assets"]["cost_models"] = self._performance_models(directory, state, inputs)
            for workload in inputs["request_workloads"]:
                workload["path"] = self.environment.container_path(directory / "inputs" / workload["path"])
            # Detailed graphs have their own retained location. Temporary round
            # inputs/results can be removed after the complete report is published.
            inputs["output"]["directory"] = self.environment.container_path(run.diagnostics("prediction", ordinal))
            for candidate in inputs["configurations"]:
                candidate["config"] = self.environment.container_path(directory / "inputs" / candidate["config"])
            write_json(round_directory / "project.json", inputs)
            state["prediction"]["round"] = ordinal
            state["task_status"] = "predicting"
            write_json(directory / "run.json", state)

        project = round_directory / "project.json"
        inputs = read_json(project)

        # Expose only declared prediction inputs; the full repository is not mounted.
        mounts = {
            self.collection.root / "build/markov/markov",
            self.collection.root / "src/collection",
            directory / "inputs",
        }

        # Fitted models contain all query values. Raw sample references are
        # provenance and are not opened by the prediction consumer.
        mounts.update(self.environment.host_path(path) for path in inputs["assets"]["cost_models"])
        declared = [(path, "performance") for path in inputs["assets"]["performance_manifests"]]
        for asset in inputs["assets"]["templates"]:
            declared.append((asset["manifest"], "base"))
            if asset["node_durations"] is not None:
                mounts.add(self.environment.host_path(asset["node_durations"]))
        for path, role in declared:
            manifest = self.environment.host_path(path)
            metadata = read_json(manifest)
            if metadata["role"] != role:
                raise ValueError(f"prediction import requires role={role}: {manifest}")
            mounts.add(manifest)
            if role == "base":
                asset = metadata.get("compiled_templates")
                if asset is not None and (manifest.parent / asset).is_file():
                    mounts.add((manifest.parent / asset).resolve())

        self._prediction_job(directory, state).run(
            [
                "python3",
                "-m",
                "markov_collection.workflow.prediction",
                "--project",
                self.environment.container_path(project),
                "--directory",
                self.environment.container_path(round_directory),
                "--catalog-directory",
                self.environment.container_path(run.catalogs),
            ],
            mounts,
            {run.catalogs, run.diagnostics("prediction", ordinal)}
            if inputs["output"]["diagnostics"]
            else {run.catalogs},
        )

        return read_json(round_directory / "result.json")

    def _publish(self, directory: Path, state: dict[str, Any], result: dict[str, Any]) -> None:
        result["collection_performed"] = bool(state["attempts"])
        # The next sampling plan consumes the same durable attempt history.
        # Regrouping missing objects must not restart an unchanged native point.
        result["performance_sampling"] = state["performance_sampling"]
        usage = {
            role: {"consumed_wall_time_ns": permission["consumed_wall_time_ns"]}
            for role, permission in state["collection"].items()
        }

        result["collection_usage"] = usage
        collection_failures = {}
        for group in result["representative_collection"].get("groups", []):
            reason = state["representatives"].get(group["representative_configuration_id"])
            if reason is not None:
                for configuration in group["configuration_ids"]:
                    collection_failures[configuration] = reason

        for candidate in result["combinations"]:
            missing_source = any(gap["reason"] == "compatible_template_missing" for gap in candidate["gaps"])
            reason = collection_failures.get(candidate["configuration_id"], state.get("reason"))
            if candidate["status"] == "needs_data" and missing_source and reason is not None:
                collection_gap = {"kind": "collection", "reason": reason}
                if collection_gap not in candidate["gaps"]:
                    candidate["gaps"].append(collection_gap)
        relative = Path("results/prediction.json")
        write_json(directory / relative, result)
        state["prediction"]["result"] = str(relative)

        write_json(directory / "run.json", state)

        # The immutable asset references are now in the published result and the
        # ledger points to it. No later stage reads this round's full result.
        RunDirectory(directory, state["id"]).consume_work("prediction", state["prediction"]["round"])

    def _finish(
        self, directory: Path, state: dict[str, Any], result: dict[str, Any], reason: str | None = None
    ) -> None:
        statuses = {candidate["status"] for candidate in result["combinations"]}
        state["status"] = "complete"
        for status in ("failed", "needs_data", "unsupported"):
            if status in statuses:
                state["status"] = status
                break
        # Incomplete predictions remain open to asset/budget repair. Independent
        # validation starts only after every prediction has been fixed.
        validate = state["validation"] is not None and state["status"] == "complete"
        state["task_status"] = "validating" if validate else "finished"
        if validate:
            state["status"] = "needs_data"
            state["performance_plan"] = None
        if reason is not None:
            state["reason"] = reason
        else:
            state.pop("reason", None)

        self._publish(directory, state, result)
        self._cleanup(directory, state)
        if validate:
            self._validate(directory, state)
        else:
            self._summarize(directory, state)

    def _validate(self, directory: Path, state: dict[str, Any]) -> None:
        state["status"] = "needs_data"
        state.pop("reason", None)
        write_json(directory / "run.json", state)
        compared, finished = ValidationWorkflow(self.collection).run(directory, state)
        observations = state["validation"]["observations"]
        if compared:
            state["status"] = "complete"
            state.pop("reason", None)
        else:
            state["status"] = "failed" if any(item["status"] == "failed" for item in observations) else "needs_data"
            state["reason"] = "independent_validation_incomplete"
        state["task_status"] = "finished" if finished else "validating"
        write_json(directory / "run.json", state)
        if finished:
            self._cleanup(directory, state)
        self._summarize(directory, state)

    def _summarize(self, directory: Path, state: dict[str, Any]) -> None:
        state["summary"] = RunSummary(directory, state).write()
        write_json(directory / "run.json", state)

    def _cleanup(self, directory: Path, state: dict[str, Any]) -> None:
        # All processing handoffs have completed. Catalogs and diagnostics are
        # outside work/. Resume repeats cleanup if the host stopped after the
        # terminal ledger publication, without repeating the actual processing.
        if state["status"] == "complete":
            state["performance_plan"] = None
        write_json(directory / "run.json", state)
        if state["performance_plan"] is None:
            (directory / "results/sampling-plan.json").unlink(missing_ok=True)
        run = RunDirectory(directory, state["id"])
        run.consume_work("prediction", state["prediction"]["round"])
        for imported in state["performance_imports"].values():
            if imported["cost_models"] is not None:
                run.consume_work("fitting", imported["ordinal"])

        planning = directory / "work/planning"
        if planning.exists():
            shutil.rmtree(planning)
        (directory / "work/prepared_input.json").unlink(missing_ok=True)
        work = directory / "work"
        if work.exists():
            for path in reversed(sorted(work.rglob("*"))):
                if path.is_dir() and not any(path.iterdir()):
                    path.rmdir()
            if not any(work.iterdir()):
                work.rmdir()

    def _collect_representative(self, directory: Path, state: dict[str, Any]) -> None:
        active = state["active_representative"]
        try:
            self.collection.collect(directory, state)
        except CaptureProcessingFailed:
            # The attempt retains the processor's error and captured inputs.
            # Isolate this group; never turn a host interruption into a failure.
            pass

        if state["capture"]["manifest"] is None:
            state["representatives"][active] = state["capture"].get("reason", "representative_collection_incomplete")
        else:
            project_path = directory / "inputs/project.json"
            project = read_json(project_path)
            manifest = self.environment.container_path(Path(state["capture"]["manifest"]))
            # Publishing the input can precede the state write on interruption.
            if not any(asset["manifest"] == manifest for asset in project["assets"]["templates"]):
                project["assets"]["templates"].append({"manifest": manifest, "node_durations": None})
                write_json(project_path, project)

        # Failed collection changes only the group's outcome, not simulation
        # inputs. Persist the reusable-result phase across a host interruption.
        phase = "planned" if state["capture"]["manifest"] is not None else "selecting"
        state.update(active_representative=None, task_status=phase, status="needs_data")
        state.pop("reason", None)
        write_json(directory / "run.json", state)

    def _select_performance(
        self, directory: Path, state: dict[str, Any], result: dict[str, Any]
    ) -> tuple[bool, str | None]:
        # Publish the complete result before planning. An interrupted host resumes
        # this selection, rather than running another prediction for the same inputs.
        state["task_status"] = "selecting"
        self._publish(directory, state, result)
        round_name = state["prediction"]["round"]
        recorded = state["performance_plan"]
        if recorded is None or recorded["round"] != round_name:
            plan = self.collection.prepare(
                directory / "inputs/project.json",
                "performance_plan",
                "transfer",
                prediction_result=directory / state["prediction"]["result"],
            )
            # One current plan explains unresolved sampling decisions. Retain it
            # for incomplete runs; a complete prediction no longer consumes it.
            relative = Path("results/sampling-plan.json")
            write_json(directory / relative, plan)
            state["performance_plan"] = {"round": round_name, "path": str(relative)}
            write_json(directory / "run.json", state)
        else:
            plan = read_json(directory / recorded["path"])

        permission = state["collection"]["performance"]
        ready = False
        for index, group in enumerate(plan["groups"]):
            if group["status"] != "ready":
                continue
            ready = True
            key = group["key"]
            if key in state["performance_sampling"]:
                continue
            if not permission["allow_run"]:
                return False, "performance_collection_disabled"
            if permission["consumed_wall_time_ns"] >= permission["max_wall_time_sec"] * 1_000_000_000:
                return False, "performance_collection_budget_exhausted"

            requests = Path("work/planning") / f"{round_name:03d}" / f"requests-{index}.json"
            write_json(directory / requests, group["measurement_requests"])
            sampling = {
                "configuration_id": group["configuration_id"],
                "measurement_kind": group["measurement_kind"],
                "status": "selected",
                "manifest": None,
            }
            try:
                prepared = self.collection.prepare(
                    directory / "inputs/project.json",
                    "performance",
                    group["measurement_kind"],
                    directory / requests,
                    configuration_id=group["configuration_id"],
                    workload_id=group["workload_id"],
                )
            except subprocess.CalledProcessError as error:
                # The planner supplied a finite group. A rejected native recipe
                # ends this group without consuming or retrying a capture.
                sampling.update(status="failed", reason=str(error))
                state["performance_sampling"][key] = sampling
                write_json(directory / "run.json", state)
                continue

            prepared["measurement_kind"] = group["measurement_kind"]
            if group["measurement_kind"] == "cpu":
                prepared["cpu_services"] = group["cpu_services"]
                prepared["cpu_service_descriptors"] = group["cpu_service_descriptors"]
            prepared["repetitions"] = 3
            self.collection.select(prepared, directory, state)
            state["performance_sampling"][key] = sampling
            state.update(
                active_performance=key,
                task_status="planned",
            )
            self._publish(directory, state, result)
            return True, None

        return False, "performance_sampling_no_new_evidence" if ready else None

    def _collect_performance(self, directory: Path, state: dict[str, Any]) -> None:
        key = state["active_performance"]
        sampling = state["performance_sampling"][key]
        sampling["status"] = "selected"
        sampling.pop("reason", None)
        try:
            self.collection.collect(directory, state)
        except CaptureProcessingFailed:
            pass

        manifest = state["capture"]["manifest"]
        models: list[str] = []
        if manifest is None:
            sampling.update(status="failed", reason=state["capture"].get("reason", "performance_collection_incomplete"))
            if sampling["reason"] == "performance_collection_budget_exhausted":
                # Keep this group's owner for explicit resume. The failed native
                # attempt and its consumed quota remain in the collection ledger.
                self._finish(directory, state, read_json(directory / state["prediction"]["result"]), sampling["reason"])
                return
        else:
            manifest = self.environment.container_path(Path(manifest))
            # The active handoff remains durable while fitting. A resumed host
            # consumes the same isolated job and never recaptures this group.
            sampling["manifest"] = manifest
            write_json(directory / "run.json", state)
            try:
                models = self._performance_import(directory, state, manifest)
            except ModelingJobFailed as error:
                # Only a proven terminal fit failure ends this group. Host
                # interruption and infrastructure errors keep the active handoff.
                sampling.update(status="failed", reason=str(error))
            else:
                sampling["status"] = "captured" if models else "no_candidates"
                project_path = directory / "inputs/project.json"
                project = read_json(project_path)
                if manifest not in project["assets"]["performance_manifests"]:
                    project["assets"]["performance_manifests"].append(manifest)
                    write_json(project_path, project)

        state.update(active_performance=None, task_status="planned" if models else "selecting", status="needs_data")
        state.pop("reason", None)
        write_json(directory / "run.json", state)

    def _continue(self, directory: Path, state: dict[str, Any]) -> None:
        if state["active_representative"] is not None:
            self._collect_representative(directory, state)
        if state["active_performance"] is not None:
            self._collect_performance(directory, state)
            if state["task_status"] == "finished":
                return

        if state["task_status"] == "selecting":
            result = read_json(directory / state["prediction"]["result"])
        else:
            result = self._simulate(directory, state)

        while True:
            missing = {
                combination["configuration_id"]
                for combination in result["combinations"]
                if combination["status"] == "needs_data"
                and any(gap["reason"] == "compatible_template_missing" for gap in combination["gaps"])
            }
            permission = state["collection"]["base"]
            source_group = None
            source_reason = None
            if missing:
                # Bad declared assets need repair; they never authorize a
                # replacement capture. Healthy combinations can still advance.
                if any(asset["status"] != "loaded" for asset in result["assets"]):
                    source_reason = "template_asset_repair_required"
                elif not permission["allow_run"]:
                    source_reason = "base_collection_disabled"
                elif permission["consumed_wall_time_ns"] >= permission["max_wall_time_sec"] * 1_000_000_000:
                    source_reason = "base_collection_budget_exhausted"
                else:
                    # Source groups keep priority and get one attempt each.
                    # Rule and cost gaps do not request another structure capture.
                    for group in result["representative_collection"].get("groups", []):
                        representative = group["representative_configuration_id"]
                        if (
                            not missing.intersection(group["configuration_ids"])
                            or representative in state["representatives"]
                        ):
                            continue
                        if group["candidate_recipe"]["status"] == "not_ready":
                            state["representatives"][representative] = group["candidate_recipe"]["reason"]
                            continue
                        source_group = group
                        break
                    if source_group is None:
                        source_reason = "representative_source_unavailable"

            if source_group is None:
                # Publish structural reasons on their combinations before any
                # performance handoff.
                if source_reason is not None:
                    state["reason"] = source_reason
                selected, reason = self._select_performance(directory, state, result)
                if not selected:
                    self._finish(directory, state, result, source_reason or reason)
                    return
                self._collect_performance(directory, state)
                if state["task_status"] == "finished":
                    return
                if state["task_status"] != "selecting":
                    result = self._simulate(directory, state)
                continue

            representative = source_group["representative_configuration_id"]
            prepared = self.collection.prepare(
                directory / "inputs/project.json",
                "base",
                "transfer",
                configuration_id=representative,
                representative=True,
            )
            self.collection.select(prepared, directory, state)
            state["representatives"][representative] = None
            state.update(active_representative=representative, task_status="planned")
            self._publish(directory, state, result)

            self._collect_representative(directory, state)
            if state["task_status"] != "selecting":
                result = self._simulate(directory, state)


def main() -> None:
    parser = argparse.ArgumentParser(description="Run one isolated prediction processing round")
    parser.add_argument("--project", type=Path, required=True)
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--catalog-directory", type=Path, required=True)
    arguments = parser.parse_args()

    PredictionProcessing(arguments.project, arguments.directory, arguments.catalog_directory).run()


if __name__ == "__main__":
    main()
