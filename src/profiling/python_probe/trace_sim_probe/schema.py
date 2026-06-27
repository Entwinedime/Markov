"""Python probe constants and HiCache fact contract helpers."""

from __future__ import annotations

HICACHE_CATEGORY = "hicache"
FRAMEWORK_SGLANG = "sglang"
HICACHE_RADIX_SCHEMA = "hicache_radix"

HICACHE_RADIX_OP = "HiCache::radix_op"
HICACHE_STORAGE_OP = "HiCache::storage_op"
HICACHE_CACHE_OPERATION = "HiCache::cache_operation"
HICACHE_DEBUG_EVENT = "HiCache::debug"

HICACHE_CONSUMER_STATE_MODEL = "hicache_state_model"
HICACHE_CONSUMER_PROFILE_QUALITY = "hicache_profile_quality"
HICACHE_CONSUMER_INPUT_CONTRACT = "hicache_input_contract"
HICACHE_CONSUMER_FINAL_STATE_VALIDATOR = "hicache_final_state_validator"
HICACHE_CONSUMER_TRANSITION_VALIDATOR = "hicache_transition_validator"
HICACHE_CONSUMER_PROBE_DEBUG = "hicache_probe_debug"

HICACHE_FACT_CONSUMERS = frozenset(
    {
        HICACHE_CONSUMER_STATE_MODEL,
        HICACHE_CONSUMER_PROFILE_QUALITY,
        HICACHE_CONSUMER_INPUT_CONTRACT,
        HICACHE_CONSUMER_FINAL_STATE_VALIDATOR,
        HICACHE_CONSUMER_TRANSITION_VALIDATOR,
        HICACHE_CONSUMER_PROBE_DEBUG,
    }
)

HICACHE_FACT_CLASSES = frozenset(
    {
        "workload_identity",
        "target_policy_input",
        "runtime_model_checkpoint",
        "source_actual",
        "timing_observation",
        "oracle_state",
        "debug_quality",
    }
)

HICACHE_FACT_ROLES_BY_CLASS = {
    "workload_identity": frozenset(
        {
            "request_admission",
            "request_bound_match_anchor",
            "request_lifecycle_anchor",
        }
    ),
    "target_policy_input": frozenset({"prefetch_decision"}),
    "runtime_model_checkpoint": frozenset(
        {
            "prefetch_check_point",
            "storage_control_drain_boundary",
        }
    ),
    "source_actual": frozenset(
        {
            "all_blocks_cleared_observed",
            "cache_config_observed",
            "cache_stage_match_path_observed",
            "capacity_request",
            "capacity_result_observed",
            "evictable_state_observed",
            "host_eviction_observed",
            "host_mem_release_enqueue_observed",
            "host_ref_delta_observed",
            "insert_path",
            "insert_result_observed",
            "load_ack_checkpoint_observed",
            "load_back_request_observed",
            "load_back_result_observed",
            "load_enqueue_observed",
            "load_start_observed",
            "lock_scope_delta",
            "lock_scope_result_observed",
            "lookup_result_observed",
            "maintenance_checkpoint",
            "node_remove_observed",
            "node_store_observed",
            "prefetch_decision_observed",
            "prefetch_enqueue_observed",
            "prefetch_intent_observed",
            "prefetch_loaded_tokens_observed",
            "prefetch_progress_observed",
            "prefetch_rate_limit_observed",
            "prefetch_terminate_observed",
            "prefetch_terminate_requested_observed",
            "radix_node_mutation_observed",
            "request_abort_cleanup_observed",
            "request_admission_observed",
            "request_lifecycle_path_observed",
            "request_lifecycle_runtime_observed",
            "storage_hit_query_observed",
            "write_ack_checkpoint_observed",
            "write_counter_delta_observed",
            "write_enqueue_observed",
            "write_start_observed",
            "writeback_enqueue_observed",
            "writeback_schedule_observed",
            "writeback_storage_schedule_observed",
        }
    ),
    "timing_observation": frozenset(
        {
            "prefetch_io_observed",
            "writeback_io_observed",
        }
    ),
    "oracle_state": frozenset({"state_snapshot"}),
    "debug_quality": frozenset(
        {
            "forced_token_bundle",
            "forced_token_plan",
            "profiling_quality",
        }
    ),
}

_STATE_MODEL_CONSUMERS = frozenset(
    {
        HICACHE_CONSUMER_STATE_MODEL,
        HICACHE_CONSUMER_PROFILE_QUALITY,
    }
)

_STATE_MODEL_CONTROL_CONSUMERS = frozenset(
    {
        HICACHE_CONSUMER_STATE_MODEL,
        HICACHE_CONSUMER_PROFILE_QUALITY,
        HICACHE_CONSUMER_TRANSITION_VALIDATOR,
    }
)

HICACHE_CONSUMERS_BY_CLASS = {
    "workload_identity": frozenset(
        {
            HICACHE_CONSUMER_STATE_MODEL,
            HICACHE_CONSUMER_PROFILE_QUALITY,
            HICACHE_CONSUMER_INPUT_CONTRACT,
        }
    ),
    "target_policy_input": _STATE_MODEL_CONSUMERS,
    "runtime_model_checkpoint": _STATE_MODEL_CONTROL_CONSUMERS,
    "source_actual": frozenset(
        {
            HICACHE_CONSUMER_PROFILE_QUALITY,
            HICACHE_CONSUMER_TRANSITION_VALIDATOR,
        }
    ),
    "timing_observation": frozenset(
        {
            HICACHE_CONSUMER_PROFILE_QUALITY,
            HICACHE_CONSUMER_TRANSITION_VALIDATOR,
        }
    ),
    "oracle_state": frozenset(
        {
            HICACHE_CONSUMER_PROFILE_QUALITY,
            HICACHE_CONSUMER_FINAL_STATE_VALIDATOR,
            HICACHE_CONSUMER_TRANSITION_VALIDATOR,
        }
    ),
    "debug_quality": frozenset(
        {
            HICACHE_CONSUMER_PROFILE_QUALITY,
            HICACHE_CONSUMER_INPUT_CONTRACT,
            HICACHE_CONSUMER_PROBE_DEBUG,
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
