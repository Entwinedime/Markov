"""Publish Ascend profiler exports and worker journals from one attempt."""

import json
from pathlib import Path
from typing import Any

from markov_collection.artifacts.json_file import write_json


class AscendCaptureArtifacts:
    def __init__(self, prepared: dict[str, Any], directory: Path) -> None:
        self.prepared = prepared
        self.directory = directory
        # Read before workers start: PID reuse must not import prior log bytes.
        self.log_offsets = {
            path: path.stat().st_size for path in (Path.home() / "ascend/log/run/plog").glob("plog-*.log")
        }

    def publish(self) -> None:
        traces = []
        # This is the producer enumerating its own completed vendor exports. Consumers
        # receive explicit references and never search a directory to select an input.
        for trace in sorted((self.directory / "native_profile").glob("*/ASCEND_PROFILER_OUTPUT/trace_view.json")):
            profiler_directory = trace.parent.parent
            profiler_info = next(profiler_directory.glob("profiler_info_*.json"))
            (clock_start_info,) = profiler_directory.glob("PROF*/host/start_info")
            with profiler_info.open(encoding="utf-8") as stream:
                rank = json.load(stream)["rank_id"]

            traces.append(
                {
                    "trace": str(trace.relative_to(self.directory)),
                    "kernel_details": str((trace.parent / "kernel_details.csv").relative_to(self.directory)),
                    "profiler_info": str(profiler_info.relative_to(self.directory)),
                    "clock_start_info": str(clock_start_info.relative_to(self.directory)),
                    "device_info": [
                        str(path.relative_to(self.directory))
                        for path in sorted(profiler_directory.glob("PROF*/device_*/info.json.*"))
                        if path.suffix[1:].isdecimal()
                    ],
                    "database": str((trace.parent / f"ascend_pytorch_profiler_{rank}.db").relative_to(self.directory)),
                }
            )

        # Retain only bytes appended during this attempt by observed workers.
        # PID reuse must not import an older communicator initialization.
        communication_logs = []
        log_directory = self.directory / "communication_logs"
        log_directory.mkdir(exist_ok=True)
        for observation in sorted((self.directory / "observations").glob("*.jsonl")):
            with observation.open(encoding="utf-8") as stream:
                first = json.loads(next(stream))
            pid, rank = first["pid"], first["rank"]
            for log in sorted((Path.home() / "ascend/log/run/plog").glob(f"plog-{pid}_*.log")):
                with log.open("rb") as stream:
                    stream.seek(self.log_offsets.get(log, 0))
                    content = stream.read()
                if not content:
                    continue

                destination = log_directory / log.name
                destination.write_bytes(content)
                communication_logs.append(
                    {"rank": rank, "pid": pid, "path": str(destination.relative_to(self.directory))}
                )

        capture = {
            "role": self.prepared["role"],
            "framework": self.prepared["framework"],
            "input": "prepared_input.json",
            "environment": "native_environment.json",
            "responses": "responses.json" if self.prepared["role"] != "performance" else "measurement_completions.json",
            "observations": [
                str(path.relative_to(self.directory))
                for path in sorted((self.directory / "observations").glob("*.jsonl"))
            ],
            "native_api": [
                str(path.relative_to(self.directory))
                for path in sorted((self.directory / "native_api").glob("*.jsonl"))
            ],
            "event_history": [
                str(path.relative_to(self.directory))
                for path in sorted((self.directory / "native_api" / "initial_events").glob("*.jsonl"))
            ],
            "profile_controls": [
                str(path.relative_to(self.directory))
                for path in sorted((self.directory / "profile_controls").glob("*.json"))
            ],
            "workspace_states": [
                str(path.relative_to(self.directory))
                for path in sorted((self.directory / "workspace_states").glob("*.json"))
            ],
            "traces": traces,
            "communication_logs": communication_logs,
        }
        if self.prepared["role"] == "validation":
            capture["candidate_id"] = self.prepared["candidate_id"]

        if self.prepared["framework"] == "ktransformers":
            capture["native_requests"] = "native_requests.json"

        write_json(self.directory / "native_capture.json", capture)
