"""Use the native PD rank resolver to declare topology-local peer interfaces."""

from sglang.srt.disaggregation.base import KVArgs
from sglang.srt.disaggregation.common.conn import CommonKVManager, PrefillServerInfo


class NativeKvTopology:
    def __init__(self, tp, pp, rank, stage) -> None:
        self.attn_tp_size = tp
        self.attn_cp_size = 1
        self.attn_cp_rank = 0
        self.pp_size = pp
        self.pp_rank = stage
        self.enable_all_cp_ranks_for_transfer = False
        self.is_mla_backend = False
        self.kv_args = KVArgs()
        self.kv_args.engine_rank = rank

    def prefill_sources(self, tp, pp, page_size, kv_dtype):
        info = PrefillServerInfo(tp, 1, 1, pp, page_size, kv_dtype, False)
        CommonKVManager._resolve_rank_mapping(self, info)
        return info


class PdPeerBindings:
    def __init__(self, workers):
        self.workers = workers
        self.sources = {}
        self.destinations = {rank: {} for rank, row in workers.items() if row["mode"] == "prefill"}
        self.infos = {}
        prefill = next(row for row in workers.values() if row["mode"] == "prefill")["parallel"]
        ordered = sorted(workers, key=lambda rank: (workers[rank]["parallel"]["pp_rank"], workers[rank]["parallel"]["tp_rank"]))
        for target in ordered:
            row = workers[target]
            if row["mode"] != "decode":
                continue
            parallel = row["parallel"]
            mapping = NativeKvTopology(parallel["logical_tp"], parallel["logical_pp"], parallel["tp_rank"], parallel["pp_rank"])
            info = mapping.prefill_sources(prefill["logical_tp"], prefill["logical_pp"], row["page_size"], row["layout"]["dtype"])
            self.infos[target] = info
            self.sources[target] = [
                rank for tp_rank in info.target_tp_ranks for pp_rank in reversed(info.target_pp_ranks)
                for rank in ordered if workers[rank]["mode"] == "prefill"
                and workers[rank]["parallel"]["tp_rank"] == tp_rank and workers[rank]["parallel"]["pp_rank"] == pp_rank
            ]
            for source in self.sources[target]:
                if workers[source]["page_size"] != row["page_size"]:
                    raise ValueError("native PD transfer requires matching page sizes")
                peers = self.destinations[source]
                peers[target] = f"destination/{len(peers)}"

    def layouts(self, source):
        return {
            binding: {
                "layout": self.workers[target]["layout"],
                "tp": self.workers[target]["parallel"]["logical_tp"],
                "pp": self.workers[target]["parallel"]["logical_pp"],
            }
            for target, binding in self.destinations.get(source, {}).items()
        }

    def signature(self, local):
        pairs = (
            [(local, target) for target in self.destinations[local]]
            if self.workers[local]["mode"] == "prefill"
            else [(source, local) for source in self.sources[local]]
        )
        result = []
        for source, target in pairs:
            peer = target if source == local else source
            layout = dict(self.workers[peer]["layout"])
            layout.update(start_layer=0, end_layer=layout["end_layer"] - layout["start_layer"])

            # Group position distinguishes unequal-TP shard duties even when every local tensor has the same shape.
            destinations = list(self.destinations[source])
            origins = self.sources[target]
            result.append((
                origins.index(source), len(origins), destinations.index(target), len(destinations), layout,
                self.workers[source]["registration"]["metadata"]["row_bytes"],
                self.workers[target]["registration"]["metadata"]["row_bytes"],
            ))
        return result
