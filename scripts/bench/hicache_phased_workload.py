#!/usr/bin/env python3
"""生成确定性 HiCache 分阶段 workload。

该脚本只负责向目标 SGLang server 发送可复现请求，并输出 workload_report。
report 中的 expected mechanisms 用于 profile quality 审计，不作为 C++ state model 输入。
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import statistics
import sys
import time
import urllib.request
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, Iterable, List


SCRIPT_DIR = Path(__file__).resolve().parent
INTERNAL_DIR = SCRIPT_DIR.parent / "internal"
if str(INTERNAL_DIR) not in sys.path:
    sys.path.insert(0, str(INTERNAL_DIR))

from hicache_forced_token_contract import (  # noqa: E402
    FORCED_TOKEN_BUNDLE_SCHEMA,
    FORCED_TOKEN_PLAN_SCHEMA,
    forced_token_plan_summary,
    index_plan_requests_by_logical_id,
    int_list,
    load_forced_token_plan,
    sha256_file,
    sha256_json,
    validate_plan_contract,
)


NO_PROXY_OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))

PHASE_ORDER = (
    "warmup",
    "seed_A",
    "reuse_A",
    "backup_wait_A",
    "pressure_B",
    "reuse_A_after_pressure",
    "prefetch_seed_C",
    "prefetch_reuse_C",
    "dirty_eviction",
)


PHASE_EXPECTED_MECHANISMS = {
    "seed_A": ["lookup", "insert", "write_backup", "write_storage"],
    "reuse_A": ["lookup"],
    "backup_wait_A": ["lookup", "write_backup", "write_storage"],
    "pressure_B": ["lookup", "insert", "evict"],
    "reuse_A_after_pressure": ["lookup", "load_back"],
    "prefetch_seed_C": ["lookup", "insert", "write_backup", "write_storage"],
    "prefetch_reuse_C": [
        "lookup",
        "prefetch_decision",
        "prefetch_schedule",
        "prefetch_query",
        "prefetch_transfer",
    ],
    "dirty_eviction": ["lookup", "insert", "evict", "write_backup", "write_storage"],
}


WRITE_BACK_PHASE_EXPECTED_MECHANISMS = {
    **PHASE_EXPECTED_MECHANISMS,
    "seed_A": ["lookup", "insert"],
    "backup_wait_A": ["lookup"],
    "prefetch_seed_C": ["lookup", "insert"],
    "prefetch_reuse_C": [
        "lookup",
        "prefetch_decision",
        "prefetch_schedule",
        "prefetch_query",
    ],
}


def now_ms() -> float:
    """返回单调时钟毫秒值，用于请求 latency 统计。"""

    return time.perf_counter() * 1000.0


def wall_ms() -> float:
    """返回墙钟毫秒值，用于把 workload 时间窗写入报告。"""

    return time.time() * 1000.0


def make_shared_prefix(label: str, repeat: int) -> str:
    """构造共享 prefix，使多个请求命中同一 cache path 前缀。"""

    unit = f"HiCache calibration {label} shared prefix block. "
    return unit * repeat


def make_prompt(prefix: str, family: str, index: int, suffix_repeat: int) -> str:
    """在共享 prefix 后拼接可区分 suffix，形成同族请求。"""

    suffix = f" request family {family} item {index}. " * suffix_repeat
    return prefix + suffix


def token_ids_sha256_u32le(token_ids: Iterable[int]) -> str:
    """按 u32 little-endian token 序列生成稳定摘要。"""

    hasher = hashlib.sha256()
    for token_id in token_ids:
        token = int(token_id)
        if token < 0 or token >= 2**32:
            raise ValueError(f"token id out of u32 range: {token}")
        hasher.update(token.to_bytes(4, byteorder="little", signed=False))
    return "sha256_u32le:" + hasher.hexdigest()


def text_sha256(text: str) -> str:
    """生成文本 sha256 摘要。"""

    return "sha256_text:" + hashlib.sha256(text.encode("utf-8")).hexdigest()


def response_payload_digest(payload: bytes) -> Dict[str, Any]:
    """提取 `/generate` 响应摘要和 token ids，供 capture/replay 直接校验。"""

    body_hash = hashlib.sha256(payload).hexdigest()
    try:
        decoded = json.loads(payload.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError):
        return {"response_body_sha256": body_hash}

    result: Dict[str, Any] = {
        "response_body_sha256": body_hash,
        "_response_json": decoded,
    }
    text = decoded.get("text") if isinstance(decoded, dict) else None
    if isinstance(text, str):
        result.update(
            {
                "generated_text_chars": len(text),
                "generated_text_sha256": hashlib.sha256(
                    text.encode("utf-8")
                ).hexdigest(),
            }
        )
    output_ids = (
        int_list(decoded.get("output_ids")) if isinstance(decoded, dict) else None
    )
    if output_ids is not None:
        result.update(
            {
                "actual_output_count": len(output_ids),
                "actual_output_ids_sha256_u32le": token_ids_sha256_u32le(output_ids),
            }
        )
    prompt_token_ids = (
        int_list(decoded.get("prompt_token_ids"))
        if isinstance(decoded, dict)
        else None
    )
    if prompt_token_ids is not None:
        result.update(
            {
                "origin_input_count": len(prompt_token_ids),
                "origin_input_ids_sha256_u32le": token_ids_sha256_u32le(prompt_token_ids),
            }
        )
    return result


def sampling_params(
    args: argparse.Namespace,
    *,
    max_new_tokens: int | None = None,
) -> Dict[str, Any]:
    """生成显式 greedy sampling 参数，避免继承模型默认采样配置。"""

    return {
        "max_new_tokens": (
            args.max_new_tokens if max_new_tokens is None else max_new_tokens
        ),
        "temperature": 0,
        "top_p": args.top_p,
        "top_k": args.top_k,
        "sampling_seed": args.sampling_seed,
        "ignore_eos": True,
    }


def request_generate(
    url: str,
    body: Dict[str, Any],
    args: argparse.Namespace,
) -> Dict[str, Any]:
    """调用 SGLang `/generate`，返回请求状态和 latency 事实。"""

    request = urllib.request.Request(
        url,
        data=json.dumps(body).encode("utf-8"),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    start_monotonic = now_ms()
    start_wall = wall_ms()
    try:
        with NO_PROXY_OPENER.open(request, timeout=args.timeout_sec) as response:
            payload = response.read()
        end_monotonic = now_ms()
        return {
            "status": "ok",
            "http_status": response.status,
            "response_bytes": len(payload),
            **response_payload_digest(payload),
            "start_time_ms": start_wall,
            "end_time_ms": wall_ms(),
            "latency_ms": end_monotonic - start_monotonic,
        }
    except Exception as exc:
        end_monotonic = now_ms()
        return {
            "status": "error",
            "error": type(exc).__name__,
            "error_message": str(exc),
            "response_bytes": 0,
            "start_time_ms": start_wall,
            "end_time_ms": wall_ms(),
            "latency_ms": end_monotonic - start_monotonic,
        }


def phase_stats(rows: Iterable[Dict[str, Any]]) -> Dict[str, Any]:
    """汇总一个 phase 的请求数量、错误数和 latency 分布。"""

    rows = list(rows)
    latencies = [float(row["latency_ms"]) for row in rows]
    ok = [row for row in rows if row.get("status") == "ok"]
    if not latencies:
        return {"requests": 0, "ok": 0, "errors": 0, "latency_ms_sum": 0.0}
    return {
        "requests": len(rows),
        "ok": len(ok),
        "errors": len(rows) - len(ok),
        "latency_ms_sum": sum(latencies),
        "latency_ms_mean": statistics.fmean(latencies),
        "latency_ms_min": min(latencies),
        "latency_ms_max": max(latencies),
        "latency_ms_p50": statistics.median(latencies),
    }


def build_plan(args: argparse.Namespace) -> List[Dict[str, Any]]:
    """根据 CLI 参数生成固定顺序的 phase/request 计划。"""

    prefix_a = make_shared_prefix("A", args.shared_prefix_repeat)
    prefix_b = make_shared_prefix("B", args.shared_prefix_repeat)
    prefix_c = make_shared_prefix("C", args.shared_prefix_repeat)
    prefix_dirty = make_shared_prefix("D", args.shared_prefix_repeat)
    seed_prompts = [
        make_prompt(prefix_a, "A", index, args.unique_suffix_repeat)
        for index in range(args.seed_requests)
    ]
    if args.pressure_unique_prefix:
        pressure_prompts = [
            make_prompt(
                make_shared_prefix(f"B{index}", args.shared_prefix_repeat),
                "B",
                index,
                args.unique_suffix_repeat,
            )
            for index in range(args.pressure_requests)
        ]
    else:
        pressure_prompts = [
            make_prompt(prefix_b, "B", index, args.unique_suffix_repeat)
            for index in range(args.pressure_requests)
        ]
    prefetch_prompts = [
        make_prompt(prefix_c, "C", index, args.unique_suffix_repeat)
        for index in range(args.prefetch_seed_requests)
    ]
    dirty_prompts = [
        make_prompt(prefix_dirty, "D", index, args.unique_suffix_repeat)
        for index in range(args.dirty_eviction_requests)
    ]

    plan: List[Dict[str, Any]] = []
    for index in range(args.warmup_requests):
        plan.append(
            {
                "phase": "warmup",
                "prompt_id": f"warmup_{index}",
                "prompt": make_prompt(prefix_a, "warmup", index, 1),
            }
        )
    for index, prompt in enumerate(seed_prompts):
        plan.append({"phase": "seed_A", "prompt_id": f"A_{index}", "prompt": prompt})
    for index in range(args.reuse_requests):
        source_index = index % max(1, len(seed_prompts))
        plan.append(
            {
                "phase": "reuse_A",
                "prompt_id": f"A_{source_index}",
                "prompt": seed_prompts[source_index],
            }
        )
    for index in range(args.backup_wait_requests):
        source_index = index % max(1, len(seed_prompts))
        plan.append(
            {
                "phase": "backup_wait_A",
                "prompt_id": f"A_{source_index}",
                "prompt": seed_prompts[source_index],
            }
        )
    for index, prompt in enumerate(pressure_prompts):
        plan.append({"phase": "pressure_B", "prompt_id": f"B_{index}", "prompt": prompt})
    for index in range(args.reuse_after_pressure_requests):
        source_index = index % max(1, len(seed_prompts))
        plan.append(
            {
                "phase": "reuse_A_after_pressure",
                "prompt_id": f"A_{source_index}",
                "prompt": seed_prompts[source_index],
            }
        )
    for index, prompt in enumerate(prefetch_prompts):
        plan.append(
            {"phase": "prefetch_seed_C", "prompt_id": f"C_{index}", "prompt": prompt}
        )
    for index in range(args.prefetch_reuse_requests):
        source_index = index % max(1, len(prefetch_prompts))
        plan.append(
            {
                "phase": "prefetch_reuse_C",
                "prompt_id": f"C_{source_index}",
                "prompt": prefetch_prompts[source_index],
            }
        )
    for index, prompt in enumerate(dirty_prompts):
        plan.append(
            {"phase": "dirty_eviction", "prompt_id": f"D_{index}", "prompt": prompt}
        )
    return plan


def expected_mechanisms_for_phase(phase: str, args: argparse.Namespace) -> List[str]:
    """根据目标写策略生成质量检查期望。

    write-back 不会在普通 insert 后立即写 L2/L3；只有 dirty eviction / flush
    才应该强制期待写回事件。该函数只影响 workload report，不影响请求内容。
    """

    policy = str(args.cache_write_policy or "write_through").lower()
    table = (
        WRITE_BACK_PHASE_EXPECTED_MECHANISMS
        if policy == "write_back"
        else PHASE_EXPECTED_MECHANISMS
    )
    return list(table.get(phase, []))


def workload_id(args: argparse.Namespace) -> str:
    """返回 workload 的稳定逻辑 id。"""

    if args.workload_id:
        return str(args.workload_id)
    return Path(args.output_dir).name


def logical_request_id(
    args: argparse.Namespace,
    item: Dict[str, Any],
    sequence_id: int,
) -> str:
    """生成跨配置稳定的 logical request id。"""

    return f"{workload_id(args)}:{sequence_id}:{item['phase']}:{item['prompt_id']}"


def plan_prompt_descriptor(
    args: argparse.Namespace,
    item: Dict[str, Any],
    sequence_id: int,
) -> Dict[str, Any]:
    """生成 forced plan 中用于验证 request plan 未漂移的 prompt 描述。"""

    prompt = str(item["prompt"])
    return {
        "logical_request_id": logical_request_id(args, item, sequence_id),
        "sequence_id": sequence_id,
        "phase": item["phase"],
        "prompt_id": item["prompt_id"],
        "prompt_chars": len(prompt),
        "prompt_sha256": text_sha256(prompt),
    }


def workload_args_digest(args: argparse.Namespace) -> str:
    """生成影响 workload 形状的参数摘要，排除运行目录和 forced provenance。"""

    ignored = {
        "base_url",
        "output_dir",
        "timeout_sec",
        "sleep_sec",
        "phase_wait_sec",
        "max_errors",
        "forced_token_mode",
        "forced_token_plan",
        "cache_write_policy",
    }
    payload = {
        key: value
        for key, value in sorted(vars(args).items())
        if key not in ignored and value is not None
    }
    payload["script"] = "scripts/bench/hicache_phased_workload.py"
    return sha256_json(payload)


def forced_plan_by_request(plan: Dict[str, Any]) -> Dict[str, Dict[str, Any]]:
    """按 logical_request_id 建立 forced token plan 索引。"""

    return index_plan_requests_by_logical_id(plan)


def validate_forced_plan_matches_workload(
    args: argparse.Namespace,
    plan_items: List[Dict[str, Any]],
    forced_plan: Dict[str, Any],
) -> Dict[str, Dict[str, Any]]:
    """确认 replay 时当前 workload shape 与 capture plan 一致。"""

    indexed = forced_plan_by_request(forced_plan)
    expected_ids = [
        logical_request_id(args, item, sequence_id)
        for sequence_id, item in enumerate(plan_items)
    ]
    contract_errors = validate_plan_contract(
        forced_plan,
        workload_id=workload_id(args),
        workload_fingerprint=workload_args_digest(args),
        expected_request_ids=expected_ids,
    )
    if contract_errors:
        raise ValueError(
            "forced token plan does not match the current workload: "
            + ",".join(contract_errors)
        )
    for sequence_id, item in enumerate(plan_items):
        descriptor = plan_prompt_descriptor(args, item, sequence_id)
        request = indexed[descriptor["logical_request_id"]]
        for key in ("phase", "prompt_id", "prompt_sha256", "prompt_chars"):
            if request.get(key) != descriptor[key]:
                raise ValueError(
                    f"forced token plan request {descriptor['logical_request_id']} mismatches {key}"
                )
        origin_input_ids = int_list(request.get("origin_input_ids"))
        forced_output_ids = int_list(request.get("forced_output_ids"))
        if origin_input_ids is None or forced_output_ids is None:
            raise ValueError(
                f"forced token plan request {descriptor['logical_request_id']} is missing token ids"
            )
    return indexed


def build_normal_request_body(item: Dict[str, Any], args: argparse.Namespace) -> Dict[str, Any]:
    """构造普通/capture 请求体。"""

    body = {
        "text": item["prompt"],
        "sampling_params": sampling_params(args),
    }
    if args.forced_token_mode == "capture":
        body["return_prompt_token_ids"] = True
    return body


def build_replay_request_body(
    request_plan: Dict[str, Any],
    args: argparse.Namespace,
) -> Dict[str, Any]:
    """构造 forced replay 请求体，保持真实 decode 但覆盖 committed output token。"""

    origin_input_ids = int_list(request_plan.get("origin_input_ids")) or []
    forced_output_ids = int_list(request_plan.get("forced_output_ids")) or []
    return {
        "input_ids": origin_input_ids,
        "rid": request_plan["logical_request_id"],
        "return_prompt_token_ids": True,
        "sampling_params": sampling_params(args, max_new_tokens=len(forced_output_ids)),
        "trace_sim_forced_output_ids": forced_output_ids,
    }


def response_json(result: Dict[str, Any]) -> Dict[str, Any] | None:
    """读取 request_generate 内部保留的 JSON response。"""

    decoded = result.get("_response_json")
    return decoded if isinstance(decoded, dict) else None


def public_row(row: Dict[str, Any]) -> Dict[str, Any]:
    """去掉不会写入 workload_report 的内部字段。"""

    return {key: value for key, value in row.items() if not key.startswith("_")}


def write_outputs(
    output_dir: Path,
    rows: List[Dict[str, Any]],
    args: argparse.Namespace,
    forced_token_summary: Dict[str, Any],
) -> None:
    """写出 JSON/JSONL workload report，供 profile manifest 和质量审计引用。"""

    output_dir.mkdir(parents=True, exist_ok=True)
    public_rows = [public_row(row) for row in rows]
    by_phase: Dict[str, List[Dict[str, Any]]] = {phase: [] for phase in PHASE_ORDER}
    for row in public_rows:
        by_phase.setdefault(str(row["phase"]), []).append(row)

    phases = {phase: phase_stats(by_phase.get(phase, [])) for phase in PHASE_ORDER}
    total = phase_stats(public_rows)
    selected = rows_for_selected_latency(public_rows)
    expected_by_phase = {
        phase: expected_mechanisms_for_phase(phase, args)
        for phase in PHASE_ORDER
        if phase != "warmup" and by_phase.get(phase)
    }
    summary = {
        "args": vars(args),
        "workload_id": workload_id(args),
        "workload_mode": forced_token_summary["workload_mode"],
        "workload_args_digest": workload_args_digest(args),
        "forced_token": forced_token_summary,
        "phases": phases,
        "total": total,
        "selected_latency": phase_stats(selected),
        "selected_phases": [
            "reuse_A_after_pressure",
            "prefetch_reuse_C",
            "backup_wait_A",
        ],
        "expected_cache_mechanisms": expected_by_phase,
        "requests": public_rows,
    }
    (output_dir / "workload_report.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    with (output_dir / "workload_report.jsonl").open("w", encoding="utf-8") as fh:
        for row in public_rows:
            fh.write(json.dumps(row, sort_keys=True) + "\n")


def rows_for_selected_latency(rows: List[Dict[str, Any]]) -> List[Dict[str, Any]]:
    """筛选长期对比使用的关键 latency phase。"""

    return [
        row
        for row in rows
        if row.get("phase")
        in {"reuse_A_after_pressure", "prefetch_reuse_C", "backup_wait_A"}
    ]


def base_request_row(
    args: argparse.Namespace,
    item: Dict[str, Any],
    sequence_id: int,
    result: Dict[str, Any],
) -> Dict[str, Any]:
    """构造 workload_report 中每个请求的公共字段。"""

    expected_mechanisms = expected_mechanisms_for_phase(str(item["phase"]), args)
    descriptor = plan_prompt_descriptor(args, item, sequence_id)
    return {
        **descriptor,
        "expected_cache_mechanisms": expected_mechanisms,
        "max_new_tokens": args.max_new_tokens,
        **result,
    }


def capture_plan_request(
    row: Dict[str, Any],
    item: Dict[str, Any],
    args: argparse.Namespace,
) -> Dict[str, Any] | None:
    """从 capture 响应中抽取 forced plan request。"""

    decoded = response_json(row)
    if row.get("status") != "ok" or decoded is None:
        return None
    origin_input_ids = int_list(decoded.get("prompt_token_ids"))
    output_ids = int_list(decoded.get("output_ids"))
    if origin_input_ids is None or output_ids is None:
        row["status"] = "error"
        row["error"] = "MissingForcedTokenCaptureFields"
        row["error_message"] = "capture response must contain prompt_token_ids and output_ids"
        return None

    full_path = origin_input_ids + output_ids
    row.update(
        {
            "origin_input_count": len(origin_input_ids),
            "origin_input_ids_sha256_u32le": token_ids_sha256_u32le(origin_input_ids),
            "actual_output_count": len(output_ids),
            "actual_output_ids_sha256_u32le": token_ids_sha256_u32le(output_ids),
            "actual_full_path_sha256_u32le": token_ids_sha256_u32le(full_path),
        }
    )
    return {
        "logical_request_id": row["logical_request_id"],
        "sequence_id": row["sequence_id"],
        "phase": row["phase"],
        "prompt_id": row["prompt_id"],
        "prompt_chars": row["prompt_chars"],
        "prompt_sha256": row["prompt_sha256"],
        "origin_input_ids": origin_input_ids,
        "origin_input_count": len(origin_input_ids),
        "origin_input_ids_sha256_u32le": row["origin_input_ids_sha256_u32le"],
        "forced_output_ids": output_ids,
        "forced_output_count": len(output_ids),
        "forced_output_ids_sha256_u32le": row["actual_output_ids_sha256_u32le"],
        "full_path_sha256_u32le": row["actual_full_path_sha256_u32le"],
        "expected_mechanisms": expected_mechanisms_for_phase(str(item["phase"]), args),
    }


def apply_replay_checks(row: Dict[str, Any], request_plan: Dict[str, Any]) -> None:
    """把 forced replay 的期望/实际输出校验写入 request row。"""

    expected_output_ids = int_list(request_plan.get("forced_output_ids")) or []
    origin_input_ids = int_list(request_plan.get("origin_input_ids")) or []
    decoded = response_json(row)
    actual_output_ids = (
        int_list(decoded.get("output_ids")) if decoded is not None else None
    )
    actual_prompt_ids = (
        int_list(decoded.get("prompt_token_ids")) if decoded is not None else None
    )
    row.update(
        {
            "origin_input_count": len(origin_input_ids),
            "origin_input_ids_sha256_u32le": token_ids_sha256_u32le(origin_input_ids),
            "max_new_tokens": len(expected_output_ids),
            "forced_output_count": len(expected_output_ids),
            "forced_output_ids_sha256_u32le": token_ids_sha256_u32le(
                expected_output_ids
            ),
            "forced_full_path_sha256_u32le": token_ids_sha256_u32le(
                origin_input_ids + expected_output_ids
            ),
            "forced_token_output_checked": actual_output_ids is not None,
            "actual_output_matches_forced": actual_output_ids == expected_output_ids,
            "actual_prompt_matches_plan": (
                actual_prompt_ids == origin_input_ids
                if actual_prompt_ids is not None
                else False
            ),
        }
    )
    if actual_output_ids is not None:
        row.update(
            {
                "actual_output_count": len(actual_output_ids),
                "actual_output_ids_sha256_u32le": token_ids_sha256_u32le(
                    actual_output_ids
                ),
                "actual_full_path_sha256_u32le": token_ids_sha256_u32le(
                    origin_input_ids + actual_output_ids
                ),
            }
        )
    if row.get("status") == "ok" and actual_output_ids is None:
        row["error"] = "ForcedTokenReplayOutputUnchecked"
        row["error_message"] = "replay response must contain output_ids"


def build_forced_token_plan(
    args: argparse.Namespace,
    plan_requests: List[Dict[str, Any]],
) -> Dict[str, Any]:
    """构造 capture 输出的 forced_token_plan.json。"""

    current_workload_id = workload_id(args)
    current_workload_fingerprint = workload_args_digest(args)
    return {
        "schema": FORCED_TOKEN_PLAN_SCHEMA,
        "created_at": datetime.now(timezone.utc).astimezone().isoformat(),
        "workload_id": current_workload_id,
        "workload_fingerprint": current_workload_fingerprint,
        "capture": capture_provenance(args),
        "workload": {
            "input_id": current_workload_id,
            "script": "scripts/bench/hicache_phased_workload.py",
            "script_args_digest": current_workload_fingerprint,
            "request_count": len(plan_requests),
        },
        "requests": plan_requests,
    }


def capture_provenance(args: argparse.Namespace) -> Dict[str, Any]:
    """从 runner 传入的环境变量中提取 capture 审计信息。"""

    tokenizer_summary = tokenizer_digest_for_model_path(
        os.environ.get("TRACE_SIM_PROFILE_MODEL_PATH")
    )
    return {
        "source": "scripts/bench/hicache_phased_workload.py",
        "profile_manifest_path": os.environ.get("TRACE_SIM_PROFILE_MANIFEST_PATH"),
        "run_id": os.environ.get("TRACE_SIM_PROFILE_RUN_ID"),
        "run_dir": os.environ.get("TRACE_SIM_PROFILE_RUN_DIR"),
        "config_id": os.environ.get("TRACE_SIM_PROFILE_CONFIG_ID"),
        "input_id": os.environ.get("TRACE_SIM_PROFILE_INPUT_ID") or workload_id(args),
        "model_path": os.environ.get("TRACE_SIM_PROFILE_MODEL_PATH"),
        "tokenizer_digest": tokenizer_summary["digest"],
        "tokenizer_digest_files": tokenizer_summary["files"],
    }


def tokenizer_digest_for_model_path(model_path_value: str | None) -> Dict[str, Any]:
    """对 tokenizer 相关文件生成 bundle digest。"""

    if not model_path_value:
        return {"digest": None, "files": []}
    model_path = Path(model_path_value).expanduser()
    if not model_path.is_dir():
        return {"digest": None, "files": []}

    candidate_names = (
        "tokenizer.json",
        "tokenizer.model",
        "tokenizer_config.json",
        "special_tokens_map.json",
        "generation_config.json",
        "vocab.json",
        "vocab.txt",
        "merges.txt",
        "added_tokens.json",
    )
    files = [model_path / name for name in candidate_names if (model_path / name).is_file()]
    if not files:
        return {"digest": None, "files": []}

    hasher = hashlib.sha256()
    file_summaries: List[Dict[str, Any]] = []
    for path in sorted(files, key=lambda item: item.name):
        file_digest = sha256_file(path)
        file_summaries.append(
            {
                "name": path.name,
                "bytes": path.stat().st_size,
                "sha256": file_digest,
            }
        )
        hasher.update(path.name.encode("utf-8"))
        hasher.update(b"\0")
        hasher.update(file_digest.encode("ascii"))
        hasher.update(b"\0")
    return {
        "digest": "sha256_tokenizer_bundle:" + hasher.hexdigest(),
        "files": file_summaries,
    }


def default_forced_token_summary(args: argparse.Namespace) -> Dict[str, Any]:
    """创建 workload_report 中的 forced_token 汇总骨架。"""

    mode = str(args.forced_token_mode)
    return {
        "enabled": mode != "none",
        "mode": mode,
        "ready": mode == "none",
        "workload_mode": "normal" if mode == "none" else f"forced_token_{mode}",
        "workload_id": workload_id(args),
        "workload_fingerprint": workload_args_digest(args),
        "plan_path": str(args.forced_token_plan) if args.forced_token_plan else None,
        "plan_schema": FORCED_TOKEN_PLAN_SCHEMA if mode != "none" else None,
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
        "all_actual_outputs_match_plan": False if mode == "replay" else None,
        "plan_written": False if mode == "capture" else None,
        "bundle_path": os.environ.get("TRACE_SIM_FORCED_TOKEN_BUNDLE_PATH"),
        "bundle_schema": os.environ.get("TRACE_SIM_FORCED_TOKEN_BUNDLE_SCHEMA"),
        "bundle_sha256": os.environ.get("TRACE_SIM_FORCED_TOKEN_BUNDLE_SHA256"),
        "bundle_id": os.environ.get("TRACE_SIM_FORCED_TOKEN_BUNDLE_ID"),
        "bundle_plan_sha256": os.environ.get("TRACE_SIM_FORCED_TOKEN_BUNDLE_PLAN_SHA256"),
    }


def finalize_forced_token_summary(
    summary: Dict[str, Any],
    rows: List[Dict[str, Any]],
    plan_path: Path | None,
) -> Dict[str, Any]:
    """根据实际请求行和 plan 文件补全 forced_token 汇总。"""

    mode = str(summary.get("mode") or "none")
    if mode == "none":
        return summary
    summary["request_count"] = len(rows)
    if plan_path is not None and plan_path.is_file():
        plan_summary = forced_token_plan_summary(plan_path)
        summary["plan_path"] = str(plan_path)
        summary["plan_sha256"] = plan_summary.sha256 or sha256_file(plan_path)
        summary["plan_workload_id"] = plan_summary.workload_id
        summary["plan_workload_fingerprint"] = plan_summary.workload_fingerprint
        summary["plan_capture"] = plan_summary.capture
        summary["plan_capture_run_id"] = plan_summary.capture.get("run_id")
        summary["plan_capture_config_id"] = plan_summary.capture.get("config_id")
        summary["plan_capture_input_id"] = plan_summary.capture.get("input_id")
        summary["plan_capture_model_path"] = plan_summary.capture.get("model_path")
        summary["plan_capture_tokenizer_digest"] = plan_summary.capture.get("tokenizer_digest")
        if mode == "capture":
            summary["plan_written"] = True
    if mode == "replay":
        output_checked = [row for row in rows if row.get("forced_token_output_checked")]
        mismatches = [
            row for row in rows if row.get("actual_output_matches_forced") is False
        ]
        unchecked = [row for row in rows if not row.get("forced_token_output_checked")]
        prompt_mismatches = [
            row for row in rows if row.get("actual_prompt_matches_plan") is False
        ]
        summary.update(
            {
                "output_checked_count": len(output_checked),
                "mismatch_count": len(mismatches),
                "unchecked_count": len(unchecked),
                "prompt_mismatch_count": len(prompt_mismatches),
                "all_actual_outputs_match_plan": (
                    bool(rows)
                    and not mismatches
                    and not unchecked
                    and not prompt_mismatches
                ),
            }
        )
    if mode == "capture":
        summary["ready"] = bool(summary.get("plan_written"))
    elif mode == "replay":
        bundle_ready = (
            bool(summary.get("bundle_path"))
            and summary.get("bundle_schema") == FORCED_TOKEN_BUNDLE_SCHEMA
            and bool(summary.get("bundle_sha256"))
            and bool(summary.get("bundle_id"))
            and summary.get("bundle_plan_sha256") == summary.get("plan_sha256")
        )
        summary["ready"] = bool(summary.get("all_actual_outputs_match_plan")) and bundle_ready
    return summary


def workload_success(
    rows: List[Dict[str, Any]],
    forced_token_summary: Dict[str, Any],
) -> bool:
    """判断 workload 是否成功完成。"""

    if not rows or any(row.get("status") != "ok" for row in rows):
        return False
    if forced_token_summary.get("mode") == "capture":
        return bool(forced_token_summary.get("plan_written"))
    if forced_token_summary.get("mode") == "replay":
        return bool(forced_token_summary.get("ready"))
    return True


def build_arg_parser() -> argparse.ArgumentParser:
    """构造 workload CLI parser，供 runner preflight 复用。"""

    parser = argparse.ArgumentParser(
        description="Run deterministic phased requests to exercise SGLang HiCache movement paths."
    )
    parser.add_argument(
        "--base-url",
        default="http://127.0.0.1:30001",
        help="SGLang server base URL.",
    )
    parser.add_argument(
        "--output-dir",
        required=True,
        help="Directory that will receive workload_report.json/jsonl.",
    )
    parser.add_argument(
        "--workload-id",
        help="Stable logical workload id used in forced token plans.",
    )
    parser.add_argument(
        "--forced-token-mode",
        default="none",
        choices=("none", "capture", "replay"),
        help="Capture or replay a Trace-Sim forced token plan.",
    )
    parser.add_argument(
        "--forced-token-plan",
        help="Path to forced_token_plan.json. Capture defaults to <output-dir>/forced_token_plan.json.",
    )
    parser.add_argument("--warmup-requests", type=int, default=1)
    parser.add_argument("--seed-requests", type=int, default=8)
    parser.add_argument("--reuse-requests", type=int, default=8)
    parser.add_argument("--backup-wait-requests", type=int, default=8)
    parser.add_argument("--pressure-requests", type=int, default=48)
    parser.add_argument("--reuse-after-pressure-requests", type=int, default=8)
    parser.add_argument("--prefetch-seed-requests", type=int, default=4)
    parser.add_argument("--prefetch-reuse-requests", type=int, default=8)
    parser.add_argument("--dirty-eviction-requests", type=int, default=0)
    parser.add_argument(
        "--cache-write-policy",
        default="write_through",
        choices=("write_through", "write_through_selective", "write_back"),
        help="Cache write policy used only to describe expected mechanisms in workload_report.",
    )
    parser.add_argument("--pressure-unique-prefix", action="store_true")
    parser.add_argument("--shared-prefix-repeat", type=int, default=96)
    parser.add_argument("--unique-suffix-repeat", type=int, default=8)
    parser.add_argument("--max-new-tokens", type=int, default=8)
    parser.add_argument(
        "--sampling-seed",
        type=int,
        default=20260623,
        help="Fixed per-request sampling seed for determinism checks.",
    )
    parser.add_argument(
        "--top-p",
        type=float,
        default=1.0,
        help="Explicit top_p for deterministic greedy requests.",
    )
    parser.add_argument(
        "--top-k",
        type=int,
        default=1,
        help="Explicit top_k=1 keeps /generate in greedy mode.",
    )
    parser.add_argument("--timeout-sec", type=int, default=600)
    parser.add_argument("--sleep-sec", type=float, default=0.0)
    parser.add_argument("--phase-wait-sec", type=float, default=0.0)
    parser.add_argument(
        "--max-errors",
        type=int,
        default=1,
        help="Stop after this many request errors; 0 disables early stop.",
    )
    return parser


def main() -> int:
    """CLI 入口：执行请求计划并写出 workload report。"""

    parser = build_arg_parser()
    args = parser.parse_args()

    output_dir = Path(args.output_dir)
    plan_path = Path(args.forced_token_plan) if args.forced_token_plan else None
    if args.forced_token_mode == "capture" and plan_path is None:
        plan_path = output_dir / "forced_token_plan.json"
    if args.forced_token_mode == "replay" and plan_path is None:
        parser.error("--forced-token-plan is required when --forced-token-mode=replay")
    if args.forced_token_mode == "capture" and plan_path is not None and plan_path.exists():
        parser.error(f"forced token capture plan already exists: {plan_path}; capture artifacts are immutable")

    url = args.base_url.rstrip("/") + "/generate"
    rows: List[Dict[str, Any]] = []
    plan = build_plan(args)
    forced_plan: Dict[str, Any] | None = None
    replay_by_request: Dict[str, Dict[str, Any]] = {}
    if args.forced_token_mode == "replay":
        assert plan_path is not None
        try:
            forced_plan = load_forced_token_plan(plan_path)
            replay_by_request = validate_forced_plan_matches_workload(
                args,
                plan,
                forced_plan,
            )
        except (OSError, ValueError, json.JSONDecodeError) as exc:
            parser.error(str(exc))

    forced_token_summary = default_forced_token_summary(args)
    plan_requests: List[Dict[str, Any]] = []
    for sequence_id, item in enumerate(plan):
        logical_id = logical_request_id(args, item, sequence_id)
        if args.forced_token_mode == "replay":
            request_plan = replay_by_request[logical_id]
            body = build_replay_request_body(request_plan, args)
        else:
            request_plan = {}
            body = build_normal_request_body(item, args)

        result = request_generate(url, body, args)
        row = base_request_row(args, item, sequence_id, result)
        if args.forced_token_mode == "capture":
            plan_request = capture_plan_request(row, item, args)
            if plan_request is not None:
                plan_requests.append(plan_request)
        elif args.forced_token_mode == "replay":
            apply_replay_checks(row, request_plan)
        rows.append(row)
        print(
            f"phase={row['phase']} seq={sequence_id} prompt_id={row['prompt_id']} "
            f"status={row['status']} latency_ms={row['latency_ms']:.3f}",
            flush=True,
        )
        if (
            args.max_errors > 0
            and sum(1 for item in rows if item.get("status") != "ok")
            >= args.max_errors
        ):
            print(
                f"abort_after_error_count={args.max_errors} phase={row['phase']} seq={sequence_id}",
                flush=True,
            )
            break
        if args.sleep_sec > 0:
            time.sleep(args.sleep_sec)
        next_item = plan[sequence_id + 1] if sequence_id + 1 < len(plan) else None
        if (
            next_item is not None
            and next_item.get("phase") != item.get("phase")
            and args.phase_wait_sec > 0
        ):
            time.sleep(args.phase_wait_sec)

    if (
        args.forced_token_mode == "capture"
        and plan_path is not None
        and rows
        and len(plan_requests) == len(rows)
        and all(row.get("status") == "ok" for row in rows)
    ):
        forced_plan = build_forced_token_plan(args, plan_requests)
        plan_path.parent.mkdir(parents=True, exist_ok=True)
        plan_path.write_text(
            json.dumps(forced_plan, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )

    forced_token_summary = finalize_forced_token_summary(
        forced_token_summary,
        rows,
        plan_path,
    )
    write_outputs(output_dir, rows, args, forced_token_summary)
    return 0 if workload_success(rows, forced_token_summary) else 1


if __name__ == "__main__":
    raise SystemExit(main())
