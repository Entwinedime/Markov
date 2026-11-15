"""Choose representatives before observation and retain actual worker/group bindings."""

from __future__ import annotations

from typing import Any

from markov_collection.adapters.frameworks.sglang.module_layout import ModuleStructure


class WorkerRolePlan:
    def __init__(self, model: Any, facts: dict[str, Any], observation_conditions) -> None:
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
            "observation_conditions": observation_conditions,
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

    @staticmethod
    def observed_ranks(workers, bindings):
        selected = {worker["representative"] for worker in bindings.values()}
        for representative in sorted(selected):
            group = {worker["parallel"]["rank"]: set(worker["observation_conditions"]) for worker in workers
                     if bindings[str(worker["parallel"]["rank"])]["representative"] == representative}
            missing = set().union(*group.values()) - group[representative]
            while missing:
                rank = min(group, key=lambda rank: (-len(group[rank] & missing), rank))
                selected.add(rank)
                missing.difference_update(group[rank])
        return sorted(selected)

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
                **{name: value for name, value in worker.items() if name not in {"key", "observation_conditions"}},
            }

        return {
            "basis": "native_module_layout_group_roles_and_source_conditions",
            "participants": len(self.workers),
            "representatives": [worker["parallel"]["rank"] for worker in representatives],
            "observed_roles": self.observed_ranks(self.workers, bindings),
            "workers": bindings,
        }
