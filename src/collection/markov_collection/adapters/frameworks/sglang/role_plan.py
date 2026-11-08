"""Choose representatives before observation and retain actual worker/group bindings."""

from __future__ import annotations

from typing import Any

from markov_collection.adapters.frameworks.sglang.module_layout import LayerCommunication


class WorkerRolePlan:
    @classmethod
    def module_key(cls, module: Any, layers: dict[int, int], storages: dict[int, int]) -> Any:
        import torch
        from sglang.srt.layers.utils.common import PPMissingLayer

        if isinstance(module, PPMissingLayer):
            return None
        fields = {}
        identities = {
            "rank",
            "tp_rank",
            "pp_rank",
            "ep_rank",
            "moe_ep_rank",
            "moe_tp_rank",
            "attn_tp_rank",
            "gpu_id",
            "device_id",
            "device_index",
            "local_rank",
            "prefix",
            "layer_name",
        }
        for name, value in sorted(vars(module).items()):
            field = name.lstrip("_")
            if field in identities:
                continue
            if type(value) is int and field in {"layer_id", "layer_idx", "start_layer", "end_layer"} and value in layers:

                # A local layer reference is distinct from an unrelated literal with the same numeric value.
                fields[name] = ("local_layer", layers[value])
                continue
            if isinstance(value, torch.device):
                value = value.type
            elif isinstance(value, torch.dtype):
                value = str(value)
            if isinstance(value, (str, int, float, bool, type(None))):
                fields[name] = value
            elif field in {"quant_method", "dispatcher"}:
                fields[name] = f"{type(value).__module__}.{type(value).__qualname__}"

        communicator = LayerCommunication.read(module)
        if communicator is not None:
            fields["layer_communicator"] = communicator.document()

        tensors = []
        for name, value in (*module.named_parameters(recurse=False), *module.named_buffers(recurse=False)):
            storage = value.untyped_storage()._cdata
            alias = storages.setdefault(storage, len(storages))
            tensors.append(
                (
                    name,
                    tuple(value.shape),
                    tuple(value.stride()),
                    value.storage_offset(),
                    str(value.dtype),
                    value.device.type,
                    alias,
                )
            )
        children = []
        for name, child in module.named_children():
            key = cls.module_key(child, layers, storages)
            if key is not None:
                children.append((str(len(children)) if isinstance(module, torch.nn.ModuleList) else name, key))

        # Placement is bound per worker; native implementation, local geometry and flags remain significant.
        return f"{type(module).__module__}.{type(module).__qualname__}", fields, tensors, children

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
                self.module_key(model, layers, {}),
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
