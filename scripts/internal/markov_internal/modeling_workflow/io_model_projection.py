"""Projection of one compact I/O model into a target C++ request."""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

from .io_model_contract import is_missing_contract_value, positive_u64

if TYPE_CHECKING:
    from .io_model import HiCacheIoModel


def merge_hicache_io_model(config: dict[str, Any], model: HiCacheIoModel | None) -> dict[str, Any]:
    merged = dict(config)
    _reject_target_local_io_model(merged)
    if model is None:
        return merged
    page_size = positive_u64(merged.get("page_size"), "target hicache.page_size")
    merged.update(model.narrow_config(page_size))
    return merged


def _reject_target_local_io_model(config: dict[str, Any]) -> None:
    if any(
        not is_missing_contract_value(config.get(field))
        for field in ("kv_bytes_per_page", "io_planning", "io_cost")
    ):
        raise ValueError("target config cannot carry per-cell I/O cost fields; use --hicache-io-model")
