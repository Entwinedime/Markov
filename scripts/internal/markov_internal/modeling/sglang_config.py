"""HiCache execution values and their SGLang launch representation."""

from __future__ import annotations

import json
from typing import Any

from markov_internal.modeling.source_dag.profiles import (
    apply_sglang_capacity_from_flags,
    sglang_prefetch_capacity_tokens,
)

TIMEOUT_FIELDS = (
    "prefetch_timeout_base_sec",
    "prefetch_timeout_per_ki_token_sec",
    "prefetch_timeout_max_sec",
)


def hicache_workload_spec(config_id: str, fields: dict[str, Any]) -> dict[str, Any]:
    """Translate a resolved target to the template workload's configuration contract."""

    page = fields["page_size"]
    return {
        "id": config_id,
        "resolved": {
            "page_size": page,
            "device_pages": fields["l1_capacity_pages"],
            "host_pages": fields["l2_capacity_pages"],
        },
        "policy": {
            "write_policy": fields["write_policy"],
            "prefetch_stop_policy": fields["prefetch_policy"],
            "prefetch_threshold": fields["prefetch_threshold_pages"] * page,
            "prefetch_capacity_limit_tokens": sglang_prefetch_capacity_tokens(
                fields["l2_capacity_pages"] * page, fields["l1_capacity_pages"] * page
            ),
            "timeout": {name.removeprefix("prefetch_timeout_"): fields[name] for name in TIMEOUT_FIELDS},
        },
    }


def hicache_server_flags(fields: dict[str, Any], extra: dict[str, Any] | None = None) -> dict[str, str]:
    """Return normalized value flags; unchanged server settings remain in the base.

    Page capacities are the search contract. Choose the middle of SGLang's host
    rounding bucket, then use the existing forward parser to resolve capacities.
    This inverse is a unit conversion, not a performance coefficient.
    """

    page_size = fields["page_size"]
    device_pages = fields["l1_capacity_pages"]
    storage = dict(extra or {})
    storage["prefetch_threshold"] = fields["prefetch_threshold_pages"] * page_size
    for field in TIMEOUT_FIELDS:
        if field in fields:
            storage[field.removesuffix("_sec")] = fields[field]

    return {
        "page_size": str(page_size),
        "max_total_tokens": str(device_pages * page_size),
        "hicache_size": "0",
        "hicache_ratio": str((fields["l2_capacity_pages"] - 0.5) / device_pages),
        "hicache_write_policy": fields["write_policy"],
        "hicache_storage_prefetch_policy": fields["prefetch_policy"],
        "hicache_storage_backend_extra_config": json.dumps(storage, separators=(",", ":")),
    }


def resolve_hicache_fields(fields: dict[str, Any]) -> dict[str, Any]:
    """Resolve derived prefetch capacity using the same parser as source capture."""

    resolved = apply_sglang_capacity_from_flags(hicache_server_flags(fields), fields)
    if any(resolved[key] != fields[key] for key in ("page_size", "l1_capacity_pages", "l2_capacity_pages")):
        raise ValueError("page capacities cannot be represented by the SGLang launch flags")

    return {"enabled": True, **resolved}


def canonical_timeout(base: float, per_ki: float, maximum: float) -> tuple[float, float, float]:
    """Identity of min(maximum, base + per_ki * tokens / 1024) for nonnegative tokens."""

    if base >= maximum or per_ki == 0:
        constant = min(base, maximum)
        return constant, 0.0, constant

    return base, per_ki, maximum


def canonical_hicache(fields: dict[str, Any]) -> tuple[tuple[str, Any], ...]:
    """Execution identity, not a file fingerprint or a prediction-time identity.

    Ignore inactive timeout fields and normalize identical timeout functions.
    Emitted launch parameters stay unchanged; this identity is for matching only.
    """

    values = {"enabled": True, **fields}
    if values["prefetch_policy"] != "timeout":
        values = {key: value for key, value in values.items() if key not in TIMEOUT_FIELDS}
    elif all(key in values for key in TIMEOUT_FIELDS):
        values.update(zip(TIMEOUT_FIELDS, canonical_timeout(*(values[key] for key in TIMEOUT_FIELDS))))

    return tuple(sorted(values.items()))
