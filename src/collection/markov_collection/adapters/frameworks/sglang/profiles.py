"""Own selected-worker profiler publication, separately from semantic capture."""

from __future__ import annotations

import time
from pathlib import Path
from typing import Any


class NativeProfiles:
    def __init__(self, observation: Any) -> None:
        self.observation = observation

    @staticmethod
    def wait(directory: Path, ranks) -> None:
        published = [directory / "native" / f"rank-{rank}" for rank in ranks]

        # A PP ingress reply can precede downstream profiler shutdown; rename is each worker's publication boundary.
        while not all(path.is_dir() for path in published):
            time.sleep(0.05)

    def request(self, original: Any, manager: Any, request: Any) -> Any:
        from sglang.srt.managers.io_struct import ProfileReqOutput, ProfileReqType

        owner = self.observation
        if request.req_type == ProfileReqType.START_PROFILE:

            # All instances have drained warmup and published their real pool layouts before this request.
            owner.plan_roles()
        if owner.selected:
            result = original(manager, request)
        else:
            if request.req_type == ProfileReqType.STOP_PROFILE:
                import torch.distributed as distributed

                # Non-exporting participants still join the native profiler's stop consensus.
                distributed.barrier(manager.dp_tp_cpu_group)
            result = ProfileReqOutput(success=True, message="Worker has no assigned observation")
        if request.req_type == ProfileReqType.START_PROFILE and result.success:
            owner.window.prepared_phase = "profiler"
        return result

    def start(self, original: Any, manager: Any, output_dir: str, *args: Any, **kwargs: Any) -> Any:
        rank = self.observation.parallel["rank"]
        self.destination = Path(output_dir) / f"rank-{rank}"
        return original(manager, str(self.destination.with_name(f".rank-{rank}")), *args, **kwargs)

    def stop(self, original: Any, manager: Any, *args: Any, **kwargs: Any) -> Any:
        result = original(manager, *args, **kwargs)
        if result.success:
            manager.torch_profiler_output_dir.rename(self.destination)
            if self.observation.capture.closed:
                self.observation.capture.runtime.release()
        return result

    @staticmethod
    def handler(original: Any, directory: str, *args: Any, **kwargs: Any) -> Any:

        # Native recording must close before CPU-only SDK export and graph production begin.
        kwargs["analyse_flag"] = False
        return original(directory, *args, **kwargs)

    def install(self) -> None:
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        manager = "sglang.srt.managers.scheduler_components.profiler_manager.SchedulerProfilerManager."
        targets = {
            manager + "_profile": self.request,
            manager + "_init_profile": self.start,
            manager + "_stop_profile": self.stop,
            "torch_npu.profiler.tensorboard_trace_handler": self.handler,
        }
        for target, hook in targets.items():
            HookRegistry.register(target, hook, HookType.AROUND)
