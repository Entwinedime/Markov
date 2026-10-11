"""Python probe target and HiCache fact contracts, without runtime installation."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

HICACHE_CONSUMER_STATE_MODEL = "hicache_state_model"
HICACHE_CONSUMER_INPUT_CONTRACT = "hicache_input_contract"
HICACHE_CONSUMER_DAG_PATCH = "hicache_dag_patch"

HICACHE_FACT_CONSUMERS = frozenset(
    {
        HICACHE_CONSUMER_STATE_MODEL,
        HICACHE_CONSUMER_INPUT_CONTRACT,
        HICACHE_CONSUMER_DAG_PATCH,
    }
)


def runtime_probe_names(consumers: tuple[str, ...], diagnostics: str) -> tuple[str, ...]:
    """DAG consumers need causal observations even without optional diagnostics."""
    if HICACHE_CONSUMER_DAG_PATCH in consumers or diagnostics == "timing":
        return (
            "runtime_preparation",
            "response_boundaries",
            "cpu_collectives",
            "layer_waits",
            "decode_allocation",
            "attention_backend",
            "dense_backend",
        )
    return ()


HICACHE_FACT_ROLES_BY_CLASS = {
    "workload_identity": frozenset(
        {
            "cache_extend_input",
            "cache_lifecycle_commit",
            "cache_lookup_input",
            "prefetch_candidate_anchor",
        }
    ),
    "source_actual": frozenset(
        {
            "capacity_request",
            "capacity_result_observed",
            "insert_result_observed",
            "loadback_decision_observed",
            "commit_device_to_host_enqueue_observed",
            "commit_capacity_release_observed",
            "lock_scope_delta",
            "lock_scope_result_observed",
            "prefetch_decision_observed",
            "prefetch_intent_observed",
            "prefetch_progress_observed",
            "request_admission_observed",
            "writeback_enqueue_observed",
        }
    ),
    "timing_observation": frozenset(
        {
            "prefetch_io_observed",
            "writeback_io_observed",
            "loadback_io_observed",
            "commit_device_to_host_io_observed",
            "storage_read_service_observed",
            "storage_write_service_observed",
        }
    ),
}

HICACHE_CONSUMERS_BY_CLASS = {
    "workload_identity": frozenset(
        {
            HICACHE_CONSUMER_STATE_MODEL,
            HICACHE_CONSUMER_INPUT_CONTRACT,
        }
    ),
    "source_actual": frozenset(
        {
            HICACHE_CONSUMER_DAG_PATCH,
        }
    ),
    "timing_observation": frozenset(
        {
            HICACHE_CONSUMER_DAG_PATCH,
        }
    ),
}


def validate_hicache_fact(fact_class: str, role: str, consumers: list[str] | tuple[str, ...]) -> None:
    """Validate a HiCache fact class/role/consumer tuple."""

    if fact_class not in HICACHE_FACT_ROLES_BY_CLASS:
        raise ValueError(f"unknown HiCache fact class: {fact_class}")
    roles = HICACHE_FACT_ROLES_BY_CLASS[fact_class]
    if role not in roles:
        raise ValueError(f"role {role!r} is not allowed for HiCache fact class {fact_class!r}")
    if not consumers:
        raise ValueError(f"HiCache fact {fact_class}/{role} must declare at least one consumer")
    allowed = HICACHE_CONSUMERS_BY_CLASS[fact_class]
    seen: set[str] = set()
    for consumer in consumers:
        if consumer not in HICACHE_FACT_CONSUMERS:
            raise ValueError(f"unknown HiCache fact consumer: {consumer}")
        if consumer in seen:
            raise ValueError(f"duplicate HiCache fact consumer: {consumer}")
        seen.add(consumer)
        if consumer not in allowed:
            raise ValueError(f"consumer {consumer!r} is not allowed for HiCache fact {fact_class}/{role}")


@dataclass(frozen=True)
class FieldSpec:
    """A named source expression; missing required fields remain explicit."""

    name: str
    source: str = ""
    required: bool = True


@dataclass(frozen=True)
class FactSpec:
    """Fact classification and the consumers selected for this capture."""

    fact_class: str
    role: str
    consumers: tuple[str, ...]


@dataclass(frozen=True)
class TargetSpec:
    """Read-only installation input shared by catalog selection and the probe."""

    id: str
    module_name: str
    target: str
    events: dict[str, str]
    fields: tuple[FieldSpec, ...]
    fact: FactSpec
    emit_when: tuple[str, ...] = ()
    capture_thread_timing: bool = False
    capture_emission_timing: bool = False


def parse_target(raw: dict[str, Any], *, requested_consumers: tuple[str, ...] | None = None) -> TargetSpec:
    """Parse external target JSON without importing a framework or writing events.

    Catalog selection supplies normalized requested consumers and intersects
    them with the fact's allowed consumers; an empty intersection is filtered
    by the caller. Runtime environment input must explicitly declare consumers.
    Diagnostic policies must already have been resolved to booleans. Inputs
    and the resulting dictionaries are treated as read-only after parsing.
    """

    target_id = raw.get("id")
    target = raw.get("target")
    module_name = raw.get("module")
    if not isinstance(target_id, str) or not target_id:
        raise ValueError("python_probe target id must be a non-empty string")
    if not isinstance(target, str) or not target:
        raise ValueError(f"python_probe target {target_id!r} target must be a non-empty string")
    if not isinstance(module_name, str) or not module_name:
        raise ValueError(f"python_probe target {target_id!r} module must be a non-empty string")

    raw_fields = raw.get("fields")
    if not isinstance(raw_fields, list):
        raise ValueError(f"python_probe target {target_id!r} fields must be an array")
    fields = tuple(_parse_field(item, target_id, index) for index, item in enumerate(raw_fields))
    events = _parse_events(raw.get("events"), target_id)
    emit_when = _parse_emit_conditions(raw.get("emit_when"), target_id)
    fact = _parse_fact(raw.get("fact"), target_id, requested_consumers)

    capture_thread_timing = raw.get("capture_thread_timing", False)
    if not isinstance(capture_thread_timing, bool):
        raise ValueError(f"python_probe target {target_id!r} capture_thread_timing must be a boolean")
    capture_emission_timing = raw.get("capture_emission_timing", False)
    if not isinstance(capture_emission_timing, bool):
        raise ValueError(f"python_probe target {target_id!r} capture_emission_timing must be a boolean")

    return TargetSpec(
        id=target_id,
        module_name=module_name,
        target=target,
        events=events,
        fields=fields,
        fact=fact,
        emit_when=emit_when,
        capture_thread_timing=capture_thread_timing,
        capture_emission_timing=capture_emission_timing,
    )


def _parse_field(raw: Any, target_id: str, index: int) -> FieldSpec:
    if not isinstance(raw, dict):
        raise ValueError(f"python_probe target {target_id!r} fields[{index}] must be an object")
    name = raw.get("name")
    if not isinstance(name, str) or not name:
        raise ValueError(f"python_probe target {target_id!r} fields[{index}].name must be a non-empty string")
    source = raw.get("source", "")
    if source is not None and not isinstance(source, str):
        raise ValueError(f"python_probe target {target_id!r} fields[{index}].source must be a string")
    required = raw.get("required", True)
    if not isinstance(required, bool):
        raise ValueError(f"python_probe target {target_id!r} fields[{index}].required must be a boolean")
    return FieldSpec(name=name, source=source or "", required=required)


def _parse_fact(raw: Any, target_id: str, requested_consumers: tuple[str, ...] | None) -> FactSpec:
    if not isinstance(raw, dict):
        raise ValueError(f"python_probe target {target_id!r} must define fact")
    fact_class = raw.get("class")
    role = raw.get("role")
    if not isinstance(fact_class, str) or not fact_class:
        raise ValueError(f"python_probe target {target_id!r} fact.class must be a non-empty string")
    if not isinstance(role, str) or not role:
        raise ValueError(f"python_probe target {target_id!r} fact.role must be a non-empty string")

    if requested_consumers is None:
        consumers = raw.get("consumers")
    else:
        consumers = list(HICACHE_CONSUMERS_BY_CLASS.get(fact_class, ()))
    if not isinstance(consumers, list) or not all(isinstance(item, str) and item for item in consumers):
        raise ValueError(f"python_probe target {target_id!r} fact.consumers must be a non-empty string array")
    validate_hicache_fact(fact_class, role, consumers)

    selected = consumers
    if requested_consumers is not None:
        selected = [consumer for consumer in requested_consumers if consumer in consumers]
    return FactSpec(fact_class=fact_class, role=role, consumers=tuple(selected))


def _parse_events(raw: Any, target_id: str) -> dict[str, str]:
    if not isinstance(raw, dict):
        raise ValueError(f"python_probe target {target_id!r} events must be a phase-to-event-name object")
    events: dict[str, str] = {}
    for phase, event_name in raw.items():
        if phase not in {"start", "end"}:
            raise ValueError(f"python_probe target {target_id!r} events contains unsupported phase {phase!r}")
        if not isinstance(event_name, str) or not event_name:
            raise ValueError(f"python_probe target {target_id!r} event name for phase {phase!r} must be a non-empty string")
        events[phase] = event_name
    if not events:
        raise ValueError(f"python_probe target {target_id!r} events must not be empty")
    return events


def _parse_emit_conditions(raw: Any, target_id: str) -> tuple[str, ...]:
    if raw is None:
        return ()
    if not isinstance(raw, list):
        raise ValueError(f"python_probe target {target_id!r} emit_when must be an array")

    sources: list[str] = []
    for index, condition in enumerate(raw):
        if not isinstance(condition, dict):
            raise ValueError(f"python_probe target {target_id!r} emit_when[{index}] must be an object")
        source = condition.get("source")
        if not isinstance(source, str) or not source:
            raise ValueError(f"python_probe target {target_id!r} emit_when[{index}].source must be a non-empty string")
        if condition.get("op", "present") != "present":
            raise ValueError(f"python_probe target {target_id!r} emit_when[{index}].op must be 'present'")
        sources.append(source)
    return tuple(sources)
