"""Publish Ascend profiler exports and worker journals from one attempt."""

import gzip
import json
import shutil
import time
from pathlib import Path
from typing import Any

from markov_collection.artifacts.json_file import read_json, write_json


class AscendCaptureArtifacts:
    def __init__(self, prepared: dict[str, Any], directory: Path) -> None:
        from torch_npu.utils._path_manager import PathManager

        # Use the installed runtime's path contract before loading model workers.
        PathManager.check_input_directory_path(str(directory / "native_profile"))
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
            capture["observation_purpose"] = self.prepared["observation_purpose"]

        self._compress_profile_exports(capture)
        write_json(self.directory / "native_capture.json", capture)

    def _compress_profile_exports(self, capture: dict[str, Any]) -> None:
        started_ns = time.perf_counter_ns()
        profile = self.directory / "native_profile"
        before = sum(path.stat().st_size for path in profile.rglob("*") if path.is_file())
        # A trace alone does not establish a complete export. Validate declared
        # files before any pruning so failed vendor analysis keeps its evidence.
        for trace in capture["traces"]:
            references = [
                trace[name] for name in ("trace", "kernel_details", "profiler_info", "clock_start_info", "database")
            ]
            for reference in references + trace["device_info"]:
                path = self.directory / reference
                if not path.is_file():
                    raise FileNotFoundError(f"incomplete profiler export: {path}")

        for trace in capture["traces"]:
            for name in ("trace", "kernel_details"):
                path = self.directory / trace[name]
                compressed = path.with_suffix(path.suffix + ".gz")
                # Preserve every exported event/row and its original index. Only
                # encoding changes; no scope filtering or cost aggregation occurs.
                with path.open("rb") as source, gzip.open(compressed, "wb", compresslevel=1) as destination:
                    shutil.copyfileobj(source, destination)
                trace[name] = str(compressed.relative_to(self.directory))

        capture["profile_retention"] = {
            "exported_bytes": before,
            "retained_bytes": sum(path.stat().st_size for path in profile.rglob("*") if path.is_file()),
            "encoding": "lossless gzip for trace and kernel details; other declared exports unchanged",
            "processing_wall_time_ns": time.perf_counter_ns() - started_ns,
            "pending_cleanup": bool(capture["traces"]),
        }

    @staticmethod
    def finish_profile_retention(capture_path: Path, capture: dict[str, Any]) -> None:
        retention = capture.get("profile_retention", {})
        if not retention.get("pending_cleanup", False):
            return

        started_ns = time.perf_counter_ns()
        directory = capture_path.parent
        profile = directory.resolve() / "native_profile"
        retained: set[Path] = set()
        completed_exports: set[Path] = set()
        for trace in capture["traces"]:
            path = (directory / trace["trace"]).resolve()
            # A derived replay may reference an older capture. Its successful
            # normalization never authorizes deleting that capture's raw files.
            if not path.is_relative_to(profile):
                continue

            completed_exports.add(path.parent.parent)
            references = [
                trace[name] for name in ("trace", "kernel_details", "profiler_info", "clock_start_info", "database")
            ]
            retained.update((directory / reference).resolve() for reference in references + trace["device_info"])
        if not completed_exports:
            return

        # Only a fully normalized capture reaches this operation.
        # Missing-rank exports and history without pending work remain intact.
        for completed in completed_exports:
            for path in completed.rglob("*"):
                if path.is_file() and path.resolve() not in retained:
                    path.unlink()

        retention["pending_cleanup"] = False
        retention["retained_bytes"] = sum(path.stat().st_size for path in profile.rglob("*") if path.is_file())
        retention["processing_wall_time_ns"] += time.perf_counter_ns() - started_ns
        write_json(capture_path, capture)

    @staticmethod
    def release_observation_exports(capture_path: Path) -> None:
        capture = read_json(capture_path)
        if "profile_retention" not in capture:
            return
        directory = capture_path.parent.resolve()
        prepared = read_json(directory / capture["input"])
        if prepared["diagnostics"]:
            return

        retention = capture["profile_retention"]
        profile = directory / "native_profile"
        journal_roots = {
            "observations": "observations",
            "native_api": "native_api",
            "event_history": "native_api",
            "profile_controls": "profile_controls",
            "workspace_states": "workspace_states",
            "communication_logs": "communication_logs",
        }
        if "release_directories" not in retention:
            owned = set()
            borrowed = []
            for trace in capture["traces"]:
                path = (directory / trace["trace"]).resolve()
                if path.is_relative_to(profile):
                    owned.add(str(path.parent.parent.relative_to(directory)))
                else:
                    borrowed.append(trace)

            released_journal_bytes = 0
            for field, name in journal_roots.items():
                root = directory / name
                if name in owned or not root.is_dir() or root.resolve() != root:
                    continue
                references = capture[field]
                if field == "communication_logs":
                    references = [item["path"] for item in references]
                if any((directory / reference).resolve().is_relative_to(root) for reference in references):
                    owned.add(name)
                    released_journal_bytes += sum(path.stat().st_size for path in root.rglob("*") if path.is_file())
            if not owned:
                return

            # Retries need only this finite list. Retire readable profiler
            # references before deletion; normalized facts and assets are already
            # published, and borrowed captures are never part of this cleanup.
            retention["release_directories"] = sorted(owned)
            retention["raw_exports"] = "released"
            retention["released_journal_bytes"] = released_journal_bytes
            # Journal paths remain immutable provenance identifiers for cost
            # sample separation. Their payloads now live in normalized facts;
            # raw replay is explicitly unavailable after this handoff.
            capture["traces"] = borrowed
            write_json(capture_path, capture)

        for reference in retention["release_directories"]:
            path = (directory / reference).resolve()
            if path.parent != profile and path not in {directory / name for name in journal_roots.values()}:
                raise ValueError(f"raw export cleanup does not own {path}")
            if path.exists():
                shutil.rmtree(path)
        if profile.exists() and not any(profile.iterdir()):
            profile.rmdir()

        retention.pop("release_directories")
        retention["retained_bytes"] = sum(path.stat().st_size for path in profile.rglob("*") if path.is_file())
        write_json(capture_path, capture)
