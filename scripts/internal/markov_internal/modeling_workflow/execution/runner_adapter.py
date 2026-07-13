"""Adapt semantic model-run requirements to a self-contained runner config."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Any

from ...common.paths import ROOT_DIR, repo_relative_path
from ..io_model import HiCacheIoModel, merge_hicache_io_model
from ..types import ModelOutputRequirement, ModelRunSpec, ProfileRunRef


@dataclass(frozen=True)
class RunnerOutputs:
    """Runner output capabilities derived once from semantic requirements."""

    validation: bool
    dag_analysis: bool

    @classmethod
    def from_spec(cls, spec: ModelRunSpec) -> RunnerOutputs:
        """Map semantic artifact requirements to runner output capabilities."""

        requirements = spec.output_requirements
        return cls(
            validation=ModelOutputRequirement.HICACHE_VALIDATION in requirements,
            dag_analysis=ModelOutputRequirement.DAG_ANALYSIS in requirements,
        )

    @property
    def validation_backend(self) -> bool:
        """Return whether the C++ invocation must use its Debug backend."""

        return self.validation or self.dag_analysis

    def as_config(self) -> dict[str, bool]:
        """Serialize only explicitly enabled output switches for the runner."""

        output: dict[str, bool] = {}
        if self.validation:
            output["emit_validation"] = True
            output["emit_module_summary"] = True
        if self.dag_analysis:
            output["emit_dag_analysis"] = True
        return output


@dataclass(frozen=True)
class RunnerConfigBuilder:
    """Build the complete configuration for one containerized model run."""

    spec: ModelRunSpec

    def payload(self) -> dict[str, Any]:
        """Build the self-contained config consumed by the container runner."""

        outputs = RunnerOutputs.from_spec(self.spec)
        payload: dict[str, Any] = {
            "schema": "trace_sim.modeling.runner_config.v1",
            "metadata": {
                "model_run_id": self.spec.run_id,
                "validation_requests": list(self.spec.validation_requests),
            },
            "input": {
                "profile_manifest": path_text(self.spec.source_profile.manifest_path),
            },
            "output_dir": path_text(self.spec.output_dir),
            "mode": self.spec.mode,
            "cpp_trace_graph": {
                "backend_kind": "validation" if outputs.validation_backend else "release",
                "threads": self.spec.trace_threads,
                "file_threads": self.spec.trace_file_threads,
                "trace_channels": list(self.spec.trace_channels),
            },
            "outputs": outputs.as_config(),
        }
        if self.spec.hicache_io_model is not None:
            payload["metadata"]["hicache_io_model"] = self.spec.hicache_io_model.metadata()
        if self.spec.mode == "cache_state":
            payload.update(
                hicache_run_payload(
                    require_target_profile(self.spec),
                    outputs,
                    dag_patch_enabled=ModelOutputRequirement.HICACHE_DAG_PATCH in self.spec.output_requirements,
                    page_key_mode=self.spec.page_key_mode,
                    io_model=self.spec.hicache_io_model,
                )
            )
        return payload

    def command(self, runner_config_path: Path) -> list[str]:
        """Return the single supported internal container-runner invocation."""

        return [str(ROOT_DIR / "scripts/model.sh"), "--config", str(runner_config_path)]


def require_target_profile(spec: ModelRunSpec) -> ProfileRunRef:
    """Return the target profile required by a cache-state run."""

    if spec.target_profile is None:
        raise ValueError(f"cache_state model run requires target profile: {spec.run_id}")
    if spec.target_profile.hicache_config is None:
        raise ValueError(f"target profile has no HiCache config: {spec.target_profile.label}")
    return spec.target_profile


def hicache_run_payload(
    target: ProfileRunRef,
    outputs: RunnerOutputs,
    *,
    dag_patch_enabled: bool,
    page_key_mode: str,
    io_model: HiCacheIoModel | None,
) -> dict[str, Any]:
    """Build the narrow C++ HiCache and Python oracle-validation config."""

    hicache_config = merge_hicache_io_model({"enabled": True, **(target.hicache_config or {})}, io_model)
    dag_patch = hicache_config.get("dag_patch") if isinstance(hicache_config.get("dag_patch"), dict) else {}
    hicache_config["dag_patch"] = {**dag_patch, "enabled": dag_patch_enabled}
    return {
        "cpp_model_config": {"hicache": hicache_config},
        "validation": {
            "hicache_state": {
                "enabled": outputs.validation,
                "require_oracle_state_trace": outputs.validation,
                "oracle_page_key_mode": page_key_mode,
                "oracle_trace_paths": [path_text(path) for path in target.python_probe_files],
            }
        },
    }


def path_text(path: Path) -> str:
    """Serialize a repository path in a checkout-independent form."""

    return str(repo_relative_path(path))
