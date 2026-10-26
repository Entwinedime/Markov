"""Inject workload expert choices while native serving owns batching and execution."""

from __future__ import annotations

from typing import Any

import torch

from markov_collection.adapters.frameworks.sglang.batches import BatchTensors
from markov_collection.adapters.frameworks.sglang.routes import ExpertRouteConditions


class NativeExpertRoutes:
    def __init__(self, values: Any, description: dict[str, Any]) -> None:
        self.values = values
        self.input = description["serving_input"]
        settings = self.input["configuration"]
        if "pd" in settings:
            settings = settings["pd"][description["disaggregation_role"]]
        topology = settings["ep"]
        self.size = topology["size"]
        self.source = self.input["ep_routes"][topology["id"]]

        self.aliases = {
            native: request["id"]
            for field in ("baseline_requests", "source_requests")
            for native, request in zip(description[field], self.input["requests"], strict=True)
        }

        self.members: list[dict[str, Any]] | None = None

    def configure(self, model: Any, layers: int) -> None:
        from sglang.srt.layers.moe.fused_moe_triton.layer import FusedMoE

        experts = next(module for module in model.modules() if isinstance(module, FusedMoE))
        if experts.moe_ep_size != self.size:
            raise ValueError("injected routes must match the native expert group")

        self.conditions = ExpertRouteConditions(
            {
                "num_hidden_layers": layers,
                "num_experts": experts._num_global_routed,
                "num_experts_per_tok": experts.top_k,
            },
            self.size,
        )
        self.conditions.check(self.source, self.input["requests"])

    def run_batch(self, original: Any, scheduler: Any, batch: Any, *args: Any, **kwargs: Any) -> Any:

        # Uniform warmup has its own requests; baseline and capture share the supplied logical identities.
        if not batch.reqs or batch.reqs[0].rid not in self.aliases:
            return original(scheduler, batch, *args, **kwargs)

        previous = self.members
        self.members = BatchTensors.members(batch)
        try:
            return original(scheduler, batch, *args, **kwargs)
        finally:
            self.members = previous

    def topk(self, original: Any, topk: Any, *args: Any, **kwargs: Any) -> Any:
        result = original(topk, *args, **kwargs)
        if self.members is None:
            return result

        from sglang.srt.distributed.parallel_state import get_moe_ep_group
        from sglang.srt.eplb.expert_location_dispatch import topk_ids_logical_to_physical

        # The native EP group partitions the padded token vector before dispatch in this DeepEP backend.
        with self.values.auxiliary():
            group = get_moe_ep_group()
            rows = []
            for member in self.members:
                begin = member["context_tokens"]
                rows.extend(
                    self.conditions.rows(self.aliases[member["id"]], topk.layer_id, begin, begin + member["new_tokens"])
                )
            padded_rows = result.topk_ids.shape[0] * group.world_size
            rows.extend(self.conditions.rows(None, topk.layer_id, 0, padded_rows - len(rows)))
            if len(rows) != padded_rows:
                raise ValueError("native token layout exceeds the supplied route coverage")

            selected = torch.tensor(rows, dtype=result.topk_ids.dtype, device="cpu").tensor_split(group.world_size)[
                group.rank_in_group
            ]
            selected = selected.to(result.topk_ids.device)
            location = kwargs.get("expert_location_dispatch_info")
            if location is not None:
                selected = topk_ids_logical_to_physical(selected, location)
            result.topk_ids.copy_(selected)
        return result

    def install(self) -> None:
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        HookRegistry.register("sglang.srt.managers.scheduler.Scheduler.run_batch", self.run_batch, HookType.AROUND)
        HookRegistry.register("sglang.srt.layers.moe.topk.TopK.forward_npu", self.topk, HookType.AROUND)
