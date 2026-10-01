"""Render measured base replays without changing model or workload inputs."""

from __future__ import annotations

from pathlib import Path

from ...common.io import load_json
from ...common.manifest import profile_labels, workload_report_path
from ...common.paths import CONTAINER_REPO_PREFIXES, repo_relative_path, require_repo_path
from ...contracts.forced_token.quality import forced_token_quality_from_report
from ...contracts.forced_token.bundle import resolve_forced_token_bundle_plan
from ...workload_template.cli import parse_workload_command


def light_capture_config(profile_manifest: Path, output_dir: Path, bundle: Path | None = None) -> dict:
    """Replay the base's recorded token plan into isolated, lightweight outputs.

    An optional bundle must describe the same token work; it never replaces
    the base's input authority. No server is started by this preparation step.
    """
    profile_manifest = require_repo_path(profile_manifest)
    output_dir = require_repo_path(output_dir)
    manifest = load_json(profile_manifest)
    if manifest["status"] != "completed" or manifest.get("dry_run") or manifest.get("collection_errors"):
        raise ValueError("Light replay requires a successful complete base capture")

    config = load_json(require_repo_path(manifest["config_path"]))
    report = load_json(workload_report_path(manifest))
    workload = report["workload_id"]
    quality = forced_token_quality_from_report(report)
    if not quality["ready"] or quality["mode"] != "replay":
        raise ValueError("Light replay requires a verified forced-token base")

    args = parse_workload_command(config["bench"]["command"])
    # A resolved old output path would overwrite the source report on replay.
    if args is None or not args.output_dir.startswith("{bench_dir}/"):
        raise ValueError("Light replay requires a bench output relative to its new run")
    if args.forced_token_mode != "replay" or not args.forced_token_plan:
        raise ValueError("Base bench command does not preserve forced-token replay")

    metadata = config.setdefault("metadata", {})
    _, input_id = profile_labels(config, workload)
    source_plan = require_repo_path(report["forced_token"]["plan_path"])
    if bundle is not None:
        selected_plan = resolve_forced_token_bundle_plan(require_repo_path(bundle), input_id)
        if load_json(selected_plan) != load_json(source_plan):
            raise ValueError("Light replay bundle differs from the original base token plan")

    ld = config.get("profiling", {}).get("ld_preload", {})
    if not ld.get("enabled"):
        raise ValueError("Light replay requires the base LD timing channel")
    config["run_root"] = str(repo_relative_path(output_dir / "profiles"))
    # Capture names change below; preserve the source's user-facing identities.
    metadata.setdefault(
        "config_id", config.get("name") or manifest.get("run_id") or str(repo_relative_path(profile_manifest.parent))
    )
    metadata.setdefault("workload_id", input_id)
    timing = config.get("env", {}).get("SGLANG_STEP_TIMING_DIR")
    if not timing or not any(require_repo_path(timing).glob("*.jsonl")):
        raise ValueError("Base has no step CPU timings; collect a timed base before its light replay")
    if str(config.get("env", {}).get("HOOK_SYNC_THREAD_CPU")) != "1":
        raise ValueError("Base synchronization CPU timing was not enabled")

    timing_dir = repo_relative_path(require_repo_path(output_dir) / "timing")
    config["name"] = "base_cpu_light"
    config["run_id"] = "light"
    # The outer capture ledger budgets one server start per attempt.
    config["server"]["startup_max_attempts"] = 1
    config["profiling"].update(enabled=True, channels=["ld_preload"])
    config["env"]["SGLANG_STEP_TIMING_DIR"] = str(Path(CONTAINER_REPO_PREFIXES[0]) / timing_dir)
    # The existing light replay also measures prefetch service; no extra run.
    config["env"]["SGLANG_HICACHE_IO_TIMING"] = "1"
    return config
