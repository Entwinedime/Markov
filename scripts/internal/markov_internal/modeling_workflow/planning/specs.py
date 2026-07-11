"""Planning and deduplication of semantic C++ model-run requests."""

from __future__ import annotations

import hashlib
from dataclasses import dataclass
from typing import TYPE_CHECKING

from ...common.naming import safe_slug
from ..artifacts import WorkflowArtifactLayout
from ..types import CacheStatePredictionRef, ModelOutputRequirement, ModelRunSpec, ProfileRunRef

if TYPE_CHECKING:
    from ..validations.registry import ValidationRequest
    from ..context import WorkflowContext


@dataclass(frozen=True)
class ModelRunRequest:
    """Semantic C++ execution requested by one validation object."""

    mode: str
    source_profile: ProfileRunRef
    target_profile: ProfileRunRef | None
    output_requirements: frozenset[ModelOutputRequirement]
    validation_name: str
    prediction: CacheStatePredictionRef | None = None


@dataclass(frozen=True)
class ModelRunPlanner:
    """Merge validation requests into a deterministic C++ execution plan."""

    context: WorkflowContext
    artifacts: WorkflowArtifactLayout
    preflight_report: dict[str, object]

    def build(self, validations: list[ValidationRequest]) -> list[ModelRunSpec]:
        """Build the final plan after applying preflight skip decisions."""

        requests = self._collect_requests(validations)
        skip_policy = PreflightSkipPolicy.from_report(self.preflight_report)
        specs = [self._spec_from_group(group, skip_policy) for group in self._merge_requests(requests).values()]
        return sorted(specs, key=lambda spec: spec.run_id)

    def _collect_requests(self, validations: list[ValidationRequest]) -> list[ModelRunRequest]:
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

    def _spec_from_group(
        self,
        group_requests: list[ModelRunRequest],
        skip_policy: PreflightSkipPolicy,
    ) -> ModelRunSpec:
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
            skip_reason=skip_policy.reason_for(first),
            trace_threads=self.context.options.trace_threads,
            trace_file_threads=self.context.options.trace_file_threads,
            trace_channels=model_run_trace_channels(first),
            page_key_mode=self.context.options.page_key_mode,
        )


@dataclass(frozen=True)
class PreflightSkipPolicy:
    """Translate preflight evidence into stable per-cell skip reasons."""

    full_dag_by_run: dict[str, dict[str, object]]
    hicache_by_run: dict[str, dict[str, object]]

    @classmethod
    def from_report(cls, report: dict[str, object]) -> PreflightSkipPolicy:
        """Index shared preflight detail rows used by deterministic skip decisions."""

        checks = report.get("checks") if isinstance(report.get("checks"), dict) else {}
        full_dag = (
            checks.get("full_dag_trace_channels") if isinstance(checks.get("full_dag_trace_channels"), dict) else {}
        )
        hicache = checks.get("hicache_state_inputs") if isinstance(checks.get("hicache_state_inputs"), dict) else {}
        return cls(
            full_dag_by_run=index_rows(full_dag.get("rows")),
            hicache_by_run=index_rows(hicache.get("runs")),
        )

    def reason_for(self, request: ModelRunRequest) -> str:
        """Return an empty string or a stable machine-readable skip reason."""

        if request.mode == "faithful_replay":
            return self._faithful_replay_reason(request)
        if request.mode == "cache_state":
            return self._hicache_prediction_reason(request.prediction)
        return ""

    def _faithful_replay_reason(self, request: ModelRunRequest) -> str:
        row = self.full_dag_by_run.get(request.source_profile.run_id)
        if not isinstance(row, dict):
            return "preflight_missing:full_dag_trace_channels"
        if row.get("full_trace_ready") is not True:
            errors = row.get("artifact_errors") if isinstance(row.get("artifact_errors"), list) else []
            if errors:
                return "full_dag_trace_not_ready:" + ",".join(str(error) for error in errors)
            return "full_dag_trace_not_ready"
        return ""

    def _hicache_prediction_reason(self, prediction: CacheStatePredictionRef | None) -> str:
        if prediction is None:
            return ""
        source_preflight = self.hicache_by_run.get(prediction.source.run_id)
        target_preflight = self.hicache_by_run.get(prediction.target.run_id)
        readiness_reason = _hicache_readiness_reason(source_preflight, target_preflight)
        if readiness_reason or prediction.is_self:
            return readiness_reason
        return _cross_prediction_reason(source_preflight, target_preflight)


def _hicache_readiness_reason(
    source_preflight: dict[str, object] | None,
    target_preflight: dict[str, object] | None,
) -> str:
    """Return source or target readiness blockers before scope-specific checks."""

    reason = ""
    if isinstance(source_preflight, dict) and source_preflight.get("workflow_input_ready") is not True:
        reason = "source_workflow_input_not_ready"
    elif isinstance(target_preflight, dict) and target_preflight.get("workflow_input_ready") is not True:
        reason = "target_workflow_input_not_ready"
    return reason


def _cross_prediction_reason(
    source_preflight: dict[str, object] | None,
    target_preflight: dict[str, object] | None,
) -> str:
    """Require cross-config workload and forced-token identity equivalence."""

    if not isinstance(source_preflight, dict):
        return "source_preflight_missing"
    if not isinstance(target_preflight, dict):
        return "target_preflight_missing"
    if source_preflight.get("canonical_workload_signature") != target_preflight.get("canonical_workload_signature"):
        return "workload_signature_mismatch"
    if not forced_token_plan_matches(source_preflight, target_preflight):
        return "forced_token_plan_signature_mismatch"
    if not forced_token_bundle_matches(source_preflight, target_preflight):
        return "forced_token_bundle_signature_mismatch"
    return ""


def cache_prediction_key(prediction: CacheStatePredictionRef | None) -> str:
    """Return the semantic merge key for a cache-state prediction."""

    if prediction is None:
        return ""
    return f"{prediction.input_id}:{prediction.source.config_id}->{prediction.target.config_id}"


def index_rows(value: object) -> dict[str, dict[str, object]]:
    """Index well-formed preflight detail rows by run identifier."""

    if not isinstance(value, list):
        return {}
    return {str(row.get("run_id")): row for row in value if isinstance(row, dict) and row.get("run_id") is not None}


def model_run_trace_channels(request: ModelRunRequest) -> tuple[str, ...]:
    """Select manifest trace channels that the C++ backend may consume."""

    if request.mode == "cache_state":
        return ("python_probe",)
    return ()


def forced_token_plan_matches(source: dict[str, object], target: dict[str, object]) -> bool:
    """Return whether a cross-config pair shares one ready forced-token plan."""

    return matching_preflight_identity(
        source,
        target,
        ready_fields=("forced_token_enabled", "forced_token_plan_ready"),
        identity_fields=("forced_token_plan_sha256",),
    )


def forced_token_bundle_matches(source: dict[str, object], target: dict[str, object]) -> bool:
    """Return whether a cross-config pair shares one ready token bundle."""

    return matching_preflight_identity(
        source,
        target,
        ready_fields=("forced_token_bundle_ready",),
        identity_fields=("forced_token_bundle_sha256", "forced_token_bundle_id"),
    )


def matching_preflight_identity(
    source: dict[str, object],
    target: dict[str, object],
    *,
    ready_fields: tuple[str, ...],
    identity_fields: tuple[str, ...],
) -> bool:
    """Require both contracts to be ready with equal non-empty identities."""

    if any(source.get(field) is not True or target.get(field) is not True for field in ready_fields):
        return False
    return all(bool(source.get(field)) and source.get(field) == target.get(field) for field in identity_fields)


def model_run_id(
    mode: str,
    source: ProfileRunRef,
    target: ProfileRunRef | None,
    prediction: CacheStatePredictionRef | None,
    requirements: frozenset[ModelOutputRequirement],
) -> str:
    """Build a readable model-run identifier from semantic inputs."""

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
    """Build the source-to-target configuration component of a path."""

    return f"{safe_slug(prediction.source.config_id)}_to_{safe_slug(prediction.target.config_id)}"
