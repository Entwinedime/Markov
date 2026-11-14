"""Compare declared PD state footprints with actual registered buffers and copied bytes."""

from collections import Counter, defaultdict

from markov_adapters.sglang.kv_layout import KvBufferAddresses, MhaKvLayout


class PdPayloadEvidence:
    def __init__(self, registrations, sessions, pools):
        self.registrations = registrations
        self.groups, self.pools = sessions, pools
        self.sessions = {group: session for session, group in sessions.items()}
        self.buffers = {}
        for session, recording in registrations.items():
            native = recording["registration"]
            self.buffers[session] = KvBufferAddresses(
                MhaKvLayout.read(recording["layout"]), recording["page_size"],
                native["pointers"], native["lengths"], native["page_bytes"],
            )
        self.findings, self.counts = [], Counter()

    def ownership(self, contents, parent, call, source, target):
        graph = contents.graph
        scope = graph["scopes"][contents.source_ids[parent["id"]]]
        entered, = [port["event"] for port in scope["ports"] if port["name"] == "call.enter"]
        begin = graph["evidence"]["events"][entered]["time_ns"]
        end = call["host_interval"]["end_raw_ns"]
        if graph["evidence"]["clock"] != "monotonic_raw_ns" or begin is None or end < begin:
            raise ValueError("native PD ownership has no common ordered host interval")
        member, = parent["work"]["members"]
        request, chunk = member["id"], parent["native_kv_chunk"]
        transfer = call["native_transfer"]

        # A table slot is required at enqueue only. Copies subsequently retain pages and metadata rows independently.
        checks = [(source, "request", chunk.get("request_slot"), (begin, begin))]
        if transfer["kind"] == "metadata":
            checks.extend((session, "metadata", transfer[f"{side}_index"], (begin, end))
                          for session, side in ((source, "source"), (target, "target")))
        for session, field, slot, window in checks:
            pool = self.pools.get(self.groups[session])
            if pool is None or slot is None or not pool.owns(field, slot, request, window):
                self.findings.append(("binding_evidence", {"group": self.groups[session], "scope": parent["id"]}, {
                    "request": request, "field": field, "slot": slot,
                    "reason": "no measured ownership interval covers the enqueue or retained native transfer",
                }))
            else:
                self.counts[f"{field}_ownership_checked"] += 1

        if transfer["kind"] != "kv":
            return
        for session, side in ((source, "source"), (target, "target")):
            pool = self.pools.get(self.groups[session])
            ranges = [] if pool is None else [[page - 1, 0, pool.page_size] for page in transfer[f"{side}_pages"]]
            binding = {"slot": None, "begin": 0, "ranges": ranges}
            if pool is None or pool.resolve(binding, (begin, end), graph["evidence"]["clock"]) is None:
                self.findings.append(("binding_evidence", {"group": self.groups[session], "scope": parent["id"]}, {
                    "request": request, "reason": "a native transfer page has no release-free enqueue-to-copy interval",
                }))
            else:
                self.counts["queued_page_intervals_checked"] += len(binding["ranges"])

    @staticmethod
    def coverage(rectangles):
        changes = defaultdict(list)
        for begin, end, first, last in rectangles:
            if not begin < end or not first < last:
                raise ValueError("PD payload contains an empty or reversed byte rectangle")
            changes[begin].append(((first, last), 1))
            changes[end].append(((first, last), -1))
        active, result, previous = Counter(), [], None
        for position, events in sorted(changes.items()):
            if active and previous < position:
                columns = []
                for first, last in sorted(active):
                    if columns and first <= columns[-1][1]:
                        columns[-1][1] = max(columns[-1][1], last)
                    else:
                        columns.append([first, last])
                if result and result[-1][1] == previous and result[-1][2] == columns:
                    result[-1][1] = position
                else:
                    result.append([previous, position, columns])
            for column, delta in events:
                active[column] += delta
                if not active[column]:
                    del active[column]
            previous = position
        return result

    def page_map(self, transfer, member, source, target, maps):
        source_pages, target_pages = transfer["source_pages"], transfer["target_pages"]
        if len(source_pages) != len(target_pages):
            raise ValueError("native PD endpoints have different page counts")
        if self.buffers[source].page_size != self.buffers[target].page_size:
            raise ValueError("native PD endpoints have different page sizes")
        if source_pages and not transfer["blocks"]:
            raise ValueError("native PD pages have no recorded copy payload")
        for session, pages in ((source, source_pages), (target, target_pages)):
            size = self.buffers[session].page_size
            begin, count = member["context_tokens"], member["new_tokens"]
            if begin % size or len(set(pages)) != len(pages) or len(pages) != (count + size - 1) // size:
                raise ValueError("native PD page list differs from its captured page-aligned chunk")
            positions = maps[session, member["id"]]
            for index, page in enumerate(pages):
                logical = begin + index * size
                value = page * size, min(size, begin + count - logical)
                if positions.setdefault(logical, value) != value:
                    raise ValueError("one native PD chunk maps a logical page to competing physical pages")

    def raw_kv(self, transfer, member, source, target, footprints):
        for first, last, length in transfer["blocks"]:
            for session, mode, address, pages in ((source, "read", first, transfer["source_pages"]),
                                                   (target, "write", last, transfer["target_pages"])):
                buffers = self.buffers[session]
                layout, size = buffers.layout, buffers.page_size
                candidates = [index for index, (start, count) in enumerate(zip(buffers.starts, buffers.lengths, strict=True))
                              if start <= address and address + length <= start + count]
                if length <= 0 or len(candidates) != 1:
                    raise ValueError("native PD bytes do not lie in one registered buffer")

                buffer, = candidates
                layers = layout.end_layer - layout.start_layer
                width = layout.key_token_bytes if buffer < layers else layout.value_token_bytes
                component = 0 if buffer < layers else layout.key_token_bytes
                layer = layout.start_layer + buffer % layers
                offset, covered = address - buffers.starts[buffer], 0
                output = footprints[session, mode, layer, member["id"]]
                for index, page in enumerate(pages):
                    begin, end = max(offset, page * size * width), min(offset + length, (page + 1) * size * width)
                    covered += max(0, end - begin)

                    # Check raw byte intersections independently of the producer's address-to-region decoder.
                    valid_tokens = max(0, min(size, member["new_tokens"] - index * size))
                    end = min(end, (page * size + valid_tokens) * width)
                    if begin >= end:
                        continue

                    head, tail = begin // width, (end - 1) // width
                    if head == tail:
                        output.append((head, head + 1, component + begin % width, component + (end - 1) % width + 1))
                    else:
                        output.append((head, head + 1, component + begin % width, component + width))
                        if head + 1 < tail:
                            output.append((head + 1, tail, component, component + width))
                        output.append((tail, tail + 1, component, component + (end - 1) % width + 1))
                if covered != length:
                    raise ValueError("native PD bytes escape the chunk's actual physical pages")
            self.counts["recorded_kv_bytes"] += length

    def metadata(self, transfer, parent, source, target):
        member, = parent["work"]["members"]
        chunk = parent["native_kv_chunk"]
        if (transfer["room"], transfer["source_index"]) != (chunk["room"], chunk["metadata_index"]) or parent["parameters"]["last_chunk"] != 1:
            raise ValueError("native metadata transfer differs from its final submitted chunk")
        rows = []
        for session, index in ((source, transfer["source_index"]), (target, transfer["target_index"])):
            metadata = self.registrations[session]["registration"]["metadata"]
            if metadata["reservations"][str(transfer["room"])] != {"request": member["id"], "index": index}:
                raise ValueError("native metadata row differs from its request reservation")
            rows.append([(pointer + index * width, width) for pointer, width in zip(metadata["pointers"], metadata["row_bytes"], strict=True)])
            if any(width <= 0 or length % width or not 0 <= index < length // width
                   for length, width in zip(metadata["lengths"], metadata["row_bytes"], strict=True)):
                raise ValueError("native metadata row escapes its registered buffer")
        if not rows[0] or [width for _, width in rows[0]] != [width for _, width in rows[1]]:
            raise ValueError("native metadata endpoints have incompatible row layouts")
        copied = defaultdict(list)
        for source_address, target_address, length in transfer["blocks"]:
            matches = [(index, source_address - first) for index, ((first, width), (last, _)) in enumerate(zip(*rows, strict=True))
                       if first <= source_address and source_address + length <= first + width
                       and target_address - last == source_address - first]
            if len(matches) != 1 or length <= 0:
                raise ValueError("native metadata copy changes its field or row offset")
            index, offset = matches[0]
            copied[index].append((0, 1, offset, offset + length))
            self.counts["recorded_metadata_bytes"] += length
        if any(self.coverage(copied[index]) != [[0, 1, [[0, width]]]] for index, (_, width) in enumerate(rows[0])):
            raise ValueError("native metadata bytes omit a registered request field")
        self.counts["metadata_transfers"] += 1
        return {(source, "read", "metadata", member["id"], None), (target, "write", "metadata", member["id"], "first-token")}

    def inspect(self, contents, raw, group, dependencies):
        writers = defaultdict(list)
        for original in raw["scopes"]:
            for access in original.get("state_accesses", []):
                if (access["pool"], access.get("field"), access["access"]) == ("local", "metadata", "write"):
                    for request in access["requests"]:
                        writers[request].append(original["id"])

        for index, scope in enumerate(contents.graph["scopes"]):
            calls = [raw["scopes"][origin] for origin in scope["bindings"]["source_scopes"] if "native_transfer" in raw["scopes"][origin]]
            if not calls:
                continue
            native, declared = defaultdict(list), defaultdict(list)
            maps, slots = defaultdict(dict), set()
            for call in calls:
                transfer = call["native_transfer"]
                parent = raw["scopes"][call["submitted_by"]]
                member, = parent["work"]["members"]
                source, target = raw["kv_session"], transfer["session"]
                self.ownership(contents, parent, call, source, target)
                if transfer["kind"] == "metadata":
                    slots.update(self.metadata(transfer, parent, source, target))
                    origin, = writers[member["id"]]
                    producer = contents.graph["scopes"][contents.source_ids[origin]]
                    written, = [access["finish"] for access in producer["slot_accesses"]
                                if (access["pool"], access["field"], access["access"]) == ("local", "metadata", "write")
                                and member["id"] in access["requests"]]
                    consumed, = [access["start"] for access in scope["slot_accesses"]
                                 if access["pool"] == "local" and access["access"] == "read"]

                    # The original field write and registered copy row establish content origin, independently of prediction.
                    if not dependencies.reaches(written, consumed):
                        raise ValueError("native metadata copy loses its observed first-token producer")
                    self.counts["metadata_producers_checked"] += 1
                elif transfer["kind"] == "kv":
                    if transfer["source_pages"] != parent["native_kv_chunk"]["source_pages"]:
                        raise ValueError("native KV bytes differ from their enqueued source pages")
                    self.page_map(transfer, member, source, target, maps)
                    self.raw_kv(transfer, member, source, target, native)
                    self.counts["kv_transfers"] += 1
                else:
                    raise ValueError("native PD transfer has an unknown payload kind")

            for access in scope["kv_accesses"]:
                session = self.sessions[contents.pool(access["pool"], group)]
                layout = self.buffers[session].layout
                interval = access["slice"]
                if (not layout.start_layer <= access["layer"] < layout.end_layer
                        or interval["token_bytes"] != layout.key_token_bytes + layout.value_token_bytes
                        or not 0 <= interval["begin"] < interval["end"] <= interval["token_bytes"]):
                    raise ValueError("declared PD access changes its registered layer/byte layout")
                for region in access["regions"]:
                    covered = 0
                    for logical, (physical, count) in maps[session, region["request"]].items():
                        begin, end = max(logical, region["begin"]), min(logical + count, region["end"])
                        if begin < end:
                            declared[session, access["access"], access["layer"], region["request"]].append(
                                (physical + begin - logical, physical + end - logical, interval["begin"], interval["end"]))
                            covered += end - begin
                    if covered != region["end"] - region["begin"]:
                        raise ValueError("declared PD access escapes its native request/page mapping")

            for access in scope["slot_accesses"]:
                if access["members"] != f"metadata/{access['access']}" or scope["work"][access["members"]] != access["requests"]:
                    raise ValueError("declared PD metadata changes its request selector")
            actual_slots = {
                (self.sessions[contents.pool(access["pool"], group)], access["access"], access["field"], request, access.get("publication"))
                for access in scope["slot_accesses"] for request in access["requests"]
            }
            differences = [key for key in native.keys() | declared.keys() if self.coverage(native[key]) != self.coverage(declared[key])]
            if differences or slots != actual_slots:
                self.findings.append(("pd_payload", {"group": group, "scope": index}, {
                    "reason": "declared PD state differs from the actual copied byte footprint or metadata rows",
                    "kv_regions_different": len(differences), "metadata_rows_missing": len(slots - actual_slots),
                    "metadata_rows_extra": len(actual_slots - slots),
                }))
            self.counts["scopes_checked"] += 1
            self.counts["kv_accesses_checked"] += len(scope["kv_accesses"])
            self.counts["metadata_accesses_checked"] += len(scope["slot_accesses"])
