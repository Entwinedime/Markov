"""Semantic deployment applicability, shared by import and prediction boundaries."""

from __future__ import annotations

from typing import Any

from ..common.io import load_json
from .source_dag.profiles import ProfileRunRef, parse_server_command_flags


def source_environment(source: ProfileRunRef, resources: dict[str, str] | None = None) -> dict[str, Any]:
    """Read declared runtime dimensions; unknown hardware remains unknown.

    An explicit resource declaration can annotate historical inputs without
    changing their assets. It cannot override a conflicting recorded setting.
    These descriptions establish an applicability domain, not measurement proof.
    """

    flags = parse_server_command_flags(source.run_dir / "server_cmd.txt")
    flags.setdefault("tp_size", "1")
    flags.setdefault("base_gpu_id", "0")
    flags.setdefault("gpu_id_step", "1")
    config = load_json(source.config_path)
    recorded = config.get("metadata", {}).get("resource_environment")
    if recorded is not None and resources is not None and recorded != resources:
        raise ValueError(f"resource declaration conflicts with recorded capture: {source.manifest_path}")
    resources = recorded if resources is None else resources
    names = (
        "model_path",
        "tp_size",
        "dtype",
        "kv_cache_dtype",
        "quantization",
        "attention_backend",
        "sampling_backend",
        "hicache_io_backend",
        "hicache_mem_layout",
        "hicache_storage_backend",
        "numa_node",
        "base_gpu_id",
        "gpu_id_step",
        "max_running_requests",
        "chunked_prefill_size",
        "disable_cuda_graph",
        "enable_torch_compile",
        "torch_compile_max_bs",
        "cuda_graph_bs_decode",
        "cuda_graph_bs_prefill",
        "cuda_graph_max_bs_decode",
        "cuda_graph_max_bs_prefill",
        "cuda_graph_config",
        "cuda_graph_backend_decode",
        "cuda_graph_backend_prefill",
        "cuda_graph_tc_compiler",
        "disable_prefill_cuda_graph",
        "disable_decode_cuda_graph",
        "disable_cuda_graph_padding",
    )
    # Observer destinations and measurement switches are provenance, not
    # hardware or model variables. Resource/thread and execution controls stay.
    # Same-capture CPU correction is admitted separately and is not certified
    # by removing these environment switches from the applicability domain.
    capture_options = {
        "SGLANG_HICACHE_FILE_BACKEND_STORAGE_DIR",
        "HOOK_EMISSION_TIMING",
        "HOOK_SYNC_THREAD_CPU",
        "HOOK_MEMCPY2D_TIMING",
        "SGLANG_NPU_PROFILER_SERIAL_EXPORT",
        "SGLANG_HICACHE_IO_TIMING",
        "SGLANG_STEP_TIMING_DIR",
        "SGLANG_SCHEDULER_TIMING",
        "SGLANG_HICACHE_HOST_TIMING",
        "SGLANG_LAYER_WAIT_TIMING",
        "SGLANG_PREFETCH_TIMING",
        "SGLANG_STEP_TIMING_EMISSION",
        "SGLANG_STEP_SCHEDSTAT",
    }
    return {
        "framework": load_json(source.manifest_path)["framework"],
        "server": {name: flags.get(name) for name in names},
        "env": {key: value for key, value in config.get("env", {}).items() if key not in capture_options},
        "resources": resources,
    }


def environments_match(first: dict[str, Any], second: dict[str, Any]) -> bool:
    """Compare the semantic declarations, without digest or version handshakes."""

    return first == second


def require_cost_environment(deployment: dict[str, Any]) -> None:
    """Reject fitting/application under an undeclared hardware/runtime domain.

    Unknown-domain observations may be stored for later review. They cannot
    authorize sharing a trained response across unknown machines or runtimes.
    """

    resources = deployment["resources"]
    if resources is None or any(
        not isinstance(resources.get(key), str) or not resources[key].strip()
        for key in ("device", "host", "storage", "implementation")
    ):
        raise ValueError(
            "independent cost models require device, host, storage and implementation resource declarations"
        )
