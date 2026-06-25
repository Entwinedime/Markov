#!/usr/bin/env python3
"""HiCache transition mismatch catalog 产物生成。

本模块只负责把 compare 阶段已经生成的分类结果聚合成 JSON、Markdown 和
family sample 文件，不重新解释模型输入或 oracle。
"""

from __future__ import annotations

import collections
import json
import re
from pathlib import Path
from typing import Any

from hicache_transition_taxonomy import aggregate_transition_families


def write_json(path: Path, value: Any) -> None:
    """写出稳定 JSON 文件。"""

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def safe_slug(value: str) -> str:
    """把 family id 转成可用作文件名的 slug。"""

    return re.sub(r"[^A-Za-z0-9_.-]+", "_", value.strip()).strip("._") or "unknown"


def build_transition_mismatch_catalog_from_entries(
    matrix_dir: Path,
    prediction_entries: list[dict[str, Any]],
    *,
    source_matrix_path: str,
    sample_limit: int,
) -> dict[str, Any]:
    """从 compare 已生成的分类 entries 聚合 transition mismatch catalog。"""

    families = aggregate_transition_families(prediction_entries, sample_limit=sample_limit)
    unresolved = [entry for entry in prediction_entries if entry.get("family") == "unresolved_transition_mismatch"]
    return {
        "schema": "trace_sim.hicache.transition_mismatch_catalog.v1",
        "matrix_dir": str(matrix_dir),
        "source_transition_matrix_path": source_matrix_path,
        "prediction_count": len(prediction_entries),
        "ready_count": sum(1 for entry in prediction_entries if entry.get("ready")),
        "exact_count": sum(1 for entry in prediction_entries if entry.get("exact")),
        "final_state_exact_count": sum(1 for entry in prediction_entries if entry.get("final_state_exact")),
        "transition_count_exact_count": sum(1 for entry in prediction_entries if entry.get("transition_count_exact")),
        "page_lifecycle_multiset_exact_count": sum(1 for entry in prediction_entries if entry.get("page_lifecycle_multiset_exact")),
        "classified_prediction_count": sum(1 for entry in prediction_entries if entry.get("family") != "unresolved_transition_mismatch"),
        "unresolved_prediction_count": len(unresolved),
        "unresolved_predictions": [
            {
                "label": entry.get("label"),
                "target_config_id": entry.get("target_config_id"),
                "input_id": entry.get("input_id"),
                "reason": entry.get("classification_reason"),
                "mismatch_kinds": entry.get("mismatch_kinds"),
                "sample_mismatches": entry.get("sample_mismatches", [])[:sample_limit],
            }
            for entry in unresolved[:sample_limit]
        ],
        "readiness_gates": {
            "transition_classification_ready": bool(prediction_entries) and not unresolved,
            "transition_family_review_ready": all(
                bool(family.get("mechanism_review", {}).get("recommended_fix"))
                for family in families.values()
            ),
            "dag_patch_allowed": False,
        },
        "family_counts": dict(sorted(collections.Counter(str(entry.get("family") or "") for entry in prediction_entries).items())),
        "classification_counts": dict(sorted(collections.Counter(str(entry.get("classification") or "") for entry in prediction_entries).items())),
        "patch_risk_counts": dict(sorted(collections.Counter(str(entry.get("patch_risk") or "") for entry in prediction_entries).items())),
        "families": families,
        "predictions": prediction_entries,
        "notes": [
            "This catalog summarizes classifications already produced during transition exactness compare.",
            "Physical-candidate families still require source attribution before DAG patching.",
            "DAG patching must use the patch gate fields; raw transitions are not patch actions.",
        ],
    }


def write_transition_catalog_outputs(matrix_dir: Path, output_path: Path, catalog: dict[str, Any], *, sample_limit: int) -> None:
    """写出阶段一/二 catalog JSON、Markdown 和 family samples。"""

    write_json(output_path, catalog)
    write_transition_catalog_markdown(output_path.with_suffix(".md"), catalog)
    samples_dir = matrix_dir / "transition_family_samples"
    samples_dir.mkdir(parents=True, exist_ok=True)
    for family, item in catalog.get("families", {}).items():
        if not isinstance(item, dict):
            continue
        write_json(samples_dir / f"{safe_slug(str(family))}.json", family_sample_payload(item, catalog, sample_limit=sample_limit))


def write_transition_catalog_markdown(path: Path, catalog: dict[str, Any]) -> None:
    """写出人读的 transition catalog 摘要。"""

    lines = [
        "# HiCache Transition Mismatch Catalog",
        "",
        "Status: generated diagnostic artifact. Produced by `hicache_transition_exactness.py --mode compare-matrix --emit-catalog`.",
        "",
        "## Summary",
        "",
        f"- prediction_count: `{catalog.get('prediction_count')}`",
        f"- ready_count: `{catalog.get('ready_count')}`",
        f"- exact_count: `{catalog.get('exact_count')}`",
        f"- final_state_exact_count: `{catalog.get('final_state_exact_count')}`",
        f"- transition_count_exact_count: `{catalog.get('transition_count_exact_count')}`",
        f"- page_lifecycle_multiset_exact_count: `{catalog.get('page_lifecycle_multiset_exact_count')}`",
        f"- unresolved_prediction_count: `{catalog.get('unresolved_prediction_count')}`",
        f"- dag_patch_allowed: `{catalog.get('readiness_gates', {}).get('dag_patch_allowed')}`",
        "",
        "## Families",
        "",
        "| family | predictions | classification | status | patch risk | gate action | target configs | inputs |",
        "| --- | ---: | --- | --- | --- | --- | --- | --- |",
    ]
    for family, item in catalog.get("families", {}).items():
        if not isinstance(item, dict):
            continue
        lines.append(
            "| "
            + " | ".join(
                [
                    str(family),
                    str(item.get("prediction_count")),
                    str(item.get("classification")),
                    str(item.get("status")),
                    str(item.get("patch_risk")),
                    str(item.get("patch_filter_action")),
                    ", ".join(str(v) for v in item.get("target_config_ids", [])),
                    ", ".join(str(v) for v in item.get("input_ids", [])),
                ]
            )
            + " |"
        )
    lines.extend(["", "## Mechanism Review", ""])
    for family, item in catalog.get("families", {}).items():
        if not isinstance(item, dict):
            continue
        review = item.get("mechanism_review", {}) if isinstance(item.get("mechanism_review"), dict) else {}
        lines.extend(
            [
                f"### {family}",
                "",
                f"- status: `{review.get('status')}`",
                f"- patch_risk: `{review.get('patch_risk')}`",
                f"- patch_filter_action: `{item.get('patch_filter_action')}`",
                f"- source_attribution_required: `{item.get('source_attribution_required')}`",
                f"- duration_required: `{item.get('duration_required')}`",
                f"- evidence_required: {', '.join(str(v) for v in item.get('evidence_required', []))}",
                f"- observed_evidence: {review.get('observed_evidence', '')}",
                f"- recommended_fix: {review.get('recommended_fix', '')}",
            ]
        )
        anchors = review.get("sglang_source_anchor", [])
        if anchors:
            lines.append("- sglang_source_anchor:")
            lines.extend(f"  - `{anchor}`" for anchor in anchors)
        call_sites = review.get("model_call_site", [])
        if call_sites:
            lines.append("- model_call_site:")
            lines.extend(f"  - `{call_site}`" for call_site in call_sites)
        if review.get("does_not_block_patch_reason"):
            lines.append(f"- does_not_block_patch_reason: {review.get('does_not_block_patch_reason')}")
        if review.get("physical_fix_plan"):
            lines.append(f"- physical_fix_plan: {review.get('physical_fix_plan')}")
        lines.append("")
    path.write_text("\n".join(lines).rstrip() + "\n", encoding="utf-8")


def family_sample_payload(family_item: dict[str, Any], catalog: dict[str, Any], *, sample_limit: int) -> dict[str, Any]:
    """构建单个 family sample JSON。"""

    family = str(family_item.get("family") or "")
    predictions = [
        entry
        for entry in catalog.get("predictions", [])
        if isinstance(entry, dict) and str(entry.get("family") or "") == family
    ]
    return {
        "schema": "trace_sim.hicache.transition_family_samples.v1",
        "family": family,
        "classification": family_item.get("classification"),
        "status": family_item.get("status"),
        "patch_risk": family_item.get("patch_risk"),
        "mechanism_review": family_item.get("mechanism_review", {}),
        "prediction_count": len(predictions),
        "sample_predictions": predictions[:sample_limit],
    }
