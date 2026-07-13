"""HiCache fact contract helpers."""

from __future__ import annotations

HICACHE_CONSUMER_STATE_MODEL = "hicache_state_model"
HICACHE_CONSUMER_INPUT_CONTRACT = "hicache_input_contract"
HICACHE_CONSUMER_FINAL_STATE_VALIDATOR = "hicache_final_state_validator"
HICACHE_CONSUMER_TRANSITION_VALIDATOR = "hicache_transition_validator"
HICACHE_CONSUMER_DAG_PATCH = "hicache_dag_patch"

HICACHE_FACT_CONSUMERS = frozenset(
    {
        HICACHE_CONSUMER_STATE_MODEL,
        HICACHE_CONSUMER_INPUT_CONTRACT,
        HICACHE_CONSUMER_FINAL_STATE_VALIDATOR,
        HICACHE_CONSUMER_TRANSITION_VALIDATOR,
        HICACHE_CONSUMER_DAG_PATCH,
    }
)

HICACHE_FACT_CLASSES = frozenset(
    {
        "workload_identity",
        "source_actual",
        "timing_observation",
        "oracle_state",
    }
)

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
        }
    ),
    "oracle_state": frozenset({"state_snapshot"}),
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
            HICACHE_CONSUMER_TRANSITION_VALIDATOR,
            HICACHE_CONSUMER_DAG_PATCH,
        }
    ),
    "timing_observation": frozenset(
        {
            HICACHE_CONSUMER_TRANSITION_VALIDATOR,
            HICACHE_CONSUMER_DAG_PATCH,
        }
    ),
    "oracle_state": frozenset(
        {
            HICACHE_CONSUMER_FINAL_STATE_VALIDATOR,
            HICACHE_CONSUMER_TRANSITION_VALIDATOR,
        }
    ),
}


def allowed_consumers_for_fact(fact_class: str, role: str) -> frozenset[str]:
    """Return the consumers allowed to read a HiCache fact."""

    roles = HICACHE_FACT_ROLES_BY_CLASS.get(fact_class)
    if roles is None or role not in roles:
        return frozenset()
    return HICACHE_CONSUMERS_BY_CLASS.get(fact_class, frozenset())


def validate_hicache_fact(fact_class: str, role: str, consumers: list[str] | tuple[str, ...]) -> None:
    """Validate a HiCache fact class/role/consumer tuple."""

    if fact_class not in HICACHE_FACT_CLASSES:
        raise ValueError(f"unknown HiCache fact class: {fact_class}")
    roles = HICACHE_FACT_ROLES_BY_CLASS[fact_class]
    if role not in roles:
        raise ValueError(f"role {role!r} is not allowed for HiCache fact class {fact_class!r}")
    if not consumers:
        raise ValueError(f"HiCache fact {fact_class}/{role} must declare at least one consumer")
    allowed = allowed_consumers_for_fact(fact_class, role)
    seen: set[str] = set()
    for consumer in consumers:
        if consumer not in HICACHE_FACT_CONSUMERS:
            raise ValueError(f"unknown HiCache fact consumer: {consumer}")
        if consumer in seen:
            raise ValueError(f"duplicate HiCache fact consumer: {consumer}")
        seen.add(consumer)
        if consumer not in allowed:
            raise ValueError(f"consumer {consumer!r} is not allowed for HiCache fact {fact_class}/{role}")
