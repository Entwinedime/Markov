"""Check recorded resource mutations; do not choose cache matches, victims or allocations."""

from collections import Counter
from itertools import pairwise

from execution import DependencyPaths
from operands import ServingOperandEvidence
from request_state import RequestStateEvidence


class ResourceHistoryEvidence:
    def __init__(self, result: dict, ownership: RequestStateEvidence) -> None:
        self.result = result
        self.pools = {}
        self.returned = {}
        self.released = {}
        self.failures = Counter()
        self.examples = []
        self.actions = {(row["task"]["group"], row["task"]["sequence"]): row for row in result["decisions"]}
        self.ownership = ownership
        self.request_ids = {request["id"]: index for index, request in enumerate(result["requests"])}
        self.consumers = {}
        self.uses = Counter()
        self.operands = ServingOperandEvidence(result)
        self.release_paths = []
        self.frontiers = {}

        # Host return can precede device completion; retain the actual device intervals for each action.
        graph = result["graph"]
        model_regions = {region["id"] for models in self.operands.models.values() for region in models}
        for region in graph["regions"]:
            if region["parent"] in model_regions:
                model_regions.add(region["id"])
        transfers = {key for key, action in self.actions.items() if action["step"] == "transfer"}
        timeline = {item["node"]: item for item in result["execution_timeline"]}
        self.work = {}
        nodes = [
            node
            for node in graph["execution"]["nodes"]
            if node["kind"] in {"device", "transfer", "copy", "communication"}
        ]
        order = DependencyPaths.resource_order(graph["execution"], nodes)
        for node in nodes:
            region = node.get("region")
            if region is None:
                region = graph["operations"][node["operation"]]["region"]
            task = graph["regions"][region].get("task")
            if task is not None:
                key = task["group"], task["sequence"]

                # Combined actions also contain pool maintenance; only model and KV transfer work consume KV pages.
                if region not in model_regions and key not in transfers:
                    continue
                self.work.setdefault(key, []).append(timeline[node["id"]])
                streams = self.frontiers.setdefault(key, {})
                resource = node["resource"]
                if resource not in streams or order[node["id"]] > order[streams[resource]]:
                    streams[resource] = node["id"]

    def use(self, action: dict) -> None:
        if action.get("channel") or action["step"] == "indices":
            return
        pool = self.ownership.pool(action)
        event = {"pool": pool, "kind": action["step"], "order": action["submitted_order"]}
        if action["step"] == "select":
            for member in action["members"]:
                request = self.pools[pool]["requests"][self.request_ids[member["id"]]]
                self.require(request["slot"] is not None, "selection_residence", event)
            return

        allocate = self.ownership.allocates(action)
        for member in action["members"]:
            request = self.pools[pool]["requests"][self.request_ids[member["id"]]]
            self.require(request["slot"] == member["slot"], "action_ownership", event)
            if allocate:
                expected = request["allocation"]
                self.require(all(member[key] == value for key, value in expected.items()), "allocation_binding", event)
        if allocate:
            self.require(
                action["allocated_pages"] == sum(len(member["new_pages"]) for member in action["members"]),
                "allocation_count",
                event,
            )
            first = self.request_ids[action["members"][0]["id"]]
            self.require(
                action["free_pages_before"] == self.pools[pool]["requests"][first]["free_before"],
                "free_pages_before",
                event,
            )

        if action["step"] not in {"batch", "model", "compute", "stage", "transfer"}:
            return
        task = action["task"]
        key = task["group"], task["sequence"]
        work = self.work.get(key, [])
        complete = bool(work) and all(item["status"] == "completed" for item in work)
        end = max(item["completed_ns"] for item in work) if complete else None
        pages = []
        if action["step"] == "transfer":
            binding = action["transfer"] if self.ownership.native else {"source": "prefill", "destination": "decode"}
            member = action["members"][0]
            begin = member["context_tokens"] // self.ownership.page_size
            copies = action["page_copies"]
            for side, name in binding.items():
                mapped = self.pools[name]["requests"][self.request_ids[member["id"]]]["pages"]
                self.require(
                    [pair[side] for pair in copies] == mapped[begin : begin + len(copies)], "transfer_ownership", event
                )
            pages = [(pool, pair[side]) for pair in action["page_copies"] for side, pool in binding.items()]
        else:
            pool = self.ownership.pool(action)
            state = self.pools[pool]["requests"]
            self.operands.observe(
                action, {member["id"]: state[self.request_ids[member["id"]]] for member in action["members"]}
            )
            for member in action["members"]:
                request = state[self.request_ids[member["id"]]]
                tokens = member["context_tokens"] + member["new_tokens"]
                count = (tokens + self.ownership.page_size - 1) // self.ownership.page_size
                self.require(
                    request["slot"] == member["slot"]
                    and request["tokens"] >= tokens
                    and len(request["pages"]) >= count,
                    "device_consumer_residence",
                    event,
                )
                pages.extend((pool, page) for page in request["pages"][:count])

        self.require(all(self.pools[name]["free"][page] == 0 for name, page in pages), "device_pages_allocated", event)

        # This deliberately covers the whole model work, including asynchronous device tails.
        for page in pages:
            self.consumers.setdefault(page, {})[key] = end
        self.uses[action["step"]] += 1

    def require(self, valid: bool, check: str, event: dict) -> None:
        if valid:
            return
        self.failures[check] += 1
        if len(self.examples) < 12:
            self.examples.append(
                {"check": check, "pool": event["pool"], "kind": event["kind"], "order": event["order"]}
            )

    def finish(self, action: dict) -> None:

        # The public scalar describes the Decode pool for lightweight PD; native multi-pool control is aggregate.
        if self.ownership.native:
            pool, states = "all", self.pools.values()
        else:
            pool = "decode" if "decode" in self.pools else self.ownership.pool(action)
            states = (self.pools[pool],)

        self.require(
            action["free_pages_after"] == sum(sum(state["free"].values()) for state in states),
            "free_pages_after",
            {"pool": pool, "kind": action["step"], "order": action["completed_order"]},
        )

    def apply(self, event: dict) -> None:
        pool = event["pool"]
        kind = event["kind"]
        if kind == "pool_init":
            self.require(pool not in self.pools, "one_initialization", event)
            self.pools[pool] = {
                "free": Counter(range(event["pages"])),
                "slots": Counter(range(event["slots"])),
                "requests": {},
                "prefixes": {},
            }
            return

        state = self.pools[pool]
        free, slots, requests, nodes = (state[key] for key in ("free", "slots", "requests", "prefixes"))
        match kind:
            case "slots":
                self.require(not requests, "slot_capacity_before_use", event)
                state["slots"] = Counter(range(event["slots"]))
            case "prefix_touch":
                node = nodes[event["node"]]
                self.require(event["recency"] >= node["recency"], "recency_order", event)
                node["recency"] = event["recency"]
            case "prefix_insert":
                cached = {page for node in nodes.values() for page in node["pages"]}
                pages = event["pages"]
                self.require(event["node"] not in nodes, "unique_prefix", event)
                self.require(bool(pages) and len(set(pages)) == len(pages), "prefix_pages", event)
                self.require(
                    not cached.intersection(pages) and not any(free[p] for p in pages), "cached_page_ownership", event
                )
                self.require(event["parent"] == 0 or event["parent"] in nodes, "prefix_parent", event)
                nodes[event["node"]] = {key: event[key] for key in ("parent", "pages", "recency")}
                nodes[event["node"]]["references"] = 0
            case "prefix_split":
                child = nodes[event["node"]]
                pages = event["pages"]
                self.require(
                    event["prefix"] not in nodes and child["parent"] == event["parent"], "split_identity", event
                )
                self.require(
                    0 < len(pages) < len(child["pages"]) and child["pages"][: len(pages)] == pages, "split_pages", event
                )
                self.require(child["references"] == event["references"], "split_inherits_references", event)
                nodes[event["prefix"]] = {key: event[key] for key in ("parent", "pages", "recency", "references")}
                child["parent"] = event["prefix"]
                child["pages"] = child["pages"][len(pages) :]
            case "prefix_lock":
                delta = 1 if event["acquire"] else -1
                path = event["nodes"]
                for index, change in enumerate(path):
                    node = nodes[change["node"]]
                    self.require(
                        change["references"] == node["references"] + delta and change["references"] >= 0,
                        "reference_delta",
                        event,
                    )
                    parent = path[index + 1]["node"] if index + 1 < len(path) else 0
                    self.require(node["parent"] == parent, "reference_path", event)
                    node["references"] = change["references"]
            case "prefix_evict":
                node = nodes[event["node"]]
                parents = {item["parent"] for item in nodes.values()}
                self.require(node["references"] == 0 and event["node"] not in parents, "evict_unlocked_leaf", event)
                eligible = [
                    item["recency"]
                    for identity, item in nodes.items()
                    if item["references"] == 0 and identity not in parents
                ]
                self.require(bool(eligible) and node["recency"] == min(eligible), "evict_oldest_leaf", event)
                del nodes[event["node"]]
            case "bind":
                request = requests.setdefault(event["request"], {"slot": None, "tokens": 0, "pages": []})
                path, identity = [], event["node"]
                while identity:
                    node = nodes[identity]
                    self.require(node["references"] > 0, "bound_prefix_protected", event)
                    path.append(node["pages"])
                    identity = node["parent"]
                cached = [page for pages in reversed(path) for page in pages]
                tail = request["pages"][len(cached) :]
                self.require(event["pages"] == cached + tail, "bound_prefix_and_tail", event)
                self.require(not any(free[page] for page in event["pages"]), "bound_pages_allocated", event)
                request.update(tokens=event["tokens"], pages=event["pages"].copy())
            case "reserve":
                request = requests.setdefault(event["request"], {"slot": None, "tokens": 0, "pages": []})
                request["free_before"] = sum(free.values())
                request["allocation"] = {
                    "context_tokens": request["tokens"],
                    "new_tokens": event["tokens"] - request["tokens"],
                    "previous_page": request["pages"][-1] if request["pages"] else None,
                    "new_pages": event["pages"],
                }
                if request["slot"] is None:
                    self.require(slots[event["slot"]] == 1, "exclusive_slot", event)
                    slots[event["slot"]] -= 1
                else:
                    self.require(request["slot"] == event["slot"], "stable_slot", event)
                pages = event["pages"]
                self.require(
                    len(set(pages)) == len(pages) and all(free[page] == 1 for page in pages),
                    "allocate_free_pages",
                    event,
                )
                free.subtract(pages)
                request["pages"].extend(pages)
                request.update(slot=event["slot"], tokens=event["tokens"])
                expected = (event["tokens"] + self.ownership.page_size - 1) // self.ownership.page_size
                self.require(len(request["pages"]) == expected, "resident_capacity", event)
            case "release":
                request = requests.pop(event["request"])
                self.require(request["slot"] is not None and slots[request["slot"]] == 0, "release_owned_slot", event)
                slots[request["slot"]] += 1
                task = event["task"]
                self.released.setdefault((task["group"], task["sequence"]), []).append(event["request"])
            case "return_pages":
                pages = event["pages"]
                cached = {page for node in nodes.values() for page in node["pages"]}
                self.require(not cached.intersection(pages), "return_uncached_pages", event)
                consumers = set()
                for page in pages:
                    users = self.consumers.pop((pool, page), {})
                    consumers.update(users)
                    self.require(
                        all(end is not None and end <= event["time_ns"] for end in users.values()),
                        "return_after_device_consumers",
                        event,
                    )

                # All completion ports must finish before return; each device frontier must reach one of them.
                task = event["task"]
                ports = self.actions[task["group"], task["sequence"]]["execution_ports"]["completions"]
                for consumer in consumers:
                    self.release_paths.extend((node, ports, event) for node in self.frontiers[consumer].values())

                self.require(
                    len(set(pages)) == len(pages) and not any(free[page] for page in pages),
                    "return_allocated_pages",
                    event,
                )
                free.update(pages)
                self.returned.setdefault((task["group"], task["sequence"]), Counter()).update(pages)
            case _:
                raise ValueError(f"Unknown resource mutation: {kind}")

        # Reuse and slot release must come from the actual graph completion callback, never output publication.
        if kind in {"return_pages", "release"}:
            self.require(event["phase"] == "complete", "release_on_completion", event)
        if "task" in event:
            task = event["task"]
            action = self.actions[task["group"], task["sequence"]]
            field = "completed" if event["phase"] == "complete" else "submitted"
            time = action["completed_ns"] if field == "completed" else action["scheduled_ns"]
            self.require(
                event["order"] == action[field + "_order"] and event["time_ns"] == time, "action_boundary", event
            )

    def check(self) -> dict:
        events = self.result["resource_history"]
        if any(
            left["order"] > right["order"] or left["time_ns"] > right["time_ns"] for left, right in pairwise(events)
        ):
            return {"status": "differences", "failures": {"resource_event_order": 1}}
        starts = {action["submitted_order"]: action for action in self.actions.values()}
        ends = {action["completed_order"]: action for action in self.actions.values()}
        changes = {}
        for event in events:
            changes.setdefault(event["order"], []).append(event)
        for order in sorted(changes.keys() | starts.keys() | ends.keys()):
            for event in changes.get(order, []):
                self.apply(event)
            if order in starts:
                self.use(starts[order])
            if order in ends:
                self.finish(ends[order])
        terminal = {}
        for pool, state in self.pools.items():
            expected = self.result["resources"][pool]["ownership"]
            prefixes = {
                identity: {"node": identity, "pages": node["pages"], "references": node["references"]}
                for identity, node in state["prefixes"].items()
            }
            terminal[pool] = (
                state["free"] == Counter(expected["free_pages"])
                and state["slots"] == Counter(expected["free_slots"])
                and not state["requests"]
                and not expected["requests"]
                and prefixes == {node["node"]: node for node in expected["prefixes"]}
            )
        returns = all(
            self.returned.get(task, Counter()) == Counter(action["returned_pages"])
            for task, action in self.actions.items()
        )
        releases = all(
            Counter(self.released.get(task, []))
            == Counter(self.request_ids[member["id"]] for member in action["members"] if member["finished"])
            for task, action in self.actions.items()
            if action["commits_requests"] or action["step"] == "release"
        ) and all(
            self.actions[task]["commits_requests"] or self.actions[task]["step"] == "release" for task in self.released
        )
        operands = self.operands.check()
        missing = DependencyPaths(self.result["graph"]["execution"]).missing(
            {(node, port) for node, ports, _ in self.release_paths for port in ports}
        )
        for node, ports, event in self.release_paths:
            self.require(any((node, port) not in missing for port in ports), "return_after_device_paths", event)

        return {
            "status": "matched"
            if not self.failures and all(terminal.values()) and returns and releases and operands["status"] == "matched"
            else "differences",
            "scope": (
                "observed resource mutations, device-use intervals and paths to page-return completion ports; "
                "not independent KV content or completeness of declared device consumers"
            ),
            "events": dict(Counter(event["kind"] for event in events)),
            "device_consumer_actions": dict(self.uses),
            "release_device_frontiers": len(self.release_paths),
            "terminal": terminal,
            "returned_pages": returns,
            "released_requests": releases,
            "model_operands": operands,
            "failures": dict(self.failures),
            "examples": self.examples,
        }
