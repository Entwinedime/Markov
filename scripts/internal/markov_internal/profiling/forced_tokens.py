#!/usr/bin/env python3
"""HiCache forced-token profiling 输入契约。

该模块只描述 capture plan 与 replay workload 之间的稳定契约。它不启动
profiling，不读取 Python probe trace，也不把 forced output token 写入建模输入。
"""

from __future__ import annotations

import hashlib
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any


FORCED_TOKEN_PLAN_SCHEMA = "trace_sim.hicache.forced_token_plan.v1"
FORCED_TOKEN_BUNDLE_SCHEMA = "trace_sim.hicache.forced_token_bundle.v1"

FORCED_TOKEN_ERROR_SCHEMA_UNKNOWN = "forced_token_plan_schema_unknown"
FORCED_TOKEN_ERROR_PLAN_MISSING = "forced_token_plan_missing"
FORCED_TOKEN_ERROR_REQUEST_COUNT = "forced_token_plan_request_count_mismatch"
FORCED_TOKEN_ERROR_OUTPUT_MISMATCH = "forced_token_actual_output_mismatch"
FORCED_TOKEN_ERROR_OUTPUT_UNCHECKED = "forced_token_actual_output_unchecked"
FORCED_TOKEN_ERROR_ORIGIN_INPUT = "forced_token_origin_input_mismatch"
FORCED_TOKEN_ERROR_MODE_UNKNOWN = "forced_token_mode_unknown"
FORCED_TOKEN_ERROR_WORKLOAD_ID = "forced_token_plan_workload_id_mismatch"
FORCED_TOKEN_ERROR_WORKLOAD_FINGERPRINT = "forced_token_plan_workload_fingerprint_mismatch"
FORCED_TOKEN_ERROR_REQUEST_IDS = "forced_token_plan_request_id_mismatch"
FORCED_TOKEN_ERROR_REQUEST_TOKENS = "forced_token_plan_request_tokens_missing"
FORCED_TOKEN_ERROR_CAPTURE_OVERWRITE = "forced_token_capture_plan_exists"
FORCED_TOKEN_ERROR_BUNDLE_MISSING = "forced_token_bundle_missing"
FORCED_TOKEN_ERROR_BUNDLE_SCHEMA = "forced_token_bundle_schema_unknown"
FORCED_TOKEN_ERROR_BUNDLE_INPUT = "forced_token_bundle_input_missing"
FORCED_TOKEN_ERROR_BUNDLE_PLAN_MISSING = "forced_token_bundle_plan_missing"
FORCED_TOKEN_ERROR_BUNDLE_PLAN_HASH = "forced_token_bundle_plan_hash_mismatch"
FORCED_TOKEN_ERROR_BUNDLE_PLAN_METADATA = "forced_token_bundle_plan_metadata_mismatch"
FORCED_TOKEN_ERROR_BUNDLE_PROVENANCE = "forced_token_bundle_provenance_mismatch"


@dataclass(frozen=True)
class ForcedTokenPlanSummary:
    """forced-token plan 的轻量摘要，用于 manifest / quality / preflight。"""

    path: str | None
    exists: bool
    schema: str | None
    sha256: str | None
    workload_id: str | None
    workload_fingerprint: str | None
    request_count: int
    capture: dict[str, Any]

    def to_dict(self) -> dict[str, Any]:
        """转换为可写入 JSON 的稳定对象。"""

        return {
            "path": self.path,
            "exists": self.exists,
            "schema": self.schema,
            "sha256": self.sha256,
            "workload_id": self.workload_id,
            "workload_fingerprint": self.workload_fingerprint,
            "request_count": self.request_count,
            "capture": self.capture,
        }


@dataclass(frozen=True)
class ForcedTokenBundlePlan:
    """bundle 中单个 input 对应的 plan 解析结果。"""

    input_id: str
    bundle_path: str
    bundle_sha256: str
    bundle_id: str | None
    plan_path: str
    plan_sha256: str
    entry: dict[str, Any]

    def to_dict(self) -> dict[str, Any]:
        """转换为 runner / quality 可持久化的 provenance。"""

        return {
            "schema": FORCED_TOKEN_BUNDLE_SCHEMA,
            "path": self.bundle_path,
            "sha256": self.bundle_sha256,
            "bundle_id": self.bundle_id,
            "input_id": self.input_id,
            "plan_path": self.plan_path,
            "plan_sha256": self.plan_sha256,
        }


def sha256_json(value: Any) -> str:
    """生成稳定 JSON sha256 摘要。"""

    encoded = json.dumps(
        value,
        ensure_ascii=True,
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")
    return "sha256_json:" + hashlib.sha256(encoded).hexdigest()


def sha256_file(path: Path) -> str:
    """生成文件 sha256 摘要。"""

    hasher = hashlib.sha256()
    with path.open("rb") as file_obj:
        for chunk in iter(lambda: file_obj.read(1024 * 1024), b""):
            hasher.update(chunk)
    return "sha256_file:" + hasher.hexdigest()


def non_negative_int(value: Any) -> int:
    """宽松解析 report 计数字段，坏值按 0 处理。"""

    if value is None or isinstance(value, bool):
        return 0
    try:
        return max(0, int(float(value)))
    except (TypeError, ValueError):
        return 0


def int_list(value: Any) -> list[int] | None:
    """把 JSON 字段解析成 int list，失败时返回 None。"""

    if not isinstance(value, list):
        return None
    parsed: list[int] = []
    for item in value:
        if not isinstance(item, int):
            return None
        parsed.append(int(item))
    return parsed


def load_forced_token_plan(path: Path) -> dict[str, Any]:
    """读取并校验 forced-token plan 的最低结构。"""

    plan = json.loads(path.read_text(encoding="utf-8"))
    if plan.get("schema") != FORCED_TOKEN_PLAN_SCHEMA:
        raise ValueError(f"unsupported forced token plan schema: {plan.get('schema')}")
    if not isinstance(plan.get("requests"), list):
        raise ValueError("forced token plan must contain a requests list")
    return plan


def load_forced_token_bundle(path: Path) -> dict[str, Any]:
    """读取 forced-token bundle 并校验最低结构。"""

    bundle = json.loads(path.read_text(encoding="utf-8"))
    if bundle.get("schema") != FORCED_TOKEN_BUNDLE_SCHEMA:
        raise ValueError(f"unsupported forced token bundle schema: {bundle.get('schema')}")
    if not isinstance(bundle.get("bundle_id"), str) or not bundle["bundle_id"]:
        raise ValueError("forced token bundle must contain bundle_id")
    if not isinstance(bundle.get("capture_suite_dir"), str) or not bundle["capture_suite_dir"]:
        raise ValueError("forced token bundle must contain capture_suite_dir")
    if not isinstance(bundle.get("capture_config"), str) or not bundle["capture_config"]:
        raise ValueError("forced token bundle must contain capture_config")
    if not isinstance(bundle.get("model_path"), str) or not bundle["model_path"]:
        raise ValueError("forced token bundle must contain model_path")
    if not isinstance(bundle.get("server_config_id"), str) or not bundle["server_config_id"]:
        raise ValueError("forced token bundle must contain server_config_id")
    if not isinstance(bundle.get("plans"), dict) or not bundle["plans"]:
        raise ValueError("forced token bundle must contain a non-empty plans object")
    return bundle


def resolve_forced_token_bundle_plan(bundle_path: Path, input_id: str) -> ForcedTokenBundlePlan:
    """解析并验证 bundle 中一个 input 的 plan 文件。"""

    if not bundle_path.is_file():
        raise ValueError(FORCED_TOKEN_ERROR_BUNDLE_MISSING)
    try:
        bundle = load_forced_token_bundle(bundle_path)
    except (OSError, json.JSONDecodeError, ValueError) as exc:
        raise ValueError(f"{FORCED_TOKEN_ERROR_BUNDLE_SCHEMA}:{exc}") from exc

    raw_entry = bundle["plans"].get(input_id)
    if not isinstance(raw_entry, dict):
        raise ValueError(f"{FORCED_TOKEN_ERROR_BUNDLE_INPUT}:{input_id}")
    raw_plan_path = raw_entry.get("path")
    if not isinstance(raw_plan_path, str) or not raw_plan_path:
        raise ValueError(f"{FORCED_TOKEN_ERROR_BUNDLE_PLAN_MISSING}:{input_id}")
    relative_plan_path = Path(raw_plan_path)
    if relative_plan_path.is_absolute():
        raise ValueError(f"{FORCED_TOKEN_ERROR_BUNDLE_PLAN_METADATA}:{input_id}:plan path must be relative")
    bundle_dir = bundle_path.parent.resolve()
    plan_path = (bundle_dir / relative_plan_path).resolve()
    if plan_path != bundle_dir and bundle_dir not in plan_path.parents:
        raise ValueError(f"{FORCED_TOKEN_ERROR_BUNDLE_PLAN_METADATA}:{input_id}:plan path escapes bundle")
    if not plan_path.is_file():
        raise ValueError(f"{FORCED_TOKEN_ERROR_BUNDLE_PLAN_MISSING}:{plan_path}")

    actual_plan_sha256 = sha256_file(plan_path)
    expected_plan_sha256 = raw_entry.get("sha256")
    if expected_plan_sha256 != actual_plan_sha256:
        raise ValueError(
            f"{FORCED_TOKEN_ERROR_BUNDLE_PLAN_HASH}:{input_id}:"
            f"expected={expected_plan_sha256}:actual={actual_plan_sha256}"
        )

    try:
        plan = load_forced_token_plan(plan_path)
    except (OSError, json.JSONDecodeError, ValueError) as exc:
        raise ValueError(f"{FORCED_TOKEN_ERROR_BUNDLE_PLAN_MISSING}:{exc}") from exc
    entry_metadata = (
        raw_entry.get("workload_id"),
        raw_entry.get("workload_fingerprint"),
        non_negative_int(raw_entry.get("request_count")),
    )
    plan_metadata = (
        plan_workload_id(plan),
        plan_workload_fingerprint(plan),
        plan_request_count(plan),
    )
    if entry_metadata != plan_metadata or plan_metadata[0] != input_id:
        raise ValueError(
            f"{FORCED_TOKEN_ERROR_BUNDLE_PLAN_METADATA}:{input_id}:"
            f"entry={entry_metadata}:plan={plan_metadata}"
        )

    return ForcedTokenBundlePlan(
        input_id=input_id,
        bundle_path=str(bundle_path.resolve()),
        bundle_sha256=sha256_file(bundle_path),
        bundle_id=str(bundle.get("bundle_id")) if bundle.get("bundle_id") else None,
        plan_path=str(plan_path),
        plan_sha256=actual_plan_sha256,
        entry=raw_entry,
    )


def forced_token_bundle_summary(path: Path | None) -> dict[str, Any]:
    """生成 suite artifact 可直接记录的 bundle 摘要。"""

    result: dict[str, Any] = {
        "path": str(path) if path is not None else None,
        "exists": bool(path and path.is_file()),
        "schema": None,
        "sha256": None,
        "bundle_id": None,
        "capture_suite_dir": None,
        "capture_config": None,
        "input_ids": [],
        "plan_count": 0,
        "errors": [],
        "ready": False,
    }
    if path is None or not path.is_file():
        result["errors"] = [FORCED_TOKEN_ERROR_BUNDLE_MISSING]
        return result
    result["sha256"] = sha256_file(path)
    try:
        bundle = load_forced_token_bundle(path)
    except (OSError, json.JSONDecodeError, ValueError) as exc:
        result["errors"] = [f"{FORCED_TOKEN_ERROR_BUNDLE_SCHEMA}:{exc}"]
        return result
    plans = bundle.get("plans") if isinstance(bundle.get("plans"), dict) else {}
    result.update(
        {
            "schema": bundle.get("schema"),
            "bundle_id": bundle.get("bundle_id"),
            "capture_suite_dir": bundle.get("capture_suite_dir"),
            "capture_config": bundle.get("capture_config"),
            "input_ids": sorted(str(key) for key in plans),
            "plan_count": len(plans),
            "ready": True,
        }
    )
    return result


def plan_workload_id(plan: dict[str, Any]) -> str | None:
    """读取 plan 的 workload id。"""

    value = plan.get("workload_id")
    return value if isinstance(value, str) and value else None


def plan_workload_fingerprint(plan: dict[str, Any]) -> str | None:
    """读取 plan 的 workload fingerprint。"""

    value = plan.get("workload_fingerprint")
    return value if isinstance(value, str) and value else None


def plan_request_count(plan: dict[str, Any]) -> int:
    """返回 plan 中 request 条目数量。"""

    requests = plan.get("requests")
    return len(requests) if isinstance(requests, list) else 0


def index_plan_requests_by_logical_id(plan: dict[str, Any]) -> dict[str, dict[str, Any]]:
    """按 logical_request_id 建立 forced-token plan 索引。"""

    indexed: dict[str, dict[str, Any]] = {}
    for request in plan.get("requests") or []:
        if not isinstance(request, dict):
            raise ValueError("forced token plan contains a non-object request")
        logical_id = request.get("logical_request_id")
        if not isinstance(logical_id, str) or not logical_id:
            raise ValueError("forced token plan request missing logical_request_id")
        if logical_id in indexed:
            raise ValueError(f"duplicate logical_request_id in forced token plan: {logical_id}")
        indexed[logical_id] = request
    return indexed


def forced_token_plan_summary(path: Path | None) -> ForcedTokenPlanSummary:
    """读取 plan 摘要；缺失或坏文件不会抛出，交给 preflight/quality 报错。"""

    if path is None:
        return ForcedTokenPlanSummary(
            path=None,
            exists=False,
            schema=None,
            sha256=None,
            workload_id=None,
            workload_fingerprint=None,
            request_count=0,
            capture={},
        )
    if not path.is_file():
        return ForcedTokenPlanSummary(
            path=str(path),
            exists=False,
            schema=None,
            sha256=None,
            workload_id=None,
            workload_fingerprint=None,
            request_count=0,
            capture={},
        )
    try:
        plan = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return ForcedTokenPlanSummary(
            path=str(path),
            exists=True,
            schema=None,
            sha256=sha256_file(path),
            workload_id=None,
            workload_fingerprint=None,
            request_count=0,
            capture={},
        )
    capture = plan.get("capture") if isinstance(plan.get("capture"), dict) else {}
    return ForcedTokenPlanSummary(
        path=str(path),
        exists=True,
        schema=str(plan.get("schema")) if plan.get("schema") is not None else None,
        sha256=sha256_file(path),
        workload_id=plan_workload_id(plan),
        workload_fingerprint=plan_workload_fingerprint(plan),
        request_count=plan_request_count(plan),
        capture=capture,
    )


def validate_plan_contract(
    plan: dict[str, Any],
    *,
    workload_id: str | None,
    workload_fingerprint: str | None,
    expected_request_ids: list[str] | None = None,
) -> list[str]:
    """校验 plan 与当前 workload 是否描述同一条 logical token timeline。"""

    errors: list[str] = []
    if plan.get("schema") != FORCED_TOKEN_PLAN_SCHEMA:
        errors.append(FORCED_TOKEN_ERROR_SCHEMA_UNKNOWN)
    if workload_id and plan_workload_id(plan) != workload_id:
        errors.append(FORCED_TOKEN_ERROR_WORKLOAD_ID)
    if workload_fingerprint and plan_workload_fingerprint(plan) != workload_fingerprint:
        errors.append(FORCED_TOKEN_ERROR_WORKLOAD_FINGERPRINT)

    requests = plan.get("requests")
    if not isinstance(requests, list) or not requests:
        errors.append(FORCED_TOKEN_ERROR_REQUEST_COUNT)
    for request in requests or []:
        if not isinstance(request, dict):
            errors.append(FORCED_TOKEN_ERROR_REQUEST_COUNT)
            continue
        if (
            int_list(request.get("origin_input_ids")) is None
            or int_list(request.get("forced_output_ids")) is None
        ):
            errors.append(FORCED_TOKEN_ERROR_REQUEST_TOKENS)
    if expected_request_ids is not None:
        actual_request_ids = [
            str(request.get("logical_request_id"))
            for request in requests or []
            if isinstance(request, dict)
        ]
        if actual_request_ids != expected_request_ids:
            errors.append(FORCED_TOKEN_ERROR_REQUEST_IDS)
    return sorted(set(errors))


def empty_forced_token_quality() -> dict[str, Any]:
    """返回 forced-token gate 的稳定空结果。"""

    return {
        "enabled": False,
        "mode": "none",
        "ready": True,
        "plan_ready": True,
        "bundle_ready": True,
        "errors": [],
        "plan_schema": None,
        "plan_sha256": None,
        "plan_workload_id": None,
        "plan_workload_fingerprint": None,
        "plan_capture": None,
        "plan_capture_run_id": None,
        "plan_capture_config_id": None,
        "plan_capture_input_id": None,
        "plan_capture_model_path": None,
        "plan_capture_tokenizer_digest": None,
        "request_count": 0,
        "output_checked_count": 0,
        "mismatch_count": 0,
        "unchecked_count": 0,
        "prompt_mismatch_count": 0,
        "all_actual_outputs_match_plan": None,
        "plan_written": None,
        "bundle_path": None,
        "bundle_schema": None,
        "bundle_sha256": None,
        "bundle_id": None,
        "bundle_plan_sha256": None,
    }


def forced_token_quality_from_workload_report(path: Path | None) -> dict[str, Any]:
    """从 workload report 检查 forced-token profiling 输入契约。"""

    result = empty_forced_token_quality()
    if path is None or not path.is_file():
        return result
    try:
        report = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return result
    return forced_token_quality_from_report(report)


def forced_token_quality_from_report(report: dict[str, Any]) -> dict[str, Any]:
    """从已加载的 workload report 生成 forced-token quality 摘要。"""

    result = empty_forced_token_quality()
    forced = report.get("forced_token")
    if not isinstance(forced, dict) or not forced.get("enabled"):
        return result

    mode = str(forced.get("mode") or "unknown")
    errors: list[str] = []
    result.update(
        {
            "enabled": True,
            "mode": mode,
            "ready": bool(forced.get("ready", False)),
            "plan_schema": forced.get("plan_schema"),
            "plan_sha256": forced.get("plan_sha256"),
            "plan_workload_id": forced.get("plan_workload_id"),
            "plan_workload_fingerprint": forced.get("plan_workload_fingerprint"),
            "plan_capture": forced.get("plan_capture") if isinstance(forced.get("plan_capture"), dict) else None,
            "plan_capture_run_id": forced.get("plan_capture_run_id"),
            "plan_capture_config_id": forced.get("plan_capture_config_id"),
            "plan_capture_input_id": forced.get("plan_capture_input_id"),
            "plan_capture_model_path": forced.get("plan_capture_model_path"),
            "plan_capture_tokenizer_digest": forced.get("plan_capture_tokenizer_digest"),
            "request_count": non_negative_int(forced.get("request_count")),
            "output_checked_count": non_negative_int(forced.get("output_checked_count")),
            "mismatch_count": non_negative_int(forced.get("mismatch_count")),
            "unchecked_count": non_negative_int(forced.get("unchecked_count")),
            "prompt_mismatch_count": non_negative_int(forced.get("prompt_mismatch_count")),
            "all_actual_outputs_match_plan": forced.get("all_actual_outputs_match_plan"),
            "plan_written": forced.get("plan_written"),
            "bundle_path": forced.get("bundle_path"),
            "bundle_schema": forced.get("bundle_schema"),
            "bundle_sha256": forced.get("bundle_sha256"),
            "bundle_id": forced.get("bundle_id"),
            "bundle_plan_sha256": forced.get("bundle_plan_sha256"),
        }
    )

    if forced.get("plan_schema") != FORCED_TOKEN_PLAN_SCHEMA:
        errors.append(FORCED_TOKEN_ERROR_SCHEMA_UNKNOWN)
    if not forced.get("plan_sha256"):
        errors.append(FORCED_TOKEN_ERROR_PLAN_MISSING)
    if result["request_count"] <= 0:
        errors.append(FORCED_TOKEN_ERROR_REQUEST_COUNT)
    if mode == "capture":
        if forced.get("plan_written") is not True:
            errors.append(FORCED_TOKEN_ERROR_PLAN_MISSING)
    elif mode == "replay":
        if forced.get("all_actual_outputs_match_plan") is not True:
            errors.append(FORCED_TOKEN_ERROR_OUTPUT_MISMATCH)
        if result["unchecked_count"] > 0 or result["output_checked_count"] != result["request_count"]:
            errors.append(FORCED_TOKEN_ERROR_OUTPUT_UNCHECKED)
        if result.get("prompt_mismatch_count", 0) > 0:
            errors.append(FORCED_TOKEN_ERROR_ORIGIN_INPUT)
        if (
            not forced.get("bundle_path")
            or forced.get("bundle_schema") != FORCED_TOKEN_BUNDLE_SCHEMA
            or not forced.get("bundle_sha256")
            or not forced.get("bundle_id")
            or forced.get("bundle_plan_sha256") != forced.get("plan_sha256")
        ):
            errors.append(FORCED_TOKEN_ERROR_BUNDLE_PROVENANCE)
    else:
        errors.append(FORCED_TOKEN_ERROR_MODE_UNKNOWN)

    result["errors"] = sorted(set(errors))
    result["plan_ready"] = not any(
        error != FORCED_TOKEN_ERROR_BUNDLE_PROVENANCE
        for error in result["errors"]
    )
    result["bundle_ready"] = (
        mode != "replay"
        or FORCED_TOKEN_ERROR_BUNDLE_PROVENANCE not in result["errors"]
    )
    result["ready"] = result["plan_ready"] and result["bundle_ready"]
    return result
