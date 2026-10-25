"""Check native API boundaries and completion links in a matched logical window."""

from __future__ import annotations

from collections import Counter, defaultdict
from typing import Any


class ExecutionComparison:
    api_kinds = {
        "copy",
        "copy_async",
        "wait_stream",
        "wait_device",
        "record_event",
        "wait_event",
        "wait_event_complete",
        "unclassified",
    }

    def __init__(self, native: list[dict[str, Any]], target: list[dict[str, Any]], execution: dict[str, Any]) -> None:
        self.native = native
        self.positions = {operation["id"]: position for position, operation in enumerate(target)}
        self.execution = execution
        self.nodes = [node for node in execution["nodes"] if node.get("operation") in self.positions]
        self.order = self.resource_order()

    @staticmethod
    def issue(action: dict[str, Any]) -> tuple[int, int]:
        submission = action["submission"]
        queue = submission["queue"]
        return (
            (queue["producer_thread"], queue["enqueue_order"])
            if queue is not None
            else (submission["thread"], submission["order"])
        )

    def compare(self) -> dict[str, Any]:
        report = self.events()
        queues = self.queue_frontiers()
        streams = self.stream_frontiers()
        checks = report["same"], queues["same"], streams["same"]
        if False in checks:
            same = False
        elif None in checks:
            same = None
        else:
            same = True
        return {
            **report,
            "same": same,
            "status": "matched" if same else "partial" if same is None else "differences",
            "queue_frontiers": queues,
            "stream_frontiers": streams,
            "scope": ("API inventory, event/stream completion and queue drain; host service duration is separate"),
        }

    def resource_order(self) -> dict[int, int]:

        # Construction IDs can differ from native execution order on a thread or stream.
        previous = {
            edge["target"]: edge["source"]
            for edge in self.execution["dependencies"]
            if edge["kind"] == "resource_order"
        }
        order: dict[int, int] = {}
        for node in self.nodes:
            cursor = node["id"]
            path = []
            visiting = set()
            while cursor in previous and cursor not in order:
                if cursor in visiting:
                    raise ValueError("execution resource order is cyclic")
                visiting.add(cursor)
                path.append(cursor)
                cursor = previous[cursor]
            order.setdefault(cursor, 0)
            for entry in reversed(path):
                order[entry] = order[previous[entry]] + 1
        return order

    def queue_frontiers(self) -> dict[str, Any]:
        items = {}
        waits = []
        for position, operation in enumerate(self.native):
            observation = operation["execution_observation"]
            for action in observation["device_work"] + observation["runtime_actions"]:
                submission = action["submission"]
                if submission is not None and (queue := submission["queue"]) is not None:
                    identity = queue["process"], queue["correlation"]
                    item = (
                        position,
                        submission["thread"],
                        queue["producer_thread"],
                        queue["enqueue_order"],
                        queue["dequeue_order"],
                    )
                    if identity in items and items[identity] != item:
                        return {"same": None, "reason": "one native queue item spans different logical owners"}
                    items[identity] = item
                if action.get("wait_frontiers") is not None and action["wait_frontiers"]["queues"]:
                    waits.append((position, action))

        native_groups = defaultdict(dict)
        target_groups = defaultdict(dict)
        for item in items.values():
            native_groups[item[0]].setdefault(item[1], []).append(item)
        for node in self.nodes:
            if node["kind"] == "queue_return":
                target_groups[self.positions[node["operation"]]].setdefault(node["resource"], []).append(node)

        bindings = defaultdict(set)
        reverse_bindings = defaultdict(set)
        mapped_items = {}
        for position in range(len(self.native)):
            left = list(native_groups[position].items())
            right = list(target_groups[position].items())
            if not left and not right:
                continue
            if len(left) > 1 or len(right) > 1:
                return {"same": None, "reason": "multiple queue workers within one owner need an explicit binding"}
            if not left or not right or len(left[0][1]) != len(right[0][1]):
                return {"same": False, "reason": "queue item count differs", "operation_position": position}
            worker, native = left[0]
            resource, target = right[0]
            bindings[worker].add(resource)
            reverse_bindings[resource].add(worker)
            for item, node in zip(
                sorted(native, key=lambda item: item[4]),
                sorted(target, key=lambda node: self.order[node["id"]]),
                strict=True,
            ):
                mapped_items[item] = node["id"]

        if any(len(group) != 1 for group in (*bindings.values(), *reverse_bindings.values())):
            return {"same": False, "reason": "native queue worker and target resource bindings disagree"}

        entries = defaultdict(list)
        for node in self.nodes:
            if node["kind"] == "wait_entry":
                entries[self.positions[node["operation"]], node["name"]].append(node["id"])
        ordinals = Counter()
        required = set()
        external_waits = 0
        for position, action in waits:
            signature = position, action["native_api"]
            ordinal = ordinals[signature]
            ordinals[signature] += 1
            if ordinal >= len(entries[signature]):
                return {"same": False, "reason": "native draining wait has no target entry", "wait": signature}
            submission = action["submission"]
            for worker in action["wait_frontiers"]["queues"]:
                visible = [
                    item
                    for item in items.values()
                    if item[1] == worker and item[2] == submission["thread"] and item[3] < submission["order"]
                ]
                if not visible:
                    external_waits += 1
                    continue

                # A drain waits for the item's CPU return, not merely its last device submission.
                frontier = max(visible, key=lambda item: item[3])
                required.add((mapped_items[frontier], entries[signature][ordinal]))

        entry_ids = {entry for group in entries.values() for entry in group}
        actual = {
            (edge["source"], edge["target"])
            for edge in self.execution["dependencies"]
            if edge["kind"] == "queue_drain" and edge["target"] in entry_ids
        }
        same = required == actual
        return {
            "same": None if same and external_waits else same,
            "native_queue_items": len(items),
            "draining_waits": len(waits),
            "queue_completion_links": len(required),
            "missing_queue_links": len(required - actual),
            "extra_queue_links": len(actual - required),
            "external_queue_waits": external_waits,
        }

    def stream_item(
        self, position: int, kind: str, name: str, stream: int, submission: dict[str, Any], secondary: int = 0
    ) -> None:
        self.streams[stream].append(
            {
                "key": (position, kind, name),
                "issue": self.issue({"submission": submission}),
                "actor": submission["thread"],
                "order": submission["order"],
                "secondary": secondary,
            }
        )

    def stream_frontiers(self) -> dict[str, Any]:
        self.streams: dict[int, list[dict[str, Any]]] = defaultdict(list)
        waits = defaultdict(list)
        for position, operation in enumerate(self.native):
            observation = operation["execution_observation"]
            actions = observation["runtime_actions"]
            if any(item["submission"] is None for item in observation["device_work"] + actions):
                return {"same": None, "reason": "stream completion needs native submission identities"}
            if any(
                action["target"] is None
                or (action["kind"] in ("wait_stream", "wait_device") and action["wait_frontiers"] is None)
                for action in actions
                if action["kind"] in self.api_kinds - {"copy", "unclassified"}
            ):
                return {"same": None, "reason": "stream target or wait frontier was not observed"}

            event_submissions = {
                item["submission"]["source_event"]
                for item in actions
                if item["kind"] in ("record_event", "wait_event", "wait_event_complete")
            }
            observed_submissions = {item["submission"]["source_event"] for item in observation["device_work"]}
            transport = []
            for item in observation["device_work"]:
                if item["submission"]["source_event"] in event_submissions:
                    continue
                if item.get("communication", False):
                    transport.append(item)
                    continue
                self.stream_item(
                    position, item["kind"], item["name"], item["stream"], item["submission"], item["order"]
                )

            # A closed collective exposes its last transport completion; its entry only announces readiness.
            if transport:
                item = max(transport, key=lambda item: item["order"])
                self.stream_item(position, "barrier", "collective_exit", item["stream"], item["submission"])
            for action in actions:
                target = action["target"]
                if action["kind"] in ("record_event", "wait_event"):
                    self.stream_item(
                        position, "barrier", action["native_api"], target["identity"], action["submission"]
                    )
                elif (
                    action["kind"] == "copy_async" and action["submission"]["source_event"] not in observed_submissions
                ):
                    self.stream_item(position, "transfer", "transfer", target["identity"], action["submission"])
                elif action["kind"] in ("wait_stream", "wait_device"):
                    waits[position, action["kind"], action["native_api"]].append(action)

        target_streams = defaultdict(list)
        target_waits = defaultdict(list)
        for node in self.nodes:
            position = self.positions[node["operation"]]
            if self.execution["resources"][node["resource"]] == "stream" and node["name"] != "collective_entry":
                target_streams[node["resource"]].append(node)
            if node["kind"] in ("wait_stream", "wait_device"):
                target_waits[position, node["kind"], node["name"]].append(node)
        for items in target_streams.values():
            items.sort(key=lambda node: self.order[node["id"]])

        bindings = {}
        mapped = {}
        for stream, items in self.streams.items():
            if len({item["issue"][0] for item in items}) != 1:
                return {"same": None, "reason": "stream work has multiple logical producers", "stream": stream}
            items.sort(key=lambda item: (item["issue"][1], item["order"], item["secondary"]))
            keys = [item["key"] for item in items]
            candidates = [
                resource
                for resource, nodes in target_streams.items()
                if keys == [(self.positions[node["operation"]], node["kind"], node["name"]) for node in nodes]
            ]
            if len(candidates) != 1:
                return {
                    "same": None if candidates else False,
                    "reason": "native stream sequence has no unique target binding",
                    "stream": stream,
                    "native_items": len(items),
                    "candidate_count": len(candidates),
                }
            (resource,) = candidates
            bindings[stream] = resource
            for index, node in enumerate(target_streams[resource]):
                mapped[stream, index] = node["id"]
        if len(set(bindings.values())) != len(bindings) or set(bindings.values()) != set(target_streams):
            return {"same": False, "reason": "native and target stream inventories differ"}

        required = set()
        initial = set()
        native_waits = 0
        kinds = Counter()
        if waits.keys() != target_waits.keys():
            return {"same": False, "reason": "wait owners or APIs differ"}
        for signature, actions in waits.items():
            if len({action["submission"]["thread"] for action in actions}) != 1:
                return {"same": None, "reason": "wait occurrences have multiple native actors", "owner": signature}
            left = sorted(actions, key=lambda action: action["submission"]["order"])
            right = sorted(target_waits[signature], key=lambda node: self.order[node["id"]])
            if len(left) != len(right):
                return {"same": False, "reason": "wait occurrence counts differ", "owner": signature}
            for action, node in zip(left, right, strict=True):
                native_waits += 1
                kinds[action["kind"]] += 1
                submission = action["submission"]
                issued = self.issue(action)
                if action["kind"] == "wait_device" and self.execution["resources"][node["target_resource"]] != "device":
                    return {
                        "same": False,
                        "reason": "device wait targets a different resource kind",
                        "owner": signature,
                    }
                for stream in action["wait_frontiers"]["streams"]:
                    if stream not in bindings:
                        return {
                            "same": None,
                            "reason": "wait references a stream outside the selected window",
                            "stream": stream,
                        }
                    if action["kind"] == "wait_stream" and node["target_resource"] != bindings[stream]:
                        return {
                            "same": False,
                            "reason": "stream wait targets a different execution resource",
                            "owner": signature,
                        }
                    visible = [
                        index
                        for index, item in enumerate(self.streams[stream])
                        if (
                            item["order"] < submission["order"]
                            if item["actor"] == submission["thread"]
                            else item["issue"][0] == issued[0] and item["issue"][1] < issued[1]
                        )
                    ]
                    if visible:
                        required.add((mapped[stream, visible[-1]], node["id"]))
                    else:
                        initial.add((bindings[stream], node["id"]))

        wait_ids = {node["id"] for items in target_waits.values() for node in items}
        actual = {
            (edge["source"], edge["target"])
            for edge in self.execution["dependencies"]
            if edge["kind"] == "wait_completion" and edge["target"] in wait_ids
        }
        incoming = {
            (item["resource"], item["target"])
            for item in self.execution["incoming"]
            if item["kind"] == "wait_completion" and item["target"] in wait_ids
        }
        return {
            "same": required == actual and initial == incoming,
            "streams": len(bindings),
            "native_waits": native_waits,
            "kinds": dict(kinds),
            "completion_links": len(required),
            "missing": list(sorted(required - actual))[:3],
            "extra": list(sorted(actual - required))[:3],
            "initial_frontiers": len(initial),
            "same_initial_frontiers": initial == incoming,
        }

    def events(self) -> dict[str, Any]:
        expected = Counter()
        events = []
        for position, operation in enumerate(self.native):
            for action in operation["execution_observation"]["runtime_actions"]:
                expected[position, action["kind"], action["native_api"]] += 1
                if action["kind"] in ("record_event", "wait_event", "wait_event_complete"):
                    events.append((position, action))

        nodes = self.nodes
        actual = Counter(
            (self.positions[node["operation"]], node["kind"], node["name"])
            for node in nodes
            if node["kind"] in self.api_kinds
        )
        report: dict[str, Any] = {
            "same": expected == actual,
            "status": "matched" if expected == actual else "differences",
            "native_api_actions": expected.total(),
            "target_api_actions": actual.total(),
            "native_event_actions": len(events),
        }
        if expected != actual:
            report["first_missing_api"] = next(iter((expected - actual).elements()), None)
            report["first_extra_api"] = next(iter((actual - expected).elements()), None)
            return report

        if any(
            action["submission"] is None
            or "event" not in (action["native_call"] or {})
            or action["target"] is None
            or action["target"]["kind"] != ("event" if action["kind"] == "wait_event_complete" else "stream")
            for _, action in events
        ):
            return {
                **report,
                "same": None,
                "status": "partial",
                "reason": "event submission, identity or target missing",
            }
        if len({self.issue(action)[0] for _, action in events}) > 1:
            return {
                **report,
                "same": None,
                "status": "partial",
                "reason": "event APIs have multiple issuers; no cross-thread ordering assumed",
            }

        event_names = {action["native_api"] for _, action in events}
        ordinals: Counter[tuple[int, str]] = Counter()
        barriers = {}
        for node in nodes:
            if node["kind"] in ("barrier", "wait_event_complete") and node["name"] in event_names:
                signature = self.positions[node["operation"]], node["name"]
                barriers[(*signature, ordinals[signature])] = node
                ordinals[signature] += 1
        if len(barriers) != len(events):
            return {
                **report,
                "same": False,
                "status": "differences",
                "reason": "event barrier count differs",
                "target_event_barriers": len(barriers),
            }

        records = {}
        required = set()
        stream_bindings: dict[int, set[int]] = defaultdict(set)
        target_streams: dict[int, set[int]] = defaultdict(set)
        ordinals.clear()
        external_waits = 0

        # Match occurrences by logical owner and API order, never by proximity of timestamps.
        for position, action in sorted(events, key=lambda item: self.issue(item[1])):
            signature = position, action["native_api"]
            key = (*signature, ordinals[signature])
            ordinals[signature] += 1
            if key not in barriers:
                return {
                    **report,
                    "same": False,
                    "status": "differences",
                    "reason": "event stream barrier missing",
                    "event": key,
                }
            barrier = barriers[key]
            if action["kind"] != "wait_event_complete":
                stream = action["target"]["identity"]
                stream_bindings[stream].add(barrier["resource"])
                target_streams[barrier["resource"]].add(stream)
            event = action["native_call"]["event"]
            if action["kind"] == "record_event":
                records[event] = barrier["id"]
            elif event in records:
                required.add((records[event], barrier["id"]))
            else:
                external_waits += 1

        barrier_ids = {node["id"] for node in barriers.values()}
        connections = {
            (edge["source"], edge["target"])
            for edge in self.execution["dependencies"]
            if edge["kind"] == "event_wait" and edge["source"] in barrier_ids and edge["target"] in barrier_ids
        }
        same_streams = all(len(bindings) == 1 for bindings in (*stream_bindings.values(), *target_streams.values()))
        report.update(
            same=required == connections and same_streams,
            event_completion_links=len(required),
            missing_event_links=len(required - connections),
            extra_event_links=len(connections - required),
            same_event_streams=same_streams,
            external_event_waits=external_waits,
        )
        report["status"] = "matched" if report["same"] else "differences"
        if external_waits and report["same"]:
            report.update(same=None, status="partial", reason="event wait needs a record outside the selected window")
        return report
