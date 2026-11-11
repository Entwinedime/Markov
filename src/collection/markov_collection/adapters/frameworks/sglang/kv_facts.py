"""Resolve observed PD addresses through the actual registered source and destination pools."""

import json
from collections import defaultdict
from pathlib import Path

from markov_adapters.sglang.kv_layout import KvBufferAddresses, MhaKvLayout
from markov_collection.adapters.frameworks.sglang.transfer_groups import TransferGroups


class NativeKvFacts:
    def __init__(self, directory: Path, rank: int) -> None:
        self.directory = directory
        self.recording = json.loads((directory / f"rank-{rank}.json").read_text())
        self.registrations = {}
        self.groups = TransferGroups(self.recording)

    def registration(self, recording):
        observed = recording["registration"]
        layout = MhaKvLayout.read(recording["layout"])
        buffers = KvBufferAddresses(
            layout, recording["page_size"], observed["pointers"], observed["lengths"], observed["page_bytes"],
        )
        return recording["parallel"], buffers, observed["metadata"]

    def metadata_accesses(self, scope, parent, remote):
        transfer = scope["native_transfer"]
        chunk = parent["native_kv_chunk"]
        member, = parent["work"]["members"]
        source = self.registrations[self.recording["kv_session"]][2]
        target = self.registrations[transfer["session"]][2]
        if (transfer["room"], transfer["source_index"]) != (chunk["room"], chunk["metadata_index"]):
            raise ValueError("native metadata send differs from its enqueued request")
        if not source["row_bytes"] or source["row_bytes"] != target["row_bytes"]:
            raise ValueError("native metadata endpoints have different row layouts")
        if parent["parameters"]["last_chunk"] != 1:
            raise ValueError("native first-token metadata must belong to the final chunk")

        # Native send_aux transfers one complete registered row from every buffer, including the room gate.
        rows = []
        for buffers, index in ((source, transfer["source_index"]), (target, transfer["target_index"])):
            if buffers["reservations"][str(transfer["room"])] != {"request": member["id"], "index": index}:
                raise ValueError("native metadata transfer differs from its endpoint's request reservation")
            addresses = []
            for pointer, length, width in zip(buffers["pointers"], buffers["lengths"], buffers["row_bytes"], strict=True):
                if width <= 0 or length % width or not 0 <= index < length // width:
                    raise ValueError("native metadata row lies outside its registered buffer")
                addresses.append(pointer + index * width)
            rows.append(addresses)
        expected = [list(block) for block in zip(*rows, source["row_bytes"], strict=True)]
        if sorted(transfer["blocks"]) != sorted(expected):
            raise ValueError("native metadata transfer does not cover the registered request rows")

        accesses = []
        for pool, mode in (("local", "read"), (remote, "write")):
            key = f"metadata/{mode}"
            accesses.append({
                "space": "slot", "pool": pool, "field": "metadata", "access": mode,
                "members": key, "requests": [member["id"]], "interval": "service",
            })

        # Native senders independently publish the same request's first token; the receiver joins their completions.
        accesses[-1]["publication"] = "first-token"
        return accesses

    @staticmethod
    def logical_regions(buffers, address, length, pages, member):
        page_size = buffers.page_size
        begin = member["context_tokens"]
        end = begin + member["new_tokens"]
        regions = []
        for region in buffers.regions(address, length):
            token = region["token_begin"]
            while token < region["token_end"]:
                page, offset = divmod(token, page_size)
                count = min(region["token_end"] - token, page_size - offset)
                logical = begin + pages[page] * page_size + offset
                stop = min(logical + count, end)
                if logical < stop:
                    regions.append((region["layer"], region["slice"], logical, stop))
                token += count
        return regions

    def accesses(self, scope):
        transfer = scope["native_transfer"]
        parent = self.recording["scopes"][scope["submitted_by"]]
        target_rank, target, _ = self.registrations[transfer["session"]]
        remote = self.destinations[target_rank["rank"]]
        if transfer["kind"] == "metadata":
            return self.metadata_accesses(scope, parent, remote)
        member, = parent["work"]["members"]
        chunk = parent["native_kv_chunk"]
        if transfer["source_pages"] != chunk["source_pages"] or len(transfer["source_pages"]) != len(transfer["target_pages"]):
            raise ValueError("native transport changed the enqueued chunk's page extent")
        if transfer["source_pages"] and not transfer["blocks"]:
            raise ValueError("nonempty native KV transfer has no observed address blocks")
        source = self.registrations[self.recording["kv_session"]][1]
        if source.page_size != target.page_size:
            raise ValueError("native transfer endpoints have different page sizes")
        positions = {}
        for endpoint in ("source", "target"):
            pages = transfer[f"{endpoint}_pages"]
            positions[endpoint] = {int(page): index for index, page in enumerate(pages)}
            if len(positions[endpoint]) != len(pages):
                raise ValueError("native transfer repeats a physical page in one request chunk")

        intervals = defaultdict(list)
        for source_address, target_address, length in transfer["blocks"]:
            for pool, access, buffers, address, pages in (
                ("local", "read", source, source_address, positions["source"]),
                (remote, "write", target, target_address, positions["target"]),
            ):
                for layer, byte_slice, begin, end in self.logical_regions(buffers, address, length, pages, member):
                    key = pool, access, layer, byte_slice["token_bytes"], byte_slice["begin"], byte_slice["end"]
                    intervals[key].append((begin, end))
        result = []
        for (pool, access, layer, width, first, last), ranges in sorted(intervals.items()):
            merged = []
            for begin, end in sorted(ranges):
                if merged and begin <= merged[-1][1]:
                    merged[-1][1] = max(merged[-1][1], end)
                else:
                    merged.append([begin, end])
            expected = [member["context_tokens"], member["context_tokens"] + member["new_tokens"]]
            if merged != [expected]:
                raise ValueError("native transfer slice does not cover its declared request chunk")
            value = {
                "space": "kv", "pool": pool, "layer": layer, "access": access, "interval": "service",
                "slice": {"token_bytes": width, "begin": first, "end": last},
                "regions": [{"request": member["id"], "begin": expected[0], "end": expected[1]}],
                "rule": {"begin": "context_tokens", "end": ["add", "context_tokens", "new_tokens"]},
            }

            # Adjacent K/V pieces with the same logical coverage form one token payload, not separate templates.
            adjacent = result and all(result[-1][key] == value[key] for key in ("pool", "layer", "access", "regions"))
            if adjacent and result[-1]["slice"]["token_bytes"] == width and result[-1]["slice"]["end"] == first:
                result[-1]["slice"]["end"] = last
            else:
                result.append(value)
        return result

    def document(self):
        transfers = [scope for scope in self.recording["scopes"] if "native_transfer" in scope]
        if not transfers:
            return self.recording

        # The target registration is independent native evidence; source pointer arithmetic cannot invent its layout.
        for path in sorted((self.directory / "kv-pools").glob("*.json")):
            recording = json.loads(path.read_text())
            session = recording["registration"]["session"]
            if session in self.registrations:
                raise ValueError("native KV session identifies more than one pool")
            self.registrations[session] = self.registration(recording)

        # Canonical peer positions come from actual transfer endpoints, not a second run of native routing code.
        targets = {scope["native_transfer"]["session"] for scope in transfers}
        ordered = sorted(
            (self.registrations[session][0] for session in targets),
            key=lambda parallel: (parallel["pp_rank"], parallel["tp_rank"]),
        )
        self.destinations = {parallel["rank"]: f"destination/{index}" for index, parallel in enumerate(ordered)}
        if set(self.destinations.values()) != set(self.recording["environment"]["kv_peers"]):
            raise ValueError("observed native transfers do not cover the declared peer interfaces")
        for scope in transfers:
            transfer = scope["native_transfer"]
            rank, buffers, _ = self.registrations[transfer["session"]]
            binding = self.destinations[rank["rank"]]
            expected = {"layout": buffers.layout.document(), "tp": rank["logical_tp"], "pp": rank["logical_pp"]}
            if expected != self.recording["environment"]["kv_peers"][binding]:
                raise ValueError("native transfer registration differs from the planned peer interface")
            scope["state_accesses"] = self.accesses(scope)
        for parent, members in self.groups.members.items():
            parent = self.recording["scopes"][parent]
            parent["ports"].append({
                "name": "transfer.complete", "direction": "output", "use": "signal",
                "boundary": {"scope": members[-1], "interval": "service", "side": "end"},
            })
        return self.recording
