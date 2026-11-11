"""Inject workload expert choices while native serving owns batching and execution."""

from __future__ import annotations

from typing import Any

import torch
from markov_adapters.sglang.batches import BatchTensors
from markov_adapters.sglang.experts import ExpertBatch
from markov_workload.expert_routes import ExpertRoutes


class NativeExpertRoutes:
    def __init__(self, values: Any, description: dict[str, Any]) -> None:
        self.values = values
        self.input = description["serving_input"]
        settings = self.input["configuration"]
        topology = settings["ep"]
        self.size = topology["size"]
        self.source = self.input["ep_routes"][topology["id"]]

        self.aliases = {
            native: request["id"]
            for native, request in zip(description["source_requests"], self.input["requests"], strict=True)
        }

        self.members: list[dict[str, Any]] | None = None

    def configure(self, experts: dict, layers: list[dict], model_layers: int) -> None:
        from sglang.srt.distributed.parallel_state import get_moe_ep_group
        from sglang.srt.server_args import get_global_server_args

        if any(module.moe_ep_size != self.size for module in experts.values()):
            raise ValueError("injected routes must match the native expert group")

        self.conditions = ExpertRoutes(self.source, self.input["requests"], layers, model_layers, self.size - 1)
        args = get_global_server_args()
        self.geometry = ExpertBatch(self.conditions, self.size, get_moe_ep_group().rank_in_group, args.moe_a2a_backend, args.deepep_mode)

        # A module pre-hook runs before the observation wrapper snapshots the expert's input ports.
        for module in experts.values():
            module.register_forward_pre_hook(self.expert, with_kwargs=True)

    def run_batch(self, original: Any, scheduler: Any, batch: Any, *args: Any, **kwargs: Any) -> Any:

        # Uniform warmup has its own requests; capture uses the supplied logical identities.
        if not batch.reqs or batch.reqs[0].rid not in self.aliases:
            return original(scheduler, batch, *args, **kwargs)

        previous = self.members
        with self.values.metadata():
            self.members = BatchTensors.members(batch)
            for member in self.members:
                member["id"] = self.aliases[member["id"]]
        try:
            return original(scheduler, batch, *args, **kwargs)
        finally:
            self.members = previous

    def expert(self, experts: Any, args: tuple[Any, ...], kwargs: dict[str, Any]) -> None:
        if self.members is None:
            return

        from sglang.srt.distributed.parallel_state import get_moe_ep_group
        from sglang.srt.eplb.expert_location_dispatch import ExpertLocationDispatchInfo, topk_ids_logical_to_physical
        from sglang.srt.server_args import get_global_server_args

        result = args[1] if len(args) > 1 else kwargs["topk_output"]
        if result.topk_ids.shape[1] != self.conditions.layers[experts.layer_id]["top_k"]:
            raise ValueError("native expert input does not match the supplied route top-k")

        # DeepEP partitions padded tokens; a local expert group retains the entire token vector.
        with self.values.auxiliary():
            group = get_moe_ep_group()
            rows = self.conditions.batch_rows(self.members, experts.layer_id, result.topk_ids.shape[0] * group.world_size)

            selected = torch.tensor(rows, dtype=result.topk_ids.dtype, device="cpu").tensor_split(group.world_size)[
                group.rank_in_group
            ]
            selected = selected.to(result.topk_ids.device)
            location = (
                ExpertLocationDispatchInfo.init_new(experts.layer_id)
                if get_global_server_args().ep_dispatch_algorithm is not None
                else None
            )
            if location is not None:
                selected = topk_ids_logical_to_physical(selected, location)
            result.topk_ids.copy_(selected)

    def install(self) -> None:
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        HookRegistry.register("sglang.srt.managers.scheduler.Scheduler.run_batch", self.run_batch, HookType.AROUND)
