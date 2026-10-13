"""Persisted prediction workflow and its isolated, container-owned CPU processing."""

from __future__ import annotations

import argparse
import fcntl
import json
import subprocess
from datetime import datetime
from pathlib import Path
from typing import Any
from uuid import uuid4

from markov_collection.artifacts.json_file import write_json
from markov_collection.workflow.collection import CaptureProcessingFailed, CollectionWorkflow


class PredictionProcessing:
    def __init__(self, project: Path, directory: Path) -> None:
        self.project = project
        self.directory = directory

    def run(self) -> dict[str, Any]:
        # This lock belongs to the container, so a stopped host cannot start a
        # second writer for the same round. Published results belong to this round.
        with (self.directory / "processing.lock").open("a", encoding="utf-8") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)

            try:
                with (self.directory / "result.json").open(encoding="utf-8") as stream:
                    return json.load(stream)
            except FileNotFoundError:
                pass

            root = Path(__file__).resolve().parents[4]
            result = subprocess.run(
                [str(root / "build/markov/markov"), "predict", "--project", str(self.project)],
                check=True,
                capture_output=True,
                text=True,
            )

            prediction = json.loads(result.stdout)
            write_json(self.directory / "result.json", prediction)
            return prediction


class PredictionWorkflow:
    def __init__(self, collection: CollectionWorkflow) -> None:
        self.collection = collection

    def predict(self, project: Path) -> dict[str, Any]:
        bundle = self.collection._prepare(project, "prediction", "cpu")
        inputs = bundle["project"]
        # Docker names are global even when projects use different output directories.
        run_name = f"{datetime.now():run-%Y%m%d-%H%M%S-%f}-{uuid4().hex}"
        directory = self.collection._host_path(inputs["output"]["directory"]) / run_name

        input_directory = directory / "input"
        input_directory.mkdir(parents=True)
        for filename, configuration in bundle["configurations"].items():
            write_json(input_directory / filename, configuration)
        for filename, workload in bundle["request_workloads"].items():
            write_json(input_directory / filename, workload)
        write_json(input_directory / "project.json", inputs)

        state: dict[str, Any] = {
            "project": str(project),
            "task_status": "planned",
            "status": "needs_data",
            "collection": {
                role: {"consumed_wall_time_ns": 0, **permission} for role, permission in inputs["collection"].items()
            },
            "attempts": [],
            "prediction": {"rounds": [], "result": None, "recompute_configurations": None},
            "representatives": {},
            "active_representative": None,
        }

        write_json(directory / "run.json", state)
        return self.resume(directory)

    def _result(self, directory: Path, state: dict[str, Any]) -> dict[str, Any]:
        result: dict[str, Any] = {}
        if state["prediction"]["result"] is not None:
            with (directory / state["prediction"]["result"]).open(encoding="utf-8") as stream:
                result = json.load(stream)

        live, active_ns = self.collection._reconcile(state)
        if result:
            result["collection_performed"] = bool(state["attempts"])
            result["collection_usage"] = {
                role: {"consumed_wall_time_ns": permission["consumed_wall_time_ns"]}
                for role, permission in state["collection"].items()
            }

        prediction_container = self._prediction_container(directory, state)
        prediction_live = prediction_container is not None and prediction_container["Running"]

        return {
            **result,
            **state,
            "run": str(directory),
            "live_attempt": live,
            "active_wall_time_ns": active_ns,
            "prediction_live": prediction_live,
        }

    def _container_name(self, directory: Path, round_directory: Path) -> str:
        return f"markov-prediction-{directory.name}-{round_directory.name}"

    def _prediction_container(self, directory: Path, state: dict[str, Any]) -> dict[str, Any] | None:
        if not state["prediction"]["rounds"]:
            return None

        round_directory = directory / state["prediction"]["rounds"][-1]
        return self.collection._container_state(self._container_name(directory, round_directory))

    def status(self, directory: Path) -> dict[str, Any]:
        with (directory / "run.json").open(encoding="utf-8") as stream:
            state = json.load(stream)
        return self._result(directory, state)

    def resume(
        self,
        directory: Path,
        *,
        budgets: dict[str, int] | None = None,
        template_assets: Path | None = None,
        performance_manifests: list[Path] | None = None,
        cost_models: list[Path] | None = None,
    ) -> dict[str, Any]:
        with (directory / "run.lock").open("a", encoding="utf-8") as lock:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                return {**self.status(directory), "workflow_busy": True}

            with (directory / "run.json").open(encoding="utf-8") as stream:
                state = json.load(stream)
            live, _ = self.collection._reconcile(state)
            write_json(directory / "run.json", state)
            prediction_container = self._prediction_container(directory, state)
            prediction_live = prediction_container is not None and prediction_container["Running"]
            revisions = (
                bool(budgets)
                or template_assets is not None
                or performance_manifests is not None
                or cost_models is not None
            )
            if live or prediction_live:
                result = self._result(directory, state)
                if revisions:
                    result["requested_changes_applied"] = False
                return result

            # Reconcile active work before changing the inputs used by a resumed round.
            if revisions:
                self._revise_assets(
                    directory, state, budgets or {}, template_assets, performance_manifests, cost_models
                )
            if state["task_status"] == "finished" and state["active_representative"] is None:
                return self._result(directory, state)

            try:
                self._continue(directory, state)
            except KeyboardInterrupt:
                # Host interruption does not stop the container. Preserve the
                # durable phase so resume consumes this round before starting another.
                state["reason"] = "prediction_workflow_interrupted"
                write_json(directory / "run.json", state)
            except Exception as error:
                state.update(status="failed", reason=str(error))
                write_json(directory / "run.json", state)
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
            if seconds <= 0 or seconds * 1_000_000_000 < state["collection"][role]["consumed_wall_time_ns"]:
                raise ValueError(f"{role} total budget must be positive and cover its consumed time")

        templates = None
        declared = [(path, "performance") for path in performance_manifests or []]
        if template_assets is not None:
            with template_assets.open(encoding="utf-8") as stream:
                templates = json.load(stream)
            if not isinstance(templates, list):
                raise ValueError("template assets must be an array")
            for asset in templates:
                if not isinstance(asset, dict) or asset.keys() - {"manifest", "node_durations"}:
                    raise ValueError("template assets require manifest and optional node_durations")
                manifest = (template_assets.parent / asset["manifest"]).resolve()
                declared.append((manifest, "base"))
                asset["manifest"] = self.collection._container_path(manifest)
                if asset.get("node_durations") is not None:
                    asset["node_durations"] = self.collection._container_path(
                        (template_assets.parent / asset["node_durations"]).resolve()
                    )
                else:
                    asset["node_durations"] = None
        for path, role in declared:
            with path.open(encoding="utf-8") as stream:
                if json.load(stream)["role"] != role:
                    raise ValueError(f"resume import requires role={role}: {path}")

        project_path = directory / "input/project.json"
        with project_path.open(encoding="utf-8") as stream:
            project = json.load(stream)
        for role, seconds in budgets.items():
            state["collection"][role].update(allow_run=True, max_wall_time_sec=seconds)
            project["collection"][role] = {"allow_run": True, "max_wall_time_sec": seconds}
        if templates is not None:
            project["assets"]["templates"] = templates
        if performance_manifests is not None:
            project["assets"]["performance_manifests"] = [
                self.collection._container_path(path) for path in performance_manifests
            ]

        if cost_models is not None:
            project["assets"]["cost_models"] = [self.collection._container_path(path) for path in cost_models]

        write_json(project_path, project)
        # Explicit asset replacement can affect any source selection. Only the
        # automatic representative path has a producer-qualified group scope.
        state["prediction"]["recompute_configurations"] = None
        state.update(task_status="planned", status="needs_data")
        state.pop("reason", None)

        write_json(directory / "run.json", state)

    def _simulate(self, directory: Path, state: dict[str, Any]) -> dict[str, Any]:
        if state["task_status"] == "predicting":
            round_directory = directory / state["prediction"]["rounds"][-1]
        else:
            relative = Path("prediction") / f"round-{len(state['prediction']['rounds']) + 1}"
            round_directory = directory / relative
            # A host may stop after creating the directory and before saving the
            # round identity; no worker has been launched at this point.
            round_directory.mkdir(parents=True, exist_ok=True)
            with (directory / "input/project.json").open(encoding="utf-8") as stream:
                inputs = json.load(stream)
            affected = state["prediction"]["recompute_configurations"]
            if affected is not None:
                inputs["configurations"] = [item for item in inputs["configurations"] if item["id"] in affected]
            for workload in inputs["request_workloads"]:
                workload["path"] = self.collection._container_path(directory / "input" / workload["path"])
            # Graph diagnostics belong to this round's writable mount and survive resume.
            inputs["output"]["directory"] = self.collection._container_path(round_directory)
            for candidate in inputs["configurations"]:
                candidate["config"] = self.collection._container_path(directory / "input" / candidate["config"])
            write_json(round_directory / "project.json", inputs)
            state["prediction"]["rounds"].append(str(relative))
            state["task_status"] = "predicting"
            write_json(directory / "run.json", state)

        project = round_directory / "project.json"
        with project.open(encoding="utf-8") as stream:
            inputs = json.load(stream)

        command = [
            "docker",
            "run",
            "--detach",
            "--name",
            self._container_name(directory, round_directory),
            "--pull=never",
            "--network=none",
            "--read-only",
            "--cap-drop=ALL",
            "-e",
            "PYTHONDONTWRITEBYTECODE=1",
            "-e",
            "PYTHONPATH=/workspace/trace-sim/src/collection",
        ]

        # Expose only declared prediction inputs; the full repository is not mounted.
        mounts = {
            self.collection.root / "build/markov/markov",
            self.collection.root / "src/collection",
            directory / "input",
        }

        # Fitted models contain all query values. Raw sample references are
        # provenance and are not opened by the prediction consumer.
        mounts.update(self.collection._host_path(path) for path in inputs["assets"]["cost_models"])
        declared = [(path, "performance") for path in inputs["assets"]["performance_manifests"]]
        for asset in inputs["assets"]["templates"]:
            declared.append((asset["manifest"], "base"))
            if asset["node_durations"] is not None:
                mounts.add(self.collection._host_path(asset["node_durations"]))
        for path, role in declared:
            manifest = self.collection._host_path(path)
            with manifest.open(encoding="utf-8") as stream:
                metadata = json.load(stream)
            if metadata["role"] != role:
                raise ValueError(f"prediction import requires role={role}: {manifest}")
            mounts.add(manifest)
            if role == "base":
                asset = metadata.get("compiled_templates")
                if asset is not None and (manifest.parent / asset).is_file():
                    mounts.add((manifest.parent / asset).resolve())

        for path in sorted(mounts):
            command += ["--mount", f"type=bind,src={path},dst={self.collection._container_path(path)},readonly"]
        command += [
            "--mount",
            f"type=bind,src={round_directory},dst={self.collection._container_path(round_directory)}",
            self.collection._service_image("modeling"),
            "python3",
            "-m",
            "markov_collection.workflow.prediction",
            "--project",
            self.collection._container_path(project),
            "--directory",
            self.collection._container_path(round_directory),
        ]

        container_name = self._container_name(directory, round_directory)
        with (round_directory / "processing_launch.log").open("a", encoding="utf-8") as log:
            if self.collection._container_state(container_name) is None:
                subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)
            subprocess.run(["docker", "wait", container_name], check=True, stdout=log, stderr=subprocess.STDOUT)
            subprocess.run(["docker", "logs", container_name], check=True, stdout=log, stderr=subprocess.STDOUT)
            container = self.collection._container_state(container_name)
            if container["ExitCode"] != 0:
                raise RuntimeError(f"prediction container exited {container['ExitCode']}; see {round_directory}")

        with (round_directory / "result.json").open(encoding="utf-8") as stream:
            result = json.load(stream)
        if state["prediction"]["recompute_configurations"] is None:
            return result

        # Keep the raw round limited to its actual inputs. The published task
        # result combines replacements with immutable prior combination reports.
        with (directory / state["prediction"]["result"]).open(encoding="utf-8") as stream:
            previous = json.load(stream)
        replacements = {
            (item["configuration_id"], item["request_workload_id"]): item for item in result["combinations"]
        }
        result["combinations"] = [
            replacements.get((item["configuration_id"], item["request_workload_id"]), item)
            for item in previous["combinations"]
        ]
        coverage = {item["configuration_id"]: item for item in result["representative_collection"]["source_coverage"]}
        plan = previous["representative_collection"]
        plan["source_coverage"] = [coverage.get(item["configuration_id"], item) for item in plan["source_coverage"]]
        result["representative_collection"] = plan
        return result

    def _publish(self, directory: Path, state: dict[str, Any], result: dict[str, Any]) -> None:
        result["collection_performed"] = bool(state["attempts"])
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
        relative = Path(state["prediction"]["rounds"][-1]) / "prediction.json"
        write_json(directory / relative, result)
        state["prediction"]["result"] = str(relative)
        state["prediction"]["recompute_configurations"] = None

        write_json(directory / "run.json", state)

    def _finish(
        self, directory: Path, state: dict[str, Any], result: dict[str, Any], reason: str | None = None
    ) -> None:
        statuses = [candidate["status"] for candidate in result["combinations"]]
        state["status"] = "complete" if all(status == "complete" for status in statuses) else "unsupported"
        if "needs_data" in statuses:
            state["status"] = "needs_data"
        if "failed" in statuses:
            state["status"] = "failed"
        state["task_status"] = "finished"
        if reason is not None:
            state["reason"] = reason
        else:
            state.pop("reason", None)

        self._publish(directory, state, result)

    def _collect_representative(self, directory: Path, state: dict[str, Any]) -> None:
        active = state["active_representative"]
        try:
            if len(state["attempts"]) == active["attempt_index"]:
                with (directory / "prepared_input.json").open(encoding="utf-8") as stream:
                    prepared = json.load(stream)
                self.collection._collect(prepared, directory, state)
            elif state["attempts"][-1]["status"] == "completed" and state["base_manifest"] is None:
                # Resume processing the recorded attempt, never launch a replacement.
                state["status"] = "captured"
                self.collection._finalize(directory, state)
        except CaptureProcessingFailed:
            # The attempt retains the processor's error and captured inputs.
            # Isolate this group; never turn a host interruption into a failure.
            pass

        if state["base_manifest"] is None:
            state["representatives"][active["configuration_id"]] = state.get(
                "reason", "representative_collection_incomplete"
            )
        else:
            project_path = directory / "input/project.json"
            with project_path.open(encoding="utf-8") as stream:
                project = json.load(stream)
            manifest = self.collection._container_path(Path(state["base_manifest"]))
            # Publishing the input can precede the state write on interruption.
            if not any(asset["manifest"] == manifest for asset in project["assets"]["templates"]):
                project["assets"]["templates"].append({"manifest": manifest, "node_durations": None})
                write_json(project_path, project)
            state["prediction"]["recompute_configurations"] = active["configuration_ids"]

        # Failed collection changes only the group's outcome, not simulation
        # inputs. Persist the reusable-result phase across a host interruption.
        phase = "planned" if state["base_manifest"] is not None else "selecting"
        state.update(active_representative=None, task_status=phase, status="needs_data")
        state.pop("reason", None)
        write_json(directory / "run.json", state)

    def _continue(self, directory: Path, state: dict[str, Any]) -> None:
        if state["active_representative"] is not None:
            self._collect_representative(directory, state)

        if state["task_status"] == "selecting":
            with (directory / state["prediction"]["result"]).open(encoding="utf-8") as stream:
                result = json.load(stream)
        else:
            result = self._simulate(directory, state)

        while True:
            missing = {
                combination["configuration_id"]
                for combination in result["combinations"]
                if combination["status"] == "needs_data"
                and any(gap["reason"] == "compatible_template_missing" for gap in combination["gaps"])
            }
            if not missing:
                self._finish(directory, state, result)
                return

            # A declared but unreadable/uncompiled asset needs repair, not a
            # replacement native run that conceals the import failure.
            if any(asset["status"] != "loaded" for asset in result["assets"]):
                self._finish(directory, state, result, "template_asset_repair_required")
                return

            permission = state["collection"]["base"]
            if not permission["allow_run"]:
                self._finish(directory, state, result, "base_collection_disabled")
                return
            if permission["consumed_wall_time_ns"] >= permission["max_wall_time_sec"] * 1_000_000_000:
                self._finish(directory, state, result, "base_collection_budget_exhausted")
                return

            # Only absent compatible sources need a candidate. Rule and cost gaps
            # do not request another capture; every declared group gets one try.
            selected = None
            for group in result["representative_collection"].get("groups", []):
                representative = group["representative_configuration_id"]
                if not missing.intersection(group["configuration_ids"]) or representative in state["representatives"]:
                    continue
                if group["candidate_recipe"]["status"] == "not_ready":
                    state["representatives"][representative] = group["candidate_recipe"]["reason"]
                    continue
                selected = group
                break
            if selected is None:
                self._finish(directory, state, result, "representative_source_unavailable")
                return

            representative = selected["representative_configuration_id"]
            prepared = self.collection._prepare(
                directory / "input/project.json",
                "base",
                "cpu",
                configuration_id=representative,
                representative=True,
            )
            write_json(directory / "prepared_input.json", prepared)
            state["representatives"][representative] = None
            state.update(
                role="base",
                base_manifest=None,
                active_representative={
                    "configuration_id": representative,
                    "configuration_ids": selected["configuration_ids"],
                    "attempt_index": len(state["attempts"]),
                },
                task_status="planned",
            )
            self._publish(directory, state, result)

            self._collect_representative(directory, state)
            if state["task_status"] != "selecting":
                result = self._simulate(directory, state)


def main() -> None:
    parser = argparse.ArgumentParser(description="Run one isolated prediction processing round")
    parser.add_argument("--project", type=Path, required=True)
    parser.add_argument("--directory", type=Path, required=True)
    arguments = parser.parse_args()

    PredictionProcessing(arguments.project, arguments.directory).run()


if __name__ == "__main__":
    main()
