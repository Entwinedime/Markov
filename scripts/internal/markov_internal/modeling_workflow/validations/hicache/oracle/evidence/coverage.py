"""predicted transition 与 state snapshot 之间的覆盖率汇总。"""

from __future__ import annotations

from typing import Any

from ..snapshot.records import count_records_by_key, page_set_from_predicted_record
from ..snapshot.state import derived_hicache_state_from_snapshot


def build_request_transition_coverage(
    predicted_records: list[dict[str, Any]], snapshots: list[dict[str, Any]]
) -> dict[str, Any]:
    """生成 request 级 state trace 覆盖摘要。

    真实 state snapshot 目前不是严格的一次 transition oracle；它更像调用点快照。
    因此这里先做 request id 覆盖检查，帮助后续把 final set mismatch 下钻到请求。
    缺少 request_id 的 snapshot 不参与 request 级覆盖判定。
    """

    predicted_requests = sorted(
        {str(row.get("request_id")) for row in predicted_records if str(row.get("request_id") or "")}
    )
    oracle_requests = sorted({str(row.get("request_id")) for row in snapshots if str(row.get("request_id") or "")})
    predicted_set = set(predicted_requests)
    oracle_set = set(oracle_requests)
    return {
        "ready": bool(predicted_requests or oracle_requests),
        "predicted_request_count": len(predicted_requests),
        "oracle_request_count": len(oracle_requests),
        "requests_missing_oracle_snapshot": sorted(predicted_set - oracle_set),
        "requests_missing_predicted_transition": sorted(oracle_set - predicted_set),
        "predicted_transition_count_by_request": count_records_by_key(predicted_records, "request_id"),
        "oracle_snapshot_count_by_request": count_records_by_key(snapshots, "request_id"),
    }


def build_transition_coverage(
    predicted_records: list[dict[str, Any]], snapshots: list[dict[str, Any]]
) -> dict[str, Any]:
    """生成 transition 级覆盖摘要。

    这里不把 coverage 作为验证通过条件。state snapshot 是稀疏观测，不是逐步
    transition oracle；该摘要用于定位“哪些 transition / page / request 缺少解释”。
    """

    predicted_pages = sorted(
        {
            str(page)
            for record in predicted_records
            for page in page_set_from_predicted_record(record)
            if page is not None
        }
    )
    oracle_pages = sorted(
        {
            str(page)
            for snapshot in snapshots
            for pages in derived_hicache_state_from_snapshot(snapshot.get("state_snapshot", {})).values()
            if isinstance(pages, list)
            for page in pages
            if page is not None
        }
    )
    predicted_page_set = set(predicted_pages)
    oracle_page_set = set(oracle_pages)
    return {
        "ready": bool(predicted_records),
        "predicted_transition_count": len(predicted_records),
        "predicted_transition_count_by_kind": count_records_by_key(predicted_records, "transition_kind"),
        "predicted_operation_count_by_kind": count_records_by_key(predicted_records, "predicted_operation_kind"),
        "predicted_transition_count_by_source_event": count_records_by_key(predicted_records, "source_event_name"),
        "oracle_snapshot_count_by_target": count_records_by_key(snapshots, "target_id"),
        "predicted_page_count": len(predicted_pages),
        "oracle_page_count": len(oracle_pages),
        "pages_missing_predicted_transition": sorted(oracle_page_set - predicted_page_set),
        "pages_without_oracle_snapshot_evidence": sorted(predicted_page_set - oracle_page_set),
    }
