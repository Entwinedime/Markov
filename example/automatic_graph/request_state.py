"""Check recorded ownership changes without choosing batches, slots or pages."""

from collections import Counter
from itertools import pairwise


class RequestStateEvidence:
    def __init__(self, result: dict) -> None:
        self.result = result
        settings = result["configuration"]
        self.page_size = settings["page_size"]
        self.pools = settings.get("pd", {"model": settings})
        self.native = result["control_source"] == "native_control"
        if self.native:
            self.pools = {action["task"]["group"]: settings for action in result["decisions"]}
        self.requests = {item["id"]: item for item in result["requests"]}

        self.generated = Counter()
        self.resident = {}
        self.pages = {name: {} for name in self.pools}
        self.slots = {name: {} for name in self.pools}
        self.allocated = Counter()
        self.released = Counter()
        self.failures = Counter()
        self.examples = []

    def require(self, valid: bool, check: str, action: dict, request: str | None = None) -> None:
        if valid:
            return
        self.failures[check] += 1
        if len(self.examples) < 12:
            self.examples.append({"check": check, "task": action["task"], "request": request})

    def pool(self, action: dict) -> str:
        if self.native:
            return action["task"]["group"]
        if "model" in self.pools:
            return "model"
        return "prefill" if action["step"] in {"model", "release"} else "decode"

    def free_pages(self, pool: str) -> int:
        return self.pools[pool]["max_total_tokens"] // self.page_size - len(self.pages[pool])

    def start(self, action: dict) -> None:
        if action["step"] in {"send", "receive", "wait"}:
            return
        pool = self.pool(action)
        allocate = action["step"] in {"batch", "reserve", "model", "prepare"} or (
            action["step"] == "stage" and action["stage"] == 0
        )
        if allocate:
            self.require(action["free_pages_before"] == self.free_pages(pool), "free_pages_before", action)
            self.require(
                action["allocated_pages"] == sum(len(member["new_pages"]) for member in action["members"]),
                "allocation_count", action,
            )

        for member in action["members"]:
            request, slot = member["id"], member["slot"]
            key = (pool, request)
            self.require(
                action["scheduled_ns"] >= self.requests[request]["arrival_ns"], "request_arrival", action, request
            )
            self.require(0 <= slot < self.pools[pool]["max_running_requests"], "slot_bounds", action, request)
            if allocate:

                # Recomputed Prefill includes the output history retained across a residency release.
                expected_tokens = (
                    self.requests[request]["prompt_tokens"] + self.generated[key]
                    if action["phase"] == "prefill" else 1
                )
                self.require(member["new_tokens"] == expected_tokens, "model_input_tokens", action, request)
                if key not in self.resident:
                    self.require(slot not in self.slots[pool], "exclusive_slot", action, request)
                    self.slots[pool][slot] = request
                    self.resident[key] = {"slot": slot, "pages": [], "tokens": 0}
                state = self.resident[key]
                self.require(member["context_tokens"] == state["tokens"], "context_continuity", action, request)
                self.require(
                    member["previous_page"] == (state["pages"][-1] if state["pages"] else None),
                    "previous_page", action, request,
                )
                for page in member["new_pages"]:
                    self.require(page not in self.pages[pool], "exclusive_page", action, request)
                    self.require(
                        0 <= page < self.pools[pool]["max_total_tokens"] // self.page_size,
                        "page_bounds", action, request,
                    )
                    self.pages[pool][page] = request
                    state["pages"].append(page)
                    self.allocated[pool] += 1
                tokens = member["context_tokens"] + member["new_tokens"]
                self.require(
                    len(state["pages"]) == (tokens + self.page_size - 1) // self.page_size,
                    "resident_capacity", action, request,
                )
            self.require(
                key in self.resident
                and self.slots[pool].get(slot) == request
                and self.resident[key]["slot"] == slot,
                "action_ownership", action, request,
            )

        if action["step"] == "transfer":
            request = action["members"][0]["id"]
            copies = action["page_copies"]
            for role, field in (("prefill", "source"), ("decode", "destination")):
                self.require(
                    [item[field] for item in copies] == self.resident[(role, request)]["pages"],
                    "transfer_ownership", action, request,
                )

    def finish(self, action: dict) -> None:
        if action["step"] in {"send", "receive", "wait"}:
            return
        pool = self.pool(action)
        returned = []
        for member in action["members"]:
            request = member["id"]
            key = (pool, request)
            state = self.resident[key]
            if action["step"] in {"reserve", "model"}:
                state["tokens"] = member["context_tokens"] + member["new_tokens"]
            if action["commits_requests"]:
                expected = self.generated[key] + 1
                self.require(member["generated_tokens"] == expected, "token_progress", action, request)
                self.require(
                    member["resident_tokens"] == self.requests[request]["prompt_tokens"] + expected - 1,
                    "resident_tokens", action, request,
                )
                self.require(
                    member["finished"] == (expected == self.requests[request]["output_tokens"]),
                    "request_completion", action, request,
                )
                self.generated[key] = member["generated_tokens"]
                state["tokens"] = member["resident_tokens"]

            # Intermediate PP snapshots carry planned effects; only completion owners may commit them.
            if action["commits_requests"] or action["step"] == "release":
                pages = member["released_pages"]
                self.require(
                    Counter(pages) == Counter(state["pages"] if member["finished"] else []),
                    "release_ownership", action, request,
                )
                if member["finished"]:
                    returned.extend(pages)
                    self.released[pool] += len(pages)
                    for page in state["pages"]:
                        del self.pages[pool][page]
                    del self.slots[pool][state["slot"]]
                    del self.resident[key]

        self.require(Counter(action["returned_pages"]) == Counter(returned), "actual_page_return", action)

        # The public scalar is the decode pool for PD, including when a prefill action completes.
        if not self.native or len(self.pools) == 1:
            result_pool = "decode" if "decode" in self.pools else pool
            self.require(action["free_pages_after"] == self.free_pages(result_pool), "free_pages_after", action)

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
            if event == "submitted":
                self.start(actions[index])
            else:
                self.finish(actions[index])

        progress_pools = ("decode",) if "decode" in self.pools else self.pools
        expected = Counter({
            (pool, name): request["output_tokens"] for pool in progress_pools
            for name, request in self.requests.items()
        })
        completed = self.generated == expected
        free = (
            self.result["free_pages"] == sum(self.free_pages(pool) for pool in self.pools)
            if self.native else self.result.get("pool_free_pages", {"model": self.result["free_pages"]})
            == {name: self.free_pages(name) for name in self.pools}
        )
        recovered = (
            not self.resident and not any(self.pages.values()) and not any(self.slots.values())
            and free
        )
        return {
            "status": "matched" if not self.failures and completed and recovered else "differences",
            "scope": "cold KV, complete or recomputed prefill, explicit residency releases, no prefix cache",
            "state_events": len(events),
            "completed_requests": completed,
            "recovered_ownership": recovered,
            "allocated_pages": dict(self.allocated),
            "released_pages": dict(self.released),
            "failures": dict(self.failures),
            "examples": self.examples,
            "aggregate_snapshot_scope": (
                "final recovery only; native allocations can precede another group's graph submission"
                if self.native and len(self.pools) > 1 else "every completed action"
            ),
        }
