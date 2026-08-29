"""Adapt semantic model-run requirements to a self-contained runner config."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Any

from ...common.paths import ROOT_DIR, repo_relative_path, running_in_modeling_container
from ...modeling.workload import discover_workload_window
from ..io_model import HiCacheIoModel
from ..io_model_projection import merge_hicache_io_model
from ..types import ModelRunSpec, TargetHiCacheConfig


@dataclass(frozen=True)
class RunnerConfigBuilder:
    """Build the complete configuration for one containerized model run."""

    spec: ModelRunSpec

    def payload(self) -> dict[str, Any]:
        """Build the self-contained config consumed by the container runner."""

        payload: dict[str, Any] = {
            "metadata": {
                "model_run_id": self.spec.run_id,
            },
            "input": {
                "profile_manifest": path_text(self.spec.source_profile.manifest_path),
            },
            "output_dir": path_text(self.spec.output_dir),
            "mode": "cache_state",
            "cpp_trace_graph": {
                "backend_kind": "validation",
                "threads": self.spec.trace_threads,
                "file_threads": self.spec.trace_file_threads,
                "trace_channels": list(self.spec.trace_channels),
            },
            "outputs": {"emit_module_summary": True},
        }
        window = discover_workload_window({}, self.spec.source_profile.manifest_path)
        if window is not None:
            payload["cpp_trace_graph"]["trace_window_start_us"] = window.start_ns // 1000
            payload["cpp_trace_graph"]["trace_window_end_us"] = window.end_ns // 1000
            payload["cpp_trace_graph"]["actual_e2e_us"] = window.actual_e2e_ns // 1000
            payload["metadata"]["trace_window"] = {
                "source": window.source,
                "report_path": path_text(window.report_path),
                "start_ns": window.start_ns,
                "end_ns": window.end_ns,
                "actual_e2e_ns": window.actual_e2e_ns,
            }
        payload.update(
            hicache_run_payload(
                self.spec.target_config,
                source_target_same_config=self.spec.target_config.matches_source(self.spec.source_profile),
                io_model=self.spec.hicache_io_model,
            )
        )
        return payload

    def command(self, runner_config_path: Path) -> list[str]:
        """Run a cell directly in-container or enter the container once."""

        if running_in_modeling_container():
            return [
                "python3",
                "scripts/internal/entrypoints/model.py",
                "--config",
                str(runner_config_path),
            ]
        return [str(ROOT_DIR / "scripts/model.sh"), "build-dag", "--config", str(runner_config_path)]


def hicache_run_payload(
    target: TargetHiCacheConfig,
    *,
    source_target_same_config: bool,
    io_model: HiCacheIoModel | None,
) -> dict[str, Any]:
    """Build the narrow C++ HiCache config."""

    hicache_config = merge_hicache_io_model({"enabled": True, **target.fields}, io_model)
    dag_patch = hicache_config.get("dag_patch") if isinstance(hicache_config.get("dag_patch"), dict) else {}
    hicache_config["dag_patch"] = {
        **dag_patch,
        "enabled": True,
        "source_target_same_config": source_target_same_config,
    }
    return {"cpp_model_config": {"hicache": hicache_config}}


def path_text(path: Path) -> str:
    """Serialize a repository path in a checkout-independent form."""

    return str(repo_relative_path(path))
