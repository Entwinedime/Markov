"""Compare consumed physical bindings without requiring identical allocator addresses."""

from bisect import bisect_right
from collections import Counter, defaultdict

from actions import ActionIdentity
from lifetimes import StateLifetimes


class NativePoolHistory:
    def __init__(self, history):
        self.history = history
        self.page_size = history["initial"]["page_size"]
        self.capacity = history["initial"]["page_capacity"]
        self.pages, self.slots = defaultdict(list), defaultdict(list)
        self.ownership = defaultdict(list)
        for record in history["events"]:
            operation = record["operation"]
            window = record["end_raw_ns"], record["begin_raw_ns"]
            if operation["kind"] == "release_pages" and not operation["grouped"]:

                # Grouped frees only stage pages; the later ungrouped call actually returns them.
                for page in {index // self.page_size for index in operation["indices"]}:
                    self.pages[page].append(window)
            elif operation["kind"] == "release_request":
                self.slots[operation["slot"]].append(window)
                self.ownership["request", operation["slot"]][-1][2] = record["begin_raw_ns"]
            elif operation["kind"] == "slots" and record["result"] is not None:
                for request, slot in zip(operation["requests"], record["result"], strict=True):
                    self.ownership["request", slot].append([request, record["end_raw_ns"], float("inf")])
        for windows in (*self.pages.values(), *self.slots.values()):
            windows.sort()

        for record in history["state_events"]:
            slot, operation = record["slot"], record["operation"]
            if slot is None:
                continue
            claims = self.ownership[record["field"], slot]
            if operation == "allocate":
                claims.append([None, record.get("end_raw_ns"), float("inf")])
            elif operation == "bind":
                claims[-1][0] = record["request"]
            elif operation == "release":
                claims[-1][2] = record.get("begin_raw_ns")

    def owns(self, field, slot, request, window):
        begin, end = window

        # API return and release entry bound certain ownership; an overlapping or unmeasured mutation proves less.
        return sum(owner == request and first is not None and last is not None and first <= begin <= end <= last
                   for owner, first, last in self.ownership[field, slot]) == 1

    def ranges(self, binding, epoch):
        result, begin = [], binding["begin"]
        if begin < 0:
            raise ValueError("request binding starts before token zero")
        for page, first, last in binding["ranges"]:
            if not (0 <= page < self.capacity and 0 <= first < last <= self.page_size):
                raise ValueError("request binding lies outside its physical pool")
            end = begin + last - first
            result.append((begin, end, (page, epoch(page)), first))
            begin = end
        return result

    def resolve(self, binding, window, clock):
        begin, end = window
        if begin is None or end is None or end < begin or clock != self.history["clock"]:
            raise ValueError("physical binding has no common measured release clock")
        slot = binding["slot"]
        histories = [self.pages[page] for page, _, _ in binding["ranges"]]
        if slot is not None:
            histories.append(self.slots[slot])

        # An observation window must not cross a release; otherwise its physical generation is ambiguous.
        if any(start <= end and finish >= begin for windows in histories for finish, start in windows):
            return None
        ranges = self.ranges(binding, lambda page: bisect_right(self.pages[page], (begin,)))
        identity = None if slot is None else (slot, bisect_right(self.slots[slot], (begin,)))
        return identity, ranges


class BindingComparison:
    def __init__(self, histories, sessions):
        self.pools = {group: NativePoolHistory(history) for group, history in histories.items()}
        self.sessions = sessions
        self.native, self.predicted = {}, defaultdict(list)
        self.invocations = {}
        self.page_epochs, self.slot_epochs = Counter(), Counter()
        self.findings, self.counts = [], {}
        self.unconsumed_snapshots = 0
        self.lifetimes = StateLifetimes()

    @staticmethod
    def coverage(ranges):
        merged = []
        for begin, end in sorted(ranges):
            if begin == end:
                continue
            if merged and begin <= merged[-1][1]:
                merged[-1][1] = max(merged[-1][1], end)
            else:
                merged.append([begin, end])
        return merged

    def observations(self, contents, raw, group):
        graph = contents.graph
        for binding in self.pools[group].history["bindings"]:
            scope = graph["scopes"][contents.source_ids[binding["scope"]]]
            events = {port["name"]: port["event"] for port in scope["ports"]}
            window = tuple(graph["evidence"]["events"][events[name]]["time_ns"] for name in ("call.enter", "call.return"))
            yield binding, group, window

        for index, scope in enumerate(raw["scopes"]):
            transfer = scope.get("native_transfer")
            if not transfer or transfer["kind"] != "kv":
                continue
            member, = raw["scopes"][scope["submitted_by"]]["work"]["members"]
            executions = scope["service"]["executions"]
            if not executions:
                continue
            window = min(item["begin_raw_ns"] for item in executions), max(item["end_raw_ns"] for item in executions)
            for side, pool in (("source", group), ("target", self.sessions[transfer["session"]])):
                if pool not in self.pools:
                    continue
                size = self.pools[pool].page_size
                count = member["new_tokens"]
                pages = transfer[f"{side}_pages"]
                if member["context_tokens"] % size or len(pages) != (count + size - 1) // size:
                    raise ValueError("native transfer pages do not describe its aligned request chunk")

                # Transport owns frozen pages; it no longer consumes the request's table slot.
                ranges = [[page - 1, 0, min(size, count - position * size)] for position, page in enumerate(pages)]
                yield {"scope": index, "request": member["id"], "slot": None,
                       "begin": member["context_tokens"], "ranges": ranges}, pool, window

    def observed(self, contents, raw, group, action_identities):
        graph = contents.graph
        accesses, identities, seen = defaultdict(list), {}, set()
        for owner, scope in enumerate(graph["scopes"]):
            if not ActionIdentity.selected(owner, scope):
                continue
            identity = action_identities[owner]
            key = (group, owner)
            identities[key] = identity
            for index in contents.paths(owner):
                for access in graph["scopes"][index]["kv_accesses"]:
                    pool = contents.pool(access["pool"], group)
                    for region in access["regions"]:
                        accesses[key, pool, region["request"]].append((region["begin"], region["end"]))

        for binding, pool, window in self.observations(contents, raw, group):
            owner = contents.source_ids[binding["scope"]]
            while not ActionIdentity.selected(owner, graph["scopes"][owner]):
                owner = graph["scopes"][owner]["parent"]
                if owner is None:
                    raise ValueError("native physical binding has no independently identified action")
            identity = action_identities[owner]
            key = (group, owner), pool, binding["request"]
            seen.add(key)
            resolved = self.pools[pool].resolve(binding, window, graph["evidence"]["clock"])
            if resolved is None:
                self.findings.append(("binding_evidence", identity, {"pool": pool, "request": binding["request"], "reason": "release overlaps the observed binding window"}))
                continue
            slot, ranges = resolved
            end = ranges[-1][1] if ranges else binding["begin"]
            if any(left < binding["begin"] or right > end for left, right in accesses[key]):
                self.findings.append(("binding_evidence", identity, {"pool": pool, "request": binding["request"], "reason": "observed physical binding does not cover the source graph access"}))
                continue
            coverage = self.coverage(accesses[key])
            if not coverage:

                # Preparing a page table does not consume KV; execution only reports bindings used by graph accesses.
                self.unconsumed_snapshots += 1
                continue
            value = identity, slot, ranges, coverage
            if key in self.native and self.native[key] != value:
                self.findings.append(("binding_evidence", identity, {"pool": pool, "request": binding["request"], "reason": "observations disagree on an action's physical binding"}))
            else:
                self.native[key] = value

        # Missing evidence remains missing even when prediction also omits its binding records.
        for key, pool, request in accesses.keys() - seen:
            self.findings.append(("binding_evidence", identities[key], {"pool": pool, "request": request, "reason": "source KV access has no observed physical binding"}))

    def initialized(self, row):
        for instance in row["instances"]:
            settings = instance["settings"]
            geometry = settings["page_size"], settings["max_total_tokens"] // settings["page_size"]
            for group in instance["groups"]:
                pool = self.pools.get(group["id"])
                if pool is not None and geometry != (pool.page_size, pool.capacity):
                    raise ValueError("native and predicted physical pools have different geometry")

    def released(self, row):
        self.lifetimes.released(row)
        group = row["group"]
        for field, epochs in (("returned_pages", self.page_epochs), ("returned_slots", self.slot_epochs)):
            for value in row[field]:
                epochs[group, value] += 1

    def invocation(self, graph, identity, events, key):
        entered = next(port["event"] for port in graph["scopes"][0]["ports"] if port["name"] == "call.enter")
        self.invocations[events[entered]] = key, identity
        self.lifetimes.invocation(graph, identity, events, events[entered])

    def bound(self, row):
        self.lifetimes.bound(row)
        invocation, identity = self.invocations[row["invocation"]]
        group = row["pool"]
        key = invocation, group, row["request"]
        if group not in self.pools:
            self.findings.append(("binding_evidence", identity, {"pool": group, "request": row["request"], "reason": "no observed physical pool for this action"}))
            return
        pool = self.pools[group]
        ranges = pool.ranges(row, lambda page: self.page_epochs[group, page])
        slot = row["slot"]
        slot_identity = None if slot is None else (slot, self.slot_epochs[group, slot])
        self.predicted[key].append((slot_identity, ranges, row["begin"]))

    @staticmethod
    def pair(native, predicted, forward, reverse):
        if native in forward and forward[native] != predicted or predicted in reverse and reverse[predicted] != native:
            return False
        forward[native], reverse[predicted] = predicted, native
        return True

    def compare(self, pairs):
        pages, slots = defaultdict(dict), defaultdict(dict)
        matched, intervals, unpaired = 0, 0, 0
        targets = set(pairs.values())
        expected = {(pairs[key[0]], *key[1:]) for key in self.native if key[0] in pairs}
        for key in self.predicted.keys() - expected:
            if key[0] in targets:
                self.findings.append(("binding_evidence", {"group": key[0][0], "event": key[0][1]},
                                      {"pool": key[1], "request": key[2], "reason": "no observed physical binding for this action"}))

        for key, (identity, native_slot, native_ranges, expected_coverage) in self.native.items():
            group = key[1]
            if key[0] not in pairs:
                unpaired += 1
                continue
            failures, coverage = [], []
            rows = self.predicted.get((pairs[key[0]], *key[1:]), [])
            if not rows:
                failures.append("observed request binding is absent from prediction")
            for slot, ranges, begin in rows:
                if native_slot is not None and (slot is None or not self.pair(native_slot, slot, slots[group, "forward"], slots[group, "reverse"])):
                    failures.append("request-slot sharing or release generation differs")
                end = ranges[-1][1] if ranges else begin
                native_begin = native_ranges[0][0] if native_ranges else 0
                native_end = native_ranges[-1][1] if native_ranges else native_begin
                if begin < native_begin or end > native_end:
                    failures.append("predicted access exceeds the observed request extent")
                    continue
                coverage.append((begin, end))
                cursor = 0
                for start, stop, page, offset in ranges:
                    intervals += 1
                    while cursor < len(native_ranges) and native_ranges[cursor][1] <= start:
                        cursor += 1
                    index = cursor
                    while index < len(native_ranges) and native_ranges[index][0] < stop:
                        left, right, actual_page, actual_offset = native_ranges[index]
                        overlap = max(start, left)
                        if offset + overlap - start != actual_offset + overlap - left:
                            failures.append("page offset differs at a common request token")
                        if not self.pair(actual_page, page, pages[group, "forward"], pages[group, "reverse"]):
                            failures.append("KV page sharing or release generation differs")
                        index += 1

            # A page table may exceed the accessed region; compare the observed graph's actual read/write union.
            if self.coverage(coverage) != expected_coverage:
                failures.append("predicted intervals differ from the observed KV access coverage")
            if failures:
                self.findings.append(("physical_binding", identity, {"pool": group, "request": key[2], "reasons": sorted(set(failures))}))
            else:
                matched += 1
        self.counts = {"native_request_bindings": len(self.native), "request_bindings_matched": matched, "request_bindings_unpaired": unpaired,
                       "predicted_request_bindings_unpaired": sum(key[0] not in targets for key in self.predicted),
                       "snapshots_without_kv_access": self.unconsumed_snapshots,
                       "page_only_bindings": sum(value[1] is None for value in self.native.values()),
                       "predicted_regions": sum(map(len, self.predicted.values())), "page_intervals": intervals,
                       "scope": "Observed request extents, page offsets and page/slot alias identities across recorded release epochs. Transport consumes frozen pages, not table slots; native enqueue ownership and retained pages/metadata are checked separately. This comparison does not prove allocator or in-flight lifetime correctness."}
