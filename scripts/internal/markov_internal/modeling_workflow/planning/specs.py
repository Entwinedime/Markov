"""模型运行请求规划。"""

from __future__ import annotations

import hashlib
from dataclasses import dataclass
from typing import TYPE_CHECKING

from ...common.naming import safe_slug
from ..artifacts import WorkflowArtifactLayout
from ..types import ModelRunSpec, ModelOutputRequirement, CacheStatePredictionRef, ProfileRunRef

if TYPE_CHECKING:
    from ..validations.registry import ValidationRequest
    from ..context import WorkflowContext


@dataclass(frozen=True)
class ModelRunRequest:
    """单个 validation 提出的 C++ 运行请求。"""

    mode: str
    source_profile: ProfileRunRef
    target_profile: ProfileRunRef | None
    output_requirements: frozenset[ModelOutputRequirement]
    validation_name: str
    prediction: CacheStatePredictionRef | None = None

    @property
    def merge_key(self) -> tuple[str, str, str, str, tuple[str, ...]]:
        """返回可合并到同一个 C++ 运行的语义 key。"""

        target_config = self.target_profile.config_id if self.target_profile is not None else ""
        target_run = self.target_profile.run_id if self.target_profile is not None else ""
        return (
            self.mode,
            self.source_profile.run_id,
            target_config,
            target_run,
            tuple(sorted(requirement.value for requirement in self.output_requirements)),
        )


@dataclass(frozen=True)
class ModelRunPlanner:
    """把 validation request 合并成稳定的 C++ 执行计划。"""

    context: "WorkflowContext"
    artifacts: WorkflowArtifactLayout
    preflight_report: dict[str, object]

    def build(self, validations: list["ValidationRequest"]) -> list[ModelRunSpec]:
        """构造最终模型运行计划。"""

        requests = self._collect_requests(validations)
        specs = [self._spec_from_group(group) for group in self._merge_requests(requests).values()]
        return sorted(specs, key=lambda spec: spec.run_id)

    def _collect_requests(self, validations: list["ValidationRequest"]) -> list[ModelRunRequest]:
        requests: list[ModelRunRequest] = []
        for validation in validations:
            requests.extend(validation.build_model_run_requests(self.context))
        return requests

    def _merge_requests(
        self, requests: list[ModelRunRequest]
    ) -> dict[tuple[str, str, str, str], list[ModelRunRequest]]:
        merged: dict[tuple[str, str, str, str], list[ModelRunRequest]] = {}
        for request in requests:
            key = (
                request.mode,
                request.source_profile.run_id,
                request.target_profile.run_id if request.target_profile is not None else "",
                cache_prediction_key(request.prediction),
            )
            merged.setdefault(key, []).append(request)
        return merged

    def _spec_from_group(self, group_requests: list[ModelRunRequest]) -> ModelRunSpec:
        first = group_requests[0]
        requirements = frozenset(
            requirement for request in group_requests for requirement in request.output_requirements
        )
        validations_for_run = tuple(sorted({request.validation_name for request in group_requests}))
        run_id = model_run_id(first.mode, first.source_profile, first.target_profile, first.prediction, requirements)
        return ModelRunSpec(
            run_id=run_id,
            mode=first.mode,
            source_profile=first.source_profile,
            target_profile=first.target_profile,
            output_requirements=requirements,
            validation_requests=validations_for_run,
            output_dir=self.artifacts.model_run_dir(run_id),
            prediction=first.prediction,
            skip_reason=PreflightSkipPolicy(self.preflight_report).reason_for(first),
            trace_threads=self.context.options.trace_threads,
            trace_file_threads=self.context.options.trace_file_threads,
            trace_channels=model_run_trace_channels(first),
        )


@dataclass(frozen=True)
class PreflightSkipPolicy:
    """根据 preflight 结果决定某个模型运行是否应跳过。"""

    preflight_report: dict[str, object]

    def reason_for(self, request: ModelRunRequest) -> str:
        """返回空字符串或稳定 skip reason。"""

        checks = self.preflight_report.get("checks") if isinstance(self.preflight_report.get("checks"), dict) else {}
        if request.mode == "faithful_replay":
            return self._faithful_replay_reason(request, checks)
        if request.mode == "cache_state":
            hicache = checks.get("hicache_state_inputs") if isinstance(checks.get("hicache_state_inputs"), dict) else {}
            return self._hicache_prediction_reason(request.prediction, hicache)
        return ""

    def _faithful_replay_reason(self, request: ModelRunRequest, checks: dict[str, object]) -> str:
        full_dag = (
            checks.get("full_dag_trace_channels") if isinstance(checks.get("full_dag_trace_channels"), dict) else {}
        )
        rows = full_dag.get("rows") if isinstance(full_dag.get("rows"), list) else []
        row_by_run = {str(row.get("run_id")): row for row in rows if isinstance(row, dict)}
        row = row_by_run.get(request.source_profile.run_id)
        if not isinstance(row, dict):
            return "preflight_missing:full_dag_trace_channels"
        if row.get("full_trace_ready") is not True:
            errors = row.get("artifact_errors") if isinstance(row.get("artifact_errors"), list) else []
            if errors:
                return "full_dag_trace_not_ready:" + ",".join(str(error) for error in errors)
            return "full_dag_trace_not_ready"
        return ""

    def _hicache_prediction_reason(self, prediction: CacheStatePredictionRef | None, check: dict[str, object]) -> str:
        if prediction is None:
            return ""
        rows = check.get("runs") if isinstance(check.get("runs"), list) else []
        row_by_run = {str(row.get("run_id")): row for row in rows if isinstance(row, dict)}
        source_preflight = row_by_run.get(prediction.source.run_id)
        target_preflight = row_by_run.get(prediction.target.run_id)
        if isinstance(source_preflight, dict) and source_preflight.get("workflow_input_ready") is not True:
            return "source_workflow_input_not_ready"
        if isinstance(target_preflight, dict) and target_preflight.get("workflow_input_ready") is not True:
            return "target_workflow_input_not_ready"
        if prediction.is_self:
            return ""

        if not isinstance(source_preflight, dict):
            return "source_preflight_missing"
        if not isinstance(target_preflight, dict):
            return "target_preflight_missing"
        if source_preflight.get("canonical_workload_signature") != target_preflight.get(
            "canonical_workload_signature"
        ):
            return "workload_signature_mismatch"
        if not forced_token_plan_matches(source_preflight, target_preflight):
            return "forced_token_plan_signature_mismatch"
        if not forced_token_bundle_matches(source_preflight, target_preflight):
            return "forced_token_bundle_signature_mismatch"
        return ""


def cache_prediction_key(prediction: CacheStatePredictionRef | None) -> str:
    """返回 cache-state prediction 的合并 key。"""

    if prediction is None:
        return ""
    return f"{prediction.input_id}:{prediction.source.config_id}->{prediction.target.config_id}"


def model_run_trace_channels(request: ModelRunRequest) -> tuple[str, ...]:
    """返回一次 C++ run 应实际消费的 manifest trace channel。"""

    if request.mode == "cache_state":
        return ("python_probe",)
    return ()


def forced_token_plan_matches(source: dict[str, object], target: dict[str, object]) -> bool:
    """判断一对 cross-config prediction 是否使用相同 forced-token plan。"""

    if source.get("forced_token_enabled") is not True or target.get("forced_token_enabled") is not True:
        return False
    if source.get("forced_token_plan_ready") is not True or target.get("forced_token_plan_ready") is not True:
        return False
    source_plan = source.get("forced_token_plan_sha256")
    target_plan = target.get("forced_token_plan_sha256")
    return bool(source_plan) and source_plan == target_plan


def forced_token_bundle_matches(source: dict[str, object], target: dict[str, object]) -> bool:
    """判断一对 cross-config prediction 是否使用相同 forced-token bundle。"""

    if source.get("forced_token_bundle_ready") is not True or target.get("forced_token_bundle_ready") is not True:
        return False
    source_bundle = source.get("forced_token_bundle_sha256")
    target_bundle = target.get("forced_token_bundle_sha256")
    source_bundle_id = source.get("forced_token_bundle_id")
    target_bundle_id = target.get("forced_token_bundle_id")
    return (
        bool(source_bundle)
        and source_bundle == target_bundle
        and bool(source_bundle_id)
        and source_bundle_id == target_bundle_id
    )


def model_run_id(
    mode: str,
    source: ProfileRunRef,
    target: ProfileRunRef | None,
    prediction: CacheStatePredictionRef | None,
    requirements: frozenset[ModelOutputRequirement],
) -> str:
    """根据语义输入构造可读的 model run id。"""

    req_text = ",".join(sorted(requirement.value for requirement in requirements))
    req_digest = hashlib.sha1(req_text.encode("utf-8")).hexdigest()[:8]
    if prediction is not None:
        return "__".join(
            [safe_slug(mode), safe_slug(prediction.input_id), prediction_config_slug(prediction), req_digest]
        )
    parts = [safe_slug(mode), safe_slug(source.input_id), safe_slug(source.config_id), req_digest]
    if target is not None:
        parts.insert(3, f"to_{safe_slug(target.config_id)}")
    return "__".join(parts)


def prediction_config_slug(prediction: CacheStatePredictionRef) -> str:
    """构造 source->target config 的路径片段。"""

    return f"{safe_slug(prediction.source.config_id)}_to_{safe_slug(prediction.target.config_id)}"
