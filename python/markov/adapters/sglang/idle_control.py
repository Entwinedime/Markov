"""Read only the pending-work counters required by an explicit HiCache gate."""

from typing import Any, Callable


def idle_state(_original: Callable, cache: Any) -> dict:
    controller = cache.cache_controller
    pending = {
        "ongoing_write_through": len(cache.ongoing_write_through),
        "ongoing_load_back": len(cache.ongoing_load_back),
        "write_queue": len(controller.write_queue),
        "load_queue": len(controller.load_queue),
        "ack_write_queue": len(controller.ack_write_queue),
        "ack_load_queue": len(controller.ack_load_queue),
    }
    if cache.enable_storage:
        pending.update(
            ongoing_prefetch=len(cache.ongoing_prefetch),
            ongoing_backup=len(cache.ongoing_backup),
            **{
                name: getattr(controller, name).qsize()
                for name in (
                    "prefetch_queue",
                    "backup_queue",
                    "prefetch_revoke_queue",
                    "ack_backup_queue",
                    "host_mem_release_queue",
                )
            },
        )

    # The scheduler also requires is_fully_idle(); no tree or tensor snapshot.
    return {"hicache_idle": not cache.is_eagle and not any(pending.values()), "pending": pending}


def install() -> None:
    from sglang.srt.plugins.hook_registry import HookRegistry, HookType

    HookRegistry.register(
        "sglang.srt.mem_cache.hiradix_cache.HiRadixCache.hicache_diagnostic_snapshot", idle_state, HookType.AROUND
    )
