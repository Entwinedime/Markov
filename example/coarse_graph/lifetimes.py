"""Check pending KV hazards from physical bindings and execution, without replaying allocation."""

from collections import Counter, defaultdict
from dataclasses import dataclass

from dependencies import CausalGraph


@dataclass(frozen=True)
class PageAccess:
    rows: tuple
    columns: tuple
    write: bool
    start: int
    finish: int
    invocation: int

    def conflicts(self, other):
        return (self.write or other.write) and all(
            max(left[0], right[0]) < min(left[1], right[1])
            for left, right in ((self.rows, other.rows), (self.columns, other.columns))
        )


class KvLifetimes:
    def __init__(self):
        self.active = defaultdict(set)
        self.ends = defaultdict(set)
        self.widths = {}
        self.accesses = defaultdict(list)
        self.required = defaultdict(dict)
        self.identities = {}
        self.counts = Counter()
        self.findings = []

    def invocation(self, graph, identity, events, entered):
        self.entered = entered
        self.identities[entered] = identity
        self.accesses.clear()
        for scope in graph["scopes"]:
            for access in scope["kv_accesses"]:
                layout = access["slice"]
                key = access["pool"], access["layer"]
                if self.widths.setdefault(key, layout["token_bytes"]) != layout["token_bytes"]:
                    raise ValueError("one physical KV layer has inconsistent token widths")
                interval = (access["layer"], (layout["begin"], layout["end"]), access["access"] == "write",
                            events[access["start"]], events[access["finish"]])
                for region in access["regions"]:
                    self.accesses[access["pool"], region["request"]].append((region["begin"], region["end"], interval))

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
                value = PageAccess((first + lo - begin, first + hi - begin), columns, write, start, finish, self.entered)
                address = row["pool"], page, layer
                if value in self.active[address]:
                    continue

                # Page epochs do not separate physical hazards: an old access survives logical release.
                for previous in self.active[address]:
                    if previous.invocation == self.entered or not value.conflicts(previous):
                        continue
                    self.required[previous.finish].setdefault(start, {
                        "pool": row["pool"], "page": page, "layer": layer,
                        "previous_invocation": previous.invocation, "invocation": self.entered,
                    })
                self.active[address].add(value)
                self.ends[finish].add(address)
                self.counts["physical_access_intervals"] += 1
            begin = end

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
        pending = {page for pool, page, _ in self.active if pool == row["group"] and page in pages}
        self.counts.update(release_calls=1, released_pages=len(pages), released_pages_inflight=len(pending))

    def compare(self, graphs, cross_rank_edges):
        self.counts["pending_conflicts"] = sum(map(len, self.required.values()))
        self.counts["pending_conflicts_ordered"] = 0
        if not self.required:
            return

        # A remote writer and a local consumer share a pool; keep cross-rank paths in this check.
        combined = CausalGraph()
        for graph in graphs.values():
            combined.next.update({event: dict(targets) for event, targets in graph.next.items()})
        for source, target, relation in cross_rank_edges:
            combined.connect(source, target, relation)
        combined.prepare()
        sources = list(self.required)
        for begin in range(0, len(sources), 1024):
            current = sources[begin:begin + 1024]
            masks = combined.reachable_masks(current)
            for bit, source in enumerate(current):
                for target, detail in self.required[source].items():
                    if source != target and masks[combined.positions[target]] & (1 << bit):
                        self.counts["pending_conflicts_ordered"] += 1
                    else:
                        self.findings.append(("kv_lifetime", self.identities[detail["invocation"]], {
                            **detail, "previous_finish": source, "next_start": target,
                            "reason": "overlapping pending KV accesses lack an ordering path",
                        }))

    def report(self):
        return {
            **self.counts,
            "scope": "Prediction-only KV safety: actual physical binding intervals and pending access endpoints, including cross-rank paths. Completed accesses are retired before later submissions. Logical release does not erase pending accesses. Excludes tensor/request-slot lifetimes, native intra-call release timing and proof of unexercised reuse scenarios.",
        }
