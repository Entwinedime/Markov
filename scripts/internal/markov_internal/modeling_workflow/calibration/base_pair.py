"""Admission for paired light/profiled measurements of the same base."""

from collections import Counter

from ...contracts.forced_token.quality import forced_token_quality_from_report


def validate_base_pair(configs: tuple[dict, dict], reports: tuple[dict, dict], plans: tuple[dict, dict]) -> None:
    """Exclude measurement/output settings, not inference or workload settings."""

    def runtime(config):
        result = {
            key: value
            for key, value in config.items()
            if key not in {"name", "id", "run_id", "run_root", "metadata", "profiling"}
        }
        result["env"] = {
            key: value
            for key, value in config.get("env", {}).items()
            if key not in {"SGLANG_STEP_TIMING_DIR", "SGLANG_HICACHE_IO_TIMING"}
        }
        # Restart limits govern acquisition attempts, not successful inference.
        result["server"] = {key: value for key, value in result["server"].items() if key != "startup_max_attempts"}
        return result

    if runtime(configs[0]) != runtime(configs[1]):
        raise ValueError("paired base captures have different runtime configurations")
    if not plans[0] or plans[0] != plans[1]:
        raise ValueError("paired base captures require the same nonempty forced-token plan")
    if reports[0]["workload_id"] != reports[1]["workload_id"]:
        raise ValueError("paired base captures have different workload identities")

    signatures = []
    for report in reports:
        quality = forced_token_quality_from_report(report)
        if not quality["enabled"] or quality["mode"] != "replay" or not quality["ready"]:
            raise ValueError("base CPU calibration requires verified forced-token replay")
        requests = [row for row in report["requests"] if row.get("kind") == "request"]
        if not requests or any(
            row.get("http_status") != 200
            or row.get("actual_prompt_matches_plan") is not True
            or row.get("actual_output_matches_forced") is not True
            for row in requests
        ):
            raise ValueError("base CPU calibration requests did not match their token plan")
        fields = (
            "logical_request_id",
            "measure",
            "actual_prompt_token_count",
            "actual_output_count",
            "forced_output_count",
            "origin_input_count",
            "anchor_tokens",
            "tail_tokens",
        )
        signatures.append([tuple(row[key] for key in fields) for row in requests])
    if signatures[0] != signatures[1]:
        raise ValueError("paired base captures have different request order or token counts")


def validate_forward_work(light: list[dict], profiled: list[dict]) -> None:
    """Compare already-bound step shapes and counts, independent of process IDs/timing."""

    def work(rows):
        return Counter(
            (row["request_id"], row["identity"]["tp_rank"], row["identity"].get("pp_rank", 0), row["name"])
            for row in rows
        )

    if work(light) != work(profiled):
        raise ValueError("paired forward captures have different step work or rank coverage")
