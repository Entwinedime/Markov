"""SGLang torch-profiler API helpers."""

from __future__ import annotations

from typing import Any

from ..common.io import write_json
from ..common.logging import log
from ..common.process import api_base_from_ready_url, post_json
from .runtime import RunLayout, build_profile_body, channel_config


def profiler_api_base(server_cfg: dict[str, Any], profile_cfg: dict[str, Any]) -> str:
    """Return the profiler API origin, defaulting to the readiness URL origin."""

    ready_url = server_cfg.get("ready_url", "http://127.0.0.1:30000/get_model_info")
    return profile_cfg.get("api_base_url") or api_base_from_ready_url(ready_url)


def torch_profile_enabled(runtime: Any, cfg: dict[str, Any]) -> bool:
    """Return whether this run should invoke the SGLang torch-profiler API."""

    profile = channel_config(cfg, "torch")
    return runtime.enabled and "torch" in runtime.channels and profile.get("enabled", True)


def should_stop_torch_profiler_after_workload(profile_cfg: dict[str, Any]) -> bool:
    """Return whether the runner must stop profiling after the workload."""

    if not profile_cfg.get("stop_after_workload", True):
        return False
    return profile_cfg.get("num_steps") is None


def start_torch_profiler(layout: RunLayout, server_cfg: dict[str, Any], profile_cfg: dict[str, Any]) -> None:
    """Invoke ``/start_profile`` and persist both request and response."""

    body = build_profile_body(profile_cfg, layout)
    write_json(layout.run_dir / "profile_start_body.json", body)
    log("Starting SGLang profiler via /start_profile.")
    response = post_json(
        profiler_api_base(server_cfg, profile_cfg).rstrip("/") + "/start_profile",
        body,
        timeout=int(profile_cfg.get("start_timeout_sec", 120)),
        api_key=profile_cfg.get("api_key"),
    )
    write_json(layout.run_dir / "profile_start_response.json", response)


def stop_torch_profiler(layout: RunLayout, server_cfg: dict[str, Any], profile_cfg: dict[str, Any]) -> None:
    """Invoke ``/stop_profile``, recording failures unless strict stop is enabled."""

    log("Stopping SGLang profiler via /stop_profile.")
    try:
        response = post_json(
            profiler_api_base(server_cfg, profile_cfg).rstrip("/") + "/stop_profile",
            None,
            timeout=int(profile_cfg.get("stop_timeout_sec", 1800)),
            api_key=profile_cfg.get("api_key"),
        )
    except Exception as exc:
        if profile_cfg.get("strict_stop", False):
            raise
        response = {"warning": str(exc)}
    write_json(layout.run_dir / "profile_stop_response.json", response)
