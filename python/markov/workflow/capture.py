"""Prepare one inference capture and delegate device lifetime to the shared task owner."""

from __future__ import annotations

from dataclasses import asdict
from pathlib import Path

from markov.artifacts.capture import DeviceCommand
from markov.artifacts.request import PredictionRequest, read_json
from markov.artifacts.workload import Workload
from markov.workflow.device_task import DeviceTask


class InferenceCapture:
    def __init__(
        self,
        request: PredictionRequest,
        profiling: bool = True,
        calibration: bool = False,
        workload: Workload | None = None,
    ):
        self.request = request
        self.settings = request.execution.capture
        self.profiling = profiling
        self.calibration = calibration
        self.workload = workload
        self.result_key = "manifest" if profiling else "calibration" if calibration else "observation"
        self.result_file = (
            "profile_manifest.json"
            if profiling
            else "region_measurements.json"
            if calibration
            else "normal_observation.json"
        )
        self.directory = request.execution.output / (
            "base" if profiling else "calibration" if calibration else "normal"
        )

    def run(self, recapture: bool = False, reuse: bool = False) -> dict:
        if self.request.base.manifest is not None:
            if not self.profiling:
                raise ValueError("Normal measurement requires base.configuration; a trace is not a normal observation")
            if recapture:
                raise ValueError("Recapture needs base.configuration instead of an existing manifest")
            self.request.base.manifest.stat()
            return {"status": "reused", "manifest": str(self.request.base.manifest)}
        if self.settings is None:
            raise ValueError("Inference capture needs execution.capture with a runtime image, devices and budget")
        if (self.request.deployment.framework, self.request.deployment.runtime) != ("sglang", "ascend"):
            raise ValueError("Inference capture currently supports SGLang/Ascend; other adapters are not restored")
        if len(self.settings.devices) != self.request.deployment.tensor_parallel:
            raise ValueError("Single-host capture needs one declared device per tensor-parallel rank")

        from markov.adapters.sglang import capture as adapter

        workload = self.workload if self.workload is not None else Workload.load(self.request.workload)
        server = adapter.server_arguments(self.request.deployment, self.request.base.configuration)
        task = DeviceTask(self.settings, self.directory)
        task.recover()
        existing = task.state.get(self.result_key)
        if existing is not None and not recapture:
            if not reuse:
                raise ValueError(f"This task already completed at {existing}; request --recapture to repeat")
            captured = read_json(Path(existing).parent / "capture_input.json")
            if (
                captured["deployment"] != asdict(self.request.deployment)
                or captured["server"] != server
                or Workload.parse(captured["workload"]) != workload
            ):
                raise ValueError(
                    "This output belongs to different capture inputs; use a new output or explicit recapture"
                )
            Path(existing).stat()
            return {"status": "reused", self.result_key: existing, "capture_state": str(task.state_path)}

        model = Path(self.request.deployment.model)
        if not model.is_absolute() or not model.is_dir():
            raise ValueError("Inference capture needs deployment.model to name an absolute local model directory")
        payload = {
            "deployment": asdict(self.request.deployment),
            "server": server,
            "environment": adapter.capture_environment(self.profiling, self.calibration),
            "profiling": self.profiling,
            "calibration": self.calibration,
            "workload": asdict(workload),
        }
        command = DeviceCommand(
            adapter.__name__,
            ("collect", "--input", "/capture/capture_input.json"),
            payload,
            self.result_file,
            self.result_key,
            ((model, "/model", False),),
            payload["environment"],
            len(workload.requests),
            sum(len(row.input_ids) for row in workload.requests)
            if all(row.input_ids is not None for row in workload.requests)
            else None,
            sum(row.max_new_tokens for row in workload.requests),
            "workload_report.json",
        )
        return task.run(command)
