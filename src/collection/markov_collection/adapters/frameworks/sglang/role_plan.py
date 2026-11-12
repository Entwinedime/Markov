"""Choose representatives before observation and retain actual worker/group bindings."""

from __future__ import annotations

from typing import Any

from markov_collection.adapters.frameworks.sglang.module_layout import ModuleStructure


class WorkerRolePlan:
    def __init__(self, model: Any, facts: dict[str, Any]) -> None:
        import torch.distributed as distributed
        from sglang.srt.distributed import get_world_group

        parallel = facts["parallel"]
        identity = parallel["rank"]
        layout = dict(facts["kv_layout"])
        first, last = layout["start_layer"], layout["end_layer"]
        layers = {layer: index for index, layer in enumerate(range(first, last + 1))}
        layout.update(start_layer=0, end_layer=last - first)
        facts = {
            **facts,
            "key": (
                parallel["tp_rank"] == 0,
                parallel["pp_rank"] == 0,
                parallel["pp_rank"] + 1 == parallel["logical_pp"],
                [(name, group["role"], len(group["members"]), group["members"][0] == identity) for name, group in facts["groups"].items()],
                layout,
                ModuleStructure.read(model, layers, {}),
            ),
        }
        group = get_world_group().cpu_group
        self.workers = [None] * distributed.get_world_size(group)

        # PP forwards control requests after local handling, so whole-world exchange belongs to initialization.
        distributed.all_gather_object(self.workers, facts, group=group)

    def document(self, peers) -> dict[str, Any]:
        representatives = []
        bindings = {}
        for initialized in self.workers:
            worker = dict(initialized)
            if peers is not None:
                rank = worker["parallel"]["rank"]
                worker["kv_peers"] = peers.layouts(rank)
                worker["key"] = (worker["key"], peers.signature(rank))
            source = next((previous for previous in representatives if previous["key"] == worker["key"]), None)
            if source is None:
                source = worker
                representatives.append(worker)
            bindings[str(worker["parallel"]["rank"])] = {
                "representative": source["parallel"]["rank"],
                **{name: value for name, value in worker.items() if name != "key"},
            }

        return {
            "basis": "native_module_layout_and_group_roles",
            "participants": len(self.workers),
            "representatives": [worker["parallel"]["rank"] for worker in representatives],
            "workers": bindings,
        }
