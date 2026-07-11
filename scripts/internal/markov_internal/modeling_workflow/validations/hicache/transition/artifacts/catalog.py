"""Artifact generation for the HiCache transition mismatch catalog.

This module aggregates classifications already produced by comparison into
JSON, Markdown, and bounded family samples. It never reinterprets model inputs
or oracle evidence.
"""

from __future__ import annotations

import collections
from pathlib import Path
from typing import Any

from markov_internal.common.io import write_json
from markov_internal.common.naming import safe_slug
from ..exactness.taxonomy_aggregation import aggregate_transition_families


def build_transition_mismatch_catalog_from_entries(
    artifact_root: Path,
    prediction_entries: list[dict[str, Any]],
    *,
    source_summary_path: str,
    sample_limit: int,
) -> dict[str, Any]:
    """Aggregate comparison classifications into a mismatch catalog."""

    families = aggregate_transition_families(prediction_entries, sample_limit=sample_limit)
    unresolved = [entry for entry in prediction_entries if entry.get("family") == "unresolved_transition_mismatch"]
    return {
        "schema": "trace_sim.hicache.transition_mismatch_catalog.v1",
        "artifact_root": str(artifact_root),
        "source_transition_summary_path": source_summary_path,
        "prediction_count": len(prediction_entries),
        "ready_count": sum(1 for entry in prediction_entries if entry.get("ready")),
        "exact_count": sum(1 for entry in prediction_entries if entry.get("exact")),
        "final_state_exact_count": sum(1 for entry in prediction_entries if entry.get("final_state_exact")),
        "transition_count_exact_count": sum(1 for entry in prediction_entries if entry.get("transition_count_exact")),
        "page_lifecycle_multiset_exact_count": sum(
            1 for entry in prediction_entries if entry.get("page_lifecycle_multiset_exact")
        ),
        "classified_prediction_count": sum(
            1 for entry in prediction_entries if entry.get("family") != "unresolved_transition_mismatch"
        ),
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
                bool(family.get("mechanism_review", {}).get("recommended_fix")) for family in families.values()
            ),
            "dag_patch_allowed": False,
        },
        "family_counts": dict(
            sorted(collections.Counter(str(entry.get("family") or "") for entry in prediction_entries).items())
        ),
        "classification_counts": dict(
            sorted(collections.Counter(str(entry.get("classification") or "") for entry in prediction_entries).items())
        ),
        "patch_risk_counts": dict(
            sorted(collections.Counter(str(entry.get("patch_risk") or "") for entry in prediction_entries).items())
        ),
        "families": families,
        "predictions": prediction_entries,
        "notes": [
            "This catalog summarizes classifications already produced during transition exactness compare.",
            "Physical-candidate families still require source attribution before DAG patching.",
            "DAG patching must use the patch gate fields; raw transitions are not patch actions.",
        ],
    }


def write_transition_catalog_outputs(output_path: Path, catalog: dict[str, Any], *, sample_limit: int) -> None:
    """Write catalog JSON, Markdown, and bounded per-family samples."""

    write_json(output_path, catalog)
    write_transition_catalog_markdown(output_path.with_suffix(".md"), catalog)
    samples_dir = output_path.parent / "family_samples"
    samples_dir.mkdir(parents=True, exist_ok=True)
    expected_paths = {
        samples_dir / f"{safe_slug(str(family))}.json"
        for family, item in catalog.get("families", {}).items()
        if isinstance(item, dict)
    }
    for stale_path in samples_dir.glob("*.json"):
        if stale_path not in expected_paths:
            stale_path.unlink()
    for family, item in catalog.get("families", {}).items():
        if not isinstance(item, dict):
            continue
        write_json(
            samples_dir / f"{safe_slug(str(family))}.json",
            family_sample_payload(item, catalog, sample_limit=sample_limit),
        )


def write_transition_catalog_markdown(path: Path, catalog: dict[str, Any]) -> None:
    """Write the human-readable transition-catalog summary."""

    lines = [
        "# HiCache Transition Mismatch Catalog",
        "",
        "Status: generated diagnostic artifact by the unified modeling workflow transition exactness validation.",
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
        lines.append("")
    path.write_text("\n".join(lines).rstrip() + "\n", encoding="utf-8")


def family_sample_payload(family_item: dict[str, Any], catalog: dict[str, Any], *, sample_limit: int) -> dict[str, Any]:
    """Build the bounded sample artifact for one transition family."""

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
