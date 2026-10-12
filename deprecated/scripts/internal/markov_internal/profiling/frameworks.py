"""Small capability boundary for supported profiling frameworks."""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class FrameworkAdapter:
    """Framework-owned capture and cleanup capabilities."""

    name: str
    default_ready_url: str
    hook_library: str
    channels: tuple[str, ...]
    profiler_api: bool = False
    hicache: bool = False


_FRAMEWORKS = {
    "sglang": FrameworkAdapter(
        name="sglang",
        default_ready_url="http://127.0.0.1:30000/get_model_info",
        hook_library="build/docker/sglang/lib/libhook.so",
        channels=("torch", "python_probe", "ld_preload"),
        profiler_api=True,
        hicache=True,
    ),
    "ktransformers": FrameworkAdapter(
        name="ktransformers",
        default_ready_url="http://127.0.0.1:10002/v1/models",
        hook_library="build/docker/ktransformers/lib/libhook.so",
        channels=("ld_preload",),
    ),
}


def framework_adapter(name: str) -> FrameworkAdapter:
    """Resolve one explicit framework name."""

    try:
        return _FRAMEWORKS[name]
    except KeyError:
        raise ValueError(f"unknown profiling framework: {name}") from None


def validate_framework_channels(adapter: FrameworkAdapter, channels: tuple[str, ...]) -> None:
    """Reject capture channels that the selected runtime cannot produce."""

    unsupported = set(channels) - set(adapter.channels)
    if unsupported:
        raise ValueError(
            f"{adapter.name} profiling does not support channels {sorted(unsupported)}; "
            f"available={list(adapter.channels)}"
        )
