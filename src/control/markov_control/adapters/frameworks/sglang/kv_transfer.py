"""Bind observed PD peers using native topology; graph templates own transfer geometry."""

import torch
from markov_adapters.sglang.kv_peers import PdPeerBindings


class PdTopology:
    def __init__(self, workers, local):
        instances = {source["id"]: source for source, _, _, _ in workers}
        prefill, = [source for source in instances.values() if source["mode"] == "prefill"]
        decode, = [source for source in instances.values() if source["mode"] == "decode"]
        if len(instances) != 2 or prefill["model_config"] != decode["model_config"] or prefill["dtype"] != decode["dtype"]:
            raise ValueError("PD requires one Prefill and one Decode instance of the same model and dtype")
        rows = {
            index: {
                "mode": source["mode"],
                "parallel": {
                    "logical_tp": source["tp"], "logical_pp": source["pp"],
                    "tp_rank": rank % source["tp"], "pp_rank": rank // source["tp"],
                },
                "layout": source["workers"][rank]["kv_layout"], "page_size": settings["page_size"],
            }
            for index, (source, settings, _, rank) in enumerate(workers)
        }
        bindings = PdPeerBindings(rows)
        self.peers = {}
        self.info = bindings.infos.get(local)
        for origin, targets in bindings.destinations.items():
            sender, _, _, sender_rank = workers[origin]
            layouts = bindings.layouts(origin)
            for target, binding in targets.items():
                if local not in (origin, target):
                    continue
                observed = sender["workers"][sender_rank]["kv_peers"][binding]
                if observed != layouts[binding]:
                    raise ValueError("PD destination geometry differs from the observed transfer template")
                receiver, _, _, receiver_rank = workers[target]
                peer = target if local == origin else origin
                peer_source, _, _, peer_rank = workers[peer]
                self.peers[peer] = {"pool": receiver["workers"][receiver_rank]["group"], "binding": binding,
                                    "group": peer_source["workers"][peer_rank]["group"]}
                if local == target:
                    publishers = [workers[index][0]["workers"][workers[index][3]]["group"] for index in targets]
                    self.peers[peer]["publishers"] = publishers

        if local in bindings.sources:

            # Use the native bootstrap order: TP order, then higher PP stages first.
            self.peers = {peer: self.peers[peer] for peer in bindings.sources[local]}


class MetadataRows:
    def __init__(self, buffers):
        pointers, _, _ = buffers.get_buf_infos()
        tensors = {value.data_ptr(): value for value in vars(buffers).values() if isinstance(value, torch.Tensor)}
        self.buffers = [tensors[pointer] for pointer in pointers]

    def read(self, index):

        # Preserve native values, including its room gate; copying them here does not make them ready in simulated time.
        return [buffer[index].tolist() for buffer in self.buffers]

    def write(self, index, values):
        for buffer, row in zip(self.buffers, values, strict=True):
            if len(row) != buffer.shape[1]:
                raise ValueError("PD metadata row layout differs between native participants")
            buffer[index].copy_(buffer.new_tensor(row))
