"""Plan representative workers from native modules, role duties and declared shard bindings."""

from __future__ import annotations

import inspect
from typing import Any


class WorkerRolePlan:
    @staticmethod
    def mask_fields() -> tuple[str, ...]:
        from sglang.srt.layers.vocab_parallel_embedding import get_masked_input_and_mask

        return tuple(inspect.signature(get_masked_input_and_mask).parameters)[1:]

    @classmethod
    def vocabulary(cls, model: Any, size: int) -> tuple[list[dict[str, int]], bool]:
        from sglang.srt.layers.vocab_parallel_embedding import ParallelLMHead, VocabParallelEmbedding

        tables = [
            module
            for module in model.modules()
            if (
                isinstance(module, VocabParallelEmbedding)
                and not isinstance(module, ParallelLMHead)
                and module.enable_tp
            )
        ]
        fields = cls.mask_fields()
        bindings = [{} for _ in range(size)]
        reusable = True

        for table in tables:
            for rank in range(size):
                indices = table._get_indices(
                    table.num_embeddings_padded,
                    table.org_vocab_size_padded,
                    table.num_embeddings,
                    table.org_vocab_size,
                    rank,
                    size,
                )
                for field in fields:
                    name, value = "vocab_" + field, getattr(indices, field)
                    if name in bindings[rank] and bindings[rank][name] != value:
                        reusable = False
                    bindings[rank][name] = value

        return bindings, reusable

    @classmethod
    def from_model(cls, model: Any, size: int) -> dict[str, Any]:
        bindings, reusable = cls.vocabulary(model, size)

        # Native root/peer control differs; a shared mask contract must also cover every input table.
        representatives = list(range(size)) if not reusable else list(range(min(size, 2)))
        instances = [
            {
                "representative": 1,
                "environment": {"rank": rank, "tp_rank": rank, "pp_rank": 0, "ep_rank": 0},
                "bindings": bindings[rank],
            }
            for rank in range(2, size)
            if rank not in representatives
        ]
        return {
            "basis": "native_vocabulary_partitions_and_root_peer_control",
            "representatives": representatives,
            "instances": instances,
        }

    @classmethod
    def module_key(cls, module: Any, first_layer: int, storages: dict[int, int]) -> Any:
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
            if isinstance(value, int) and field in {"layer_id", "layer_idx", "start_layer", "end_layer"}:
                value -= first_layer
            if isinstance(value, torch.device):
                value = value.type
            elif isinstance(value, torch.dtype):
                value = str(value)
            if isinstance(value, (str, int, float, bool, type(None))):
                fields[name] = value
            elif field in {"quant_method", "dispatcher"}:
                fields[name] = f"{type(value).__module__}.{type(value).__qualname__}"

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
            key = cls.module_key(child, first_layer, storages)
            if key is not None:
                children.append((str(len(children)) if isinstance(module, torch.nn.ModuleList) else name, key))

        # Role and layer identities become bindings; implementation, local geometry and flags remain significant.
        return f"{type(module).__module__}.{type(module).__qualname__}", fields, tensors, children

    @classmethod
    def from_group(cls, model: Any, parallel: dict[str, Any]) -> dict[str, Any]:
        import torch.distributed as distributed
        from sglang.srt.distributed import get_world_group
        from sglang.srt.layers.moe.fused_moe_triton.layer import FusedMoE

        vocabulary, reusable = cls.vocabulary(model, parallel["logical_tp"])
        first, last = parallel["layer_range"]
        expert = any(isinstance(module, FusedMoE) for module in model.modules())
        identity = parallel["rank"]
        facts = {
            "environment": {name: parallel[name] for name in ("rank", "tp_rank", "pp_rank", "ep_rank", "layer_range")},
            "bindings": vocabulary[parallel["tp_rank"]],
            "experts": expert,
            "key": (
                parallel["tp_rank"] == 0,
                parallel["pp_rank"] == 0,
                parallel["pp_rank"] + 1 == parallel["logical_pp"],
                last - first,
                cls.module_key(model, first, {}),
                identity if not reusable else None,
            ),
        }
        group = get_world_group().cpu_group
        workers = [None] * distributed.get_world_size(group)

        # Protocol peers already exist; exchange only initialization metadata before selecting observation.
        distributed.all_gather_object(workers, facts, group=group)
        representatives = []
        instances = []

        # Compare complete keys in memory; persist only the representative and target bindings.
        for worker in workers:
            source = next((previous for previous in representatives if previous["key"] == worker["key"]), None)
            if source is None:
                representatives.append(worker)
                continue
            bindings: dict[str, Any] = dict(worker["bindings"])
            offset = worker["environment"]["layer_range"][0] - source["environment"]["layer_range"][0]
            if offset:
                bindings["layer"] = ["add", "layer", offset]
            if worker["experts"]:
                bindings.update(
                    expert_rank=worker["environment"]["ep_rank"],
                    expert_begin=["mul", worker["environment"]["ep_rank"], "local_expert_count"],
                )
                if offset:
                    bindings["expert_layer"] = ["add", "expert_layer", offset]
            instances.append(
                {
                    "representative": source["environment"]["rank"],
                    "environment": worker["environment"],
                    "bindings": bindings,
                }
            )

        return {
            "basis": "native_module_layout_and_group_roles",
            "participants": len(workers),
            "representatives": [worker["environment"]["rank"] for worker in representatives],
            "instances": instances,
        }
