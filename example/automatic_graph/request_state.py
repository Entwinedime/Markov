"""Check recorded ownership changes without choosing batches, slots or pages."""

from collections import Counter, defaultdict
from itertools import combinations, pairwise


class ResourceStateEvidence:
    """Check terminal pool ownership; cached pages may remain after requests finish."""

    def __init__(self, resources: dict) -> None:
        self.resources = resources

    def summary_checks(self) -> dict[str, bool]:
        return {
            "page_balance": all(
                pool["free_pages"] + pool["evictable_pages"] + pool["protected_pages"] == pool["capacity"]["pages"]
                and 0 <= pool["deferred_free_pages"] <= pool["free_pages"]
                and pool["evictable_pages"] >= 0
                for pool in self.resources.values()
            ),
            "request_release": all(
                pool["free_slots"] == pool["capacity"]["slots"] and pool["resident_requests"] == 0
                for pool in self.resources.values()
            ),
            "cache_unlocked": all(pool["protected_pages"] == 0 for pool in self.resources.values()),
        }

    def check(self) -> dict:
        checks = self.summary_checks()
        pools = {}
        for name, pool in self.resources.items():
            facts = pool["ownership"]
            prefixes = facts["prefixes"]
            cached = [page for prefix in prefixes for page in prefix["pages"]]
            free = facts["free_pages"]
            slots = facts["free_slots"]
            evictable = sum(len(prefix["pages"]) for prefix in prefixes if prefix["references"] == 0)
            protected = sum(len(prefix["pages"]) for prefix in prefixes if prefix["references"] > 0)
            pools[name] = {
                "page_ownership": Counter(free + cached) == Counter(range(pool["capacity"]["pages"])),
                "slot_ownership": Counter(slots) == Counter(range(pool["capacity"]["slots"])),
                "cache_references": all(prefix["references"] == 0 for prefix in prefixes),
                "cache_identity": len({prefix["node"] for prefix in prefixes}) == len(prefixes),
                "counts_match": len(free) == pool["free_pages"]
                and len(slots) == pool["free_slots"]
                and evictable == pool["evictable_pages"]
                and protected == pool["protected_pages"]
                and len(facts["requests"]) == pool["resident_requests"],
                "no_request_residences": not facts["requests"],
            }
        return {
            "status": "matched"
            if all(checks.values()) and all(all(p.values()) for p in pools.values())
            else "differences",
            "scope": "terminal ownership only; intermediate cache state and device readiness are not checked",
            "checks": checks,
            "pools": pools,
        }


class IndexWorkEvidence:
    """Compare rank-local bindings of the same standalone cache action."""

    def __init__(self, result: dict) -> None:
        self.result = result

    def check(self) -> dict:
        graph = self.result["graph"]
        groups = {group["id"]: group["members"] for group in self.result["structure"]["topology"]["groups"]}
        actions = {
            (action["task"]["group"], action["task"]["sequence"]): action
            for action in self.result["decisions"]
            if action["step"] == "indices"
        }
        completions = {port for action in actions.values() for port in action["execution_ports"]["completions"]}
        completion_ranks = {}
        for node in graph["execution"]["nodes"]:
            if node["id"] in completions:
                region = node.get("region")
                if region is None:
                    region = graph["operations"][node["operation"]]["region"]
                completion_ranks[node["id"]] = graph["regions"][region]["rank"]

        calls = defaultdict(lambda: defaultdict(list))
        stores = defaultdict(set)
        for region in graph["regions"]:
            task = region.get("task")
            if task is None or "entrypoint" not in region:
                continue
            key = task["group"], task["sequence"]
            if key not in actions:
                continue

            # Compare index contents and geometry, while preserving each rank's physical storage identity.
            rank = region["rank"]
            ports = {}
            for direction, bindings in region["ports"].items():
                ports[direction] = {}
                for name, identity in bindings.items():
                    value = graph["values"][identity]
                    ports[direction][name] = {
                        field: value[field] for field in ("dtype", "shape", "integer_data") if field in value
                    }
                    if "storage" in value and value["device"] != "cpu":
                        stores[rank].add(value["storage"])
            calls[key][rank].append((region["entrypoint"], region["parameters"], ports))

        failures = []
        for key, action in actions.items():
            members = groups[key[0]]
            present = set(calls[key]) == set(members)
            checks = {
                "rank_calls": present and all(calls[key][rank] == calls[key][members[0]] for rank in members),
                "completion_per_rank": Counter(
                    completion_ranks[port] for port in action["execution_ports"]["completions"]
                )
                == Counter(members),
            }
            if not all(checks.values()):
                failures.append({"task": action["task"], "checks": checks})

        # Stage-local index storage remains separate across actions as well as across TP peers.
        for left, right in combinations(stores, 2):
            if not stores[left].isdisjoint(stores[right]):
                failures.append({"ranks": [left, right], "check": "separate_device_storage"})
        return {
            "status": "matched" if not failures else "differences",
            "scope": "standalone cache actions: group bindings/completions and separate index storage across ranks",
            "actions": len(actions),
            "failures": failures,
        }


class RequestStateEvidence:
    """Check request progress independently of cold or shared KV page ownership."""

    def __init__(self, result: dict) -> None:
        self.result = result
        self.page_size = result["configuration"]["page_size"]
        self.native = result["control_source"] == "native_control"
        self.pools = {name: pool["capacity"] for name, pool in result["resources"].items()}
        groups = {group["id"]: group for group in result["structure"]["topology"]["groups"]}
        serving = {
            rank: group["id"] for group in groups.values() if group["role"] == "serving" for rank in group["members"]
        }
        self.instances = {name: serving[groups[name]["members"][0]] for name in self.pools}
        self.requests = {item["id"]: item for item in result["requests"]}
        self.generated = Counter()
        self.resident = {}
        self.failures = Counter()
        self.examples = []

    def require(self, valid: bool, check: str, action: dict, request: str) -> None:
        if valid:
            return
        self.failures[check] += 1
        if len(self.examples) < 12:
            self.examples.append({"check": check, "task": action["task"], "request": request})

    def pool(self, action: dict) -> str:
        if self.native:
            return action["transfer"]["destination"] if action["step"] == "transfer" else action["task"]["group"]
        if "model" in self.pools:
            return "model"
        return "prefill" if action["step"] in {"model", "release"} else "decode"

    @staticmethod
    def allocates(action: dict) -> bool:
        return action["step"] in {"batch", "reserve", "model", "prepare"} or (
            action["step"] == "stage" and action["stage"] == 0
        )

    def start(self, action: dict) -> None:
        pool = self.pool(action)
        for member in action["members"]:
            request = member["id"]
            key = pool, request
            self.require(
                action["scheduled_ns"] >= self.requests[request]["arrival_ns"], "request_arrival", action, request
            )
            if not self.allocates(action):
                continue

            # Recomputed Prefill includes retained output history; only explicit releases restart the input range.
            context = member["context_tokens"]
            remaining = self.requests[request]["prompt_tokens"] + self.generated[key] - context
            valid_tokens = (
                0 < member["new_tokens"] <= remaining if member["phase"] == "prefill" else member["new_tokens"] == 1
            )
            self.require(valid_tokens, "model_input_tokens", action, request)

            # A first range can start at a shared prefix; resource history checks its actual binding.
            if key in self.resident:
                self.require(context == self.resident[key], "context_continuity", action, request)
            self.resident[key] = context + member["new_tokens"]

    def finish(self, action: dict) -> None:
        pool = self.pool(action)
        for member in action["members"]:
            request = member["id"]
            key = pool, request
            if action["commits_requests"]:
                resident = member["context_tokens"] + member["new_tokens"]
                input_end = self.requests[request]["prompt_tokens"] + self.generated[key]
                continues = resident < input_end
                expected = self.generated[key] + (0 if continues else 1)
                self.require(member["generated_tokens"] == expected, "token_progress", action, request)
                self.require(member["continues_prefill"] == continues, "prefill_completion", action, request)
                self.require(
                    member["resident_tokens"] == resident and resident <= input_end,
                    "resident_tokens",
                    action,
                    request,
                )
                self.require(
                    member["finished"]
                    == (self.instances[pool] != "prefill" and expected == self.requests[request]["output_tokens"]),
                    "request_completion",
                    action,
                    request,
                )
                self.generated[key] = member["generated_tokens"]

            # PP completion snapshots must not overwrite a range already reserved for the next chunk.
            if (action["commits_requests"] or action["step"] == "release") and member["finished"]:
                del self.resident[key]

    def check(self) -> dict:
        actions = self.result["decisions"]
        events = sorted(
            (action[field + "_order"], action[time_field], field, index)
            for index, action in enumerate(actions)
            for field, time_field in (("submitted", "scheduled_ns"), ("completed", "completed_ns"))
        )
        ordered = (
            [event[0] for event in events] == list(range(len(events)))
            and all(left[1] <= right[1] for left, right in pairwise(events))
            and all(action["submitted_order"] < action["completed_order"] for action in actions)
        )
        if not ordered:
            return {"status": "differences", "failures": {"state_event_order": 1}}

        for _, _, event, index in events:
            action = actions[index]
            if action.get("channel") or action["step"] == "indices":
                continue
            if event == "submitted":
                self.start(action)
            else:
                self.finish(action)

        progress_pools = {name for name, instance in self.instances.items() if instance != "prefill"}
        expected = Counter(
            {
                (pool, name): request["output_tokens"]
                for pool in progress_pools
                for name, request in self.requests.items()
            }
        )
        progress = Counter({key: count for key, count in self.generated.items() if key[0] in progress_pools})
        completed = progress == expected and not self.resident
        return {
            "status": "matched" if not self.failures and completed else "differences",
            "scope": "request arrival, contiguous input ranges, recomputation and committed output progress",
            "state_events": len(events),
            "completed_requests": completed,
            "failures": dict(self.failures),
            "examples": self.examples,
        }
