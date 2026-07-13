"""Explicit HiCache model geometry and I/O cost contract."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Any

from ..common.digests import sha256_json
from ..common.io import load_json
from ..common.paths import repo_relative_path, require_repo_path


HICACHE_IO_MODEL_SCHEMA = "markov.hicache.io_model.v1"
HICACHE_RESOURCE_MODEL = "scope_local_directional_device_host_shared_host_storage_v1"
HICACHE_IO_MODEL_CALIBRATION_STATUSES = frozenset({"contract_only", "calibrated"})
MAX_U64 = (1 << 64) - 1
PROVENANCE_FIELDS = (
    "kv_geometry",
    "device_host_bandwidth",
    "host_storage_bandwidth",
)
IO_COST_FIELDS = (
    "model_id",
    "model_digest",
    "calibration_status",
    "resource_model",
    "device_host_bandwidth_bytes_per_sec",
    "host_storage_bandwidth_bytes_per_sec",
    "provenance",
)


@dataclass(frozen=True)
class HiCacheIoModel:
    """Validated model geometry and the only two HiCache cost parameters."""

    source_path: Path
    model_id: str
    calibration_status: str
    kv_bytes_per_token_per_rank: int
    device_host_bandwidth_bytes_per_sec: int
    host_storage_bandwidth_bytes_per_sec: int
    provenance: tuple[tuple[str, str], ...]
    digest: str

    @classmethod
    def load(cls, path: Path) -> HiCacheIoModel:
        """Load one strict, complete I/O model without accepting silent defaults."""

        resolved = require_repo_path(path)
        if not resolved.is_file():
            raise FileNotFoundError(f"missing HiCache I/O model: {resolved}")
        raw = load_json(resolved)
        if not isinstance(raw, dict):
            raise TypeError(f"HiCache I/O model must be a JSON object: {resolved}")

        allowed_fields = {
            "schema",
            "model_id",
            "calibration_status",
            "kv_bytes_per_token_per_rank",
            "device_host_bandwidth_bytes_per_sec",
            "host_storage_bandwidth_bytes_per_sec",
            "provenance",
        }
        unknown = sorted(set(raw) - allowed_fields)
        if unknown:
            raise ValueError(f"unknown HiCache I/O model fields: {', '.join(unknown)}")
        if raw.get("schema") != HICACHE_IO_MODEL_SCHEMA:
            raise ValueError(f"HiCache I/O model requires schema={HICACHE_IO_MODEL_SCHEMA}")

        model_id = required_text(raw.get("model_id"), "model_id")
        calibration_status = required_calibration_status(raw.get("calibration_status"))
        provenance = required_provenance(raw.get("provenance"))
        normalized = {
            "schema": HICACHE_IO_MODEL_SCHEMA,
            "model_id": model_id,
            "calibration_status": calibration_status,
            "kv_bytes_per_token_per_rank": positive_u64(
                raw.get("kv_bytes_per_token_per_rank"),
                "kv_bytes_per_token_per_rank",
            ),
            "device_host_bandwidth_bytes_per_sec": positive_u64(
                raw.get("device_host_bandwidth_bytes_per_sec"),
                "device_host_bandwidth_bytes_per_sec",
            ),
            "host_storage_bandwidth_bytes_per_sec": positive_u64(
                raw.get("host_storage_bandwidth_bytes_per_sec"),
                "host_storage_bandwidth_bytes_per_sec",
            ),
            "provenance": dict(provenance),
        }
        return cls(
            source_path=resolved,
            model_id=model_id,
            calibration_status=calibration_status,
            kv_bytes_per_token_per_rank=normalized["kv_bytes_per_token_per_rank"],
            device_host_bandwidth_bytes_per_sec=normalized["device_host_bandwidth_bytes_per_sec"],
            host_storage_bandwidth_bytes_per_sec=normalized["host_storage_bandwidth_bytes_per_sec"],
            provenance=provenance,
            digest=sha256_json(normalized),
        )

    def kv_bytes_per_page(self, page_size: int) -> int:
        """Project per-rank bytes for one target page with uint64 overflow checks."""

        if page_size <= 0:
            raise ValueError("HiCache I/O model requires a positive target page_size")
        if page_size > MAX_U64 // self.kv_bytes_per_token_per_rank:
            raise OverflowError("target page_size * kv_bytes_per_token_per_rank exceeds uint64")
        return page_size * self.kv_bytes_per_token_per_rank

    def metadata(self) -> dict[str, Any]:
        """Return compact reproducibility metadata without external file dependency."""

        return {
            "schema": HICACHE_IO_MODEL_SCHEMA,
            "model_id": self.model_id,
            "calibration_status": self.calibration_status,
            "digest": self.digest,
            "source_path": str(repo_relative_path(self.source_path)),
            "resource_model": HICACHE_RESOURCE_MODEL,
            "kv_bytes_per_token_per_rank": self.kv_bytes_per_token_per_rank,
            "device_host_bandwidth_bytes_per_sec": self.device_host_bandwidth_bytes_per_sec,
            "host_storage_bandwidth_bytes_per_sec": self.host_storage_bandwidth_bytes_per_sec,
            "provenance": dict(self.provenance),
        }

    def narrow_config(self, page_size: int) -> dict[str, Any]:
        """Return the model fields copied into one target C++ narrow config."""

        return {
            "kv_bytes_per_page": self.kv_bytes_per_page(page_size),
            "io_cost": {
                "model_id": self.model_id,
                "model_digest": self.digest,
                "calibration_status": self.calibration_status,
                "resource_model": HICACHE_RESOURCE_MODEL,
                "device_host_bandwidth_bytes_per_sec": self.device_host_bandwidth_bytes_per_sec,
                "host_storage_bandwidth_bytes_per_sec": self.host_storage_bandwidth_bytes_per_sec,
                "provenance": dict(self.provenance),
            },
        }


def merge_hicache_io_model(config: dict[str, Any], model: HiCacheIoModel | None) -> dict[str, Any]:
    """Merge one workflow-wide I/O model without overriding conflicting target facts."""

    merged = dict(config)
    if model is None:
        reject_per_target_io_model(merged)
        return merged

    page_size = positive_u64(merged.get("page_size"), "target hicache.page_size")
    narrow = model.narrow_config(page_size)
    reject_conflict(merged.get("kv_bytes_per_page"), narrow["kv_bytes_per_page"], "hicache.kv_bytes_per_page")

    existing_io = validated_existing_io_cost(merged.get("io_cost"))
    generated_io = narrow["io_cost"]
    for field in IO_COST_FIELDS:
        reject_conflict(existing_io.get(field), generated_io[field], f"hicache.io_cost.{field}")

    merged["kv_bytes_per_page"] = narrow["kv_bytes_per_page"]
    merged["io_cost"] = generated_io
    return merged


def required_text(value: Any, field: str) -> str:
    """Return one non-empty string or reject the malformed contract field."""

    if not isinstance(value, str) or not value.strip():
        raise ValueError(f"HiCache I/O model field '{field}' must be a non-empty string")
    return value.strip()


def required_calibration_status(value: Any) -> str:
    """Return the explicit model-use class that gates production graph mutation."""

    status = required_text(value, "calibration_status")
    if status not in HICACHE_IO_MODEL_CALIBRATION_STATUSES:
        allowed = ", ".join(sorted(HICACHE_IO_MODEL_CALIBRATION_STATUSES))
        raise ValueError(f"HiCache I/O model calibration_status must be one of: {allowed}")
    return status


def positive_u64(value: Any, field: str) -> int:
    """Return one positive uint64 while rejecting bools and lossy numeric forms."""

    if isinstance(value, bool) or not isinstance(value, int) or value <= 0 or value > MAX_U64:
        raise ValueError(f"HiCache I/O model field '{field}' must be a positive uint64 integer")
    return value


def required_provenance(value: Any) -> tuple[tuple[str, str], ...]:
    """Validate exact provenance fields for geometry and both bandwidth parameters."""

    if not isinstance(value, dict):
        raise ValueError("HiCache I/O model field 'provenance' must be an object")
    unknown = sorted(set(value) - set(PROVENANCE_FIELDS))
    missing = sorted(set(PROVENANCE_FIELDS) - set(value))
    if unknown or missing:
        details = []
        if missing:
            details.append("missing=" + ",".join(missing))
        if unknown:
            details.append("unknown=" + ",".join(unknown))
        raise ValueError("invalid HiCache I/O model provenance: " + "; ".join(details))
    return tuple((field, required_text(value[field], f"provenance.{field}")) for field in PROVENANCE_FIELDS)


def reject_conflict(existing: Any, generated: Any, field: str) -> None:
    """Reject an explicit target value that disagrees with the workflow-wide model."""

    if is_missing_contract_value(existing):
        return
    if existing != generated:
        raise ValueError(f"conflicting {field}: target={existing!r}, io_model={generated!r}")


def reject_per_target_io_model(config: dict[str, Any]) -> None:
    """Reject target-local cost inputs when no workflow-wide model was selected."""

    if not is_missing_contract_value(config.get("kv_bytes_per_page")):
        raise ValueError(
            "target hicache.kv_bytes_per_page cannot define a per-cell I/O model; "
            "use workflow option --hicache-io-model"
        )
    if not is_missing_contract_value(config.get("io_cost")):
        raise ValueError(
            "target hicache.io_cost cannot define per-cell I/O parameters; use workflow option --hicache-io-model"
        )


def validated_existing_io_cost(value: Any) -> dict[str, Any]:
    """Validate an optional target copy before checking it against the workflow model."""

    if value is None:
        return {}
    if not isinstance(value, dict):
        raise ValueError("target hicache.io_cost must be an object")
    unknown = sorted(set(value) - set(IO_COST_FIELDS))
    if unknown:
        raise ValueError(f"unknown target hicache.io_cost fields: {', '.join(unknown)}")
    if not is_missing_contract_value(value.get("provenance")):
        required_provenance(value["provenance"])
    return value


def is_missing_contract_value(value: Any) -> bool:
    """Return whether a target field carries no explicit model value."""

    if value is None or value == "" or value == {}:
        return True
    return isinstance(value, int) and not isinstance(value, bool) and value == 0
