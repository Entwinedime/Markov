"""Check pending state hazards from actual bindings, without replaying allocation."""

from __future__ import annotations

from collections import Counter, defaultdict
from dataclasses import dataclass


@dataclass(frozen=True)
class MemoryAccess:
    rows: tuple
    columns: tuple
    write: bool
    start: int
    finish: int
    invocation: int
    publication: tuple | None = None

    def conflicts(self, other):
        if self.write and other.write and self.publication is not None and self.publication == other.publication:
            return False
        return (self.write or other.write) and all(
            max(left[0], right[0]) < min(left[1], right[1])
            for left, right in ((self.rows, other.rows), (self.columns, other.columns))
        )


class StateLifetimes:
    def __init__(self):
        self.active = defaultdict(set)
        self.ends = defaultdict(set)
        self.widths = {}
        self.accesses = defaultdict(list)
        self.kv_bindings = defaultdict(list)
        self.fields = defaultdict(set)
        self.required = defaultdict(dict)
        self.identities = {}
        self.counts = Counter()
        self.findings = []

    def invocation(self, graph, identity, events, entered):
        self.close_bindings()
        self.entered = entered
        self.identities[entered] = identity
        for scope in graph["scopes"]:
            for access in scope["slot_accesses"]:
                for request in access["requests"]:
                    publication = (request, access["publication"]) if access.get("publication") else None
                    self.fields[access["pool"], access["field"], request].add(MemoryAccess(
                        (0, 1), (0, 1), access["access"] == "write", events[access["start"]], events[access["finish"]], entered, publication,
                    ))
            for access in scope["kv_accesses"]:
                layout = access["slice"]
                key = access["pool"], access["layer"]
                if self.widths.setdefault(key, layout["token_bytes"]) != layout["token_bytes"]:
                    raise ValueError("one physical KV layer has inconsistent token widths")
                interval = (access["layer"], (layout["begin"], layout["end"]), access["access"] == "write",
                            events[access["start"]], events[access["finish"]])
                for region in access["regions"]:
                    self.accesses[access["pool"], region["request"]].append((region["begin"], region["end"], interval))

    def close_bindings(self):
        self.counts["field_bindings_missing"] += len(self.fields)
        for pool, field, request in self.fields:
            self.findings.append(("state_binding_evidence", self.identities[self.entered], {
                "pool": pool, "field": field, "request": request, "reason": "declared state access has no actual physical row binding",
            }))
        self.fields.clear()

        # Safety needs every declared region, including calls that cannot be paired across runs.
        for key, accesses in self.accesses.items():
            bound = sorted(self.kv_bindings[key])
            missing = []
            for left, right in sorted({(left, right) for left, right, _ in accesses if left < right}):
                covered = left
                for begin, end in bound:
                    if begin > covered:
                        break
                    covered = max(covered, end)
                    if covered >= right:
                        break
                self.counts["kv_regions_checked"] += 1
                if covered < right:
                    missing.append([left, right])
            if missing:
                self.counts["kv_regions_unbound"] += len(missing)
                self.findings.append(("binding_evidence", self.identities[self.entered], {
                    "pool": key[0], "request": key[1], "regions": missing,
                    "reason": "declared KV access lacks complete actual physical binding coverage",
                }))
        self.accesses.clear()
        self.kv_bindings.clear()

    def bind(self, address, value, detail):
        if value in self.active[address]:
            return

        # Logical release never separates two accesses to the same still-active physical address.
        for previous in self.active[address]:
            if previous.invocation != self.entered and value.conflicts(previous):
                self.required[previous.finish].setdefault(value.start, {
                    **detail, "previous_invocation": previous.invocation, "invocation": self.entered,
                })
        self.active[address].add(value)
        self.ends[value.finish].add(address)
        self.counts["physical_access_intervals"] += 1
        self.counts[address[1] + "_access_intervals"] += 1

    def state_bound(self, row):
        if row["invocation"] != self.entered:
            raise ValueError("state bindings must accompany their submitted graph")
        key = row["pool"], row["field"], row["request"]
        for value in self.fields.pop(key):
            self.bind((row["pool"], "field", row["slot"], row["field"]), value, {
                "pool": row["pool"], "field": row["field"], "slot": row["slot"],
            })
        self.counts["field_bindings_checked"] += 1

    def bound(self, row):
        if row["invocation"] != self.entered:
            raise ValueError("physical bindings must accompany their submitted graph")
        begin = row["begin"]
        for page, first, last in row["ranges"]:
            end = begin + last - first
            for left, right, interval in self.accesses[row["pool"], row["request"]]:
                lo, hi = max(begin, left), min(end, right)
                if lo >= hi:
                    continue
                layer, columns, write, start, finish = interval
                value = MemoryAccess((first + lo - begin, first + hi - begin), columns, write, start, finish, self.entered)
                self.bind((row["pool"], "kv", page, layer), value, {"pool": row["pool"], "page": page, "layer": layer})
            begin = end
        self.kv_bindings[row["pool"], row["request"]].append((row["begin"], begin))

    def completed(self, event):
        for address in self.ends.pop(event, ()):
            remaining = {access for access in self.active[address] if access.finish != event}
            if remaining:
                self.active[address] = remaining
            else:
                del self.active[address]

    def released(self, row):
        pages = set(row["returned_pages"])
        if not pages:
            return
        pending = {page for pool, kind, page, _ in self.active if kind == "kv" and pool == row["group"] and page in pages}
        self.counts.update(release_calls=1, released_pages=len(pages), released_pages_inflight=len(pending))

    def compare(self, graph, event_ranks):
        self.close_bindings()
        self.counts["pending_conflicts"] = sum(map(len, self.required.values()))
        self.counts["pending_conflicts_ordered"] = 0
        if not self.required:
            return

        # Reuse the shared run-local graph, including paths that cross workers through CPU protocols.
        sources = list(self.required)
        for begin in range(0, len(sources), 1024):
            current = sources[begin:begin + 1024]
            masks = graph.reachable_masks([(event_ranks[source], source) for source in current])
            for bit, source in enumerate(current):
                for target, detail in self.required[source].items():
                    if source != target and masks[graph.positions[event_ranks[target], target]] & (1 << bit):
                        self.counts["pending_conflicts_ordered"] += 1
                    else:
                        self.findings.append(("state_lifetime", self.identities[detail["invocation"]], {
                            **detail, "previous_finish": source, "next_start": target,
                            "reason": "overlapping pending state accesses lack an ordering path",
                        }))

    def report(self):
        return {
            **self.counts,
            "scope": "Prediction-only cross-invocation KV and request-field safety. Every declared nonempty KV region and field access needs an actual physical binding, independently of cross-run pairing. Completed accesses retire before later submissions; logical release preserves pending hazards. Equal publications may write concurrently; readers join pending writers. Excludes tensor lifetimes, initialization/values, native intra-call release timing and unexercised reuse. Bindings do not prove cross-run row correspondence.",
        }
