"""Check the native MLP coordination result against each run's actual participants."""

from collections import Counter
from math import prod


class BatchSynchronization:
    def __init__(self, finding):
        self.finding = finding
        self.counts = Counter()

    @staticmethod
    def owns(work):
        positions = [*work.get("callers", ()), work.get("continuation") or {}]
        return any(position["owner"] in {"batch_sync", "batch_sync_exchange"}
                   for position in positions if "owner" in position)

    @staticmethod
    def terminal(work):
        position = work.get("continuation") or {}
        return position.get("owner") == "batch_sync_exchange" and position.get("exit") == "call.return"

    @staticmethod
    def span(identities):
        positions = [identity["work"]["continuation"] for identity in identities]
        if (positions[0]["entry"] != "call.enter" or positions[-1]["exit"] != "call.return"
                or any(position["owner"] != "batch_sync_exchange" for position in positions)):
            raise ValueError("batch synchronization span lacks its complete native invocation")
        enclosing = {name: value for name, value in identities[0]["work"].items() if name != "continuation"}
        if any({name: value for name, value in identity["work"].items() if name != "continuation"} != enclosing
               or identity["group"] != identities[0]["group"] for identity in identities):
            raise ValueError("batch synchronization span crosses different native work")
        for previous, following in zip(positions, positions[1:]):
            if (previous["exit"] == "call.return" or previous["exit"] != following["entry"]
                    or previous["exit_context"] != following["entry_context"]):
                raise ValueError("batch synchronization segments do not form one continuation")

    def attach(self, payload, endpoints, identity, origin):
        layout = payload["layout"]
        if len(layout) != 3 or any(type(size) is not int or size < 1 for size in layout):
            raise ValueError("batch synchronization requires positive DP/TP/CP dimensions")
        candidates = [endpoint for endpoint in endpoints
                      if endpoint["operation"] == "all_gather_into_tensor" and endpoint["group_role"] == payload["group"]]
        if prod(layout) == 1 and not candidates:
            self.check_values([payload], identity, origin)
            return
        if len(candidates) != 1 or "batch_sync" in candidates[0] or len(candidates[0]["members"]) != prod(layout):
            raise ValueError("batch synchronization observation has no unique matching communication")
        candidates[0]["batch_sync"] = payload

    def check(self, endpoints, origin):
        relevant = [endpoint["operation"] == "all_gather_into_tensor" and self.owns(endpoint["work"].get("work", {}))
                    for endpoint in endpoints]
        if not any(relevant):
            return
        if not all(relevant):
            self.finding("batch_sync_protocol", endpoints[0]["work"], origin=origin,
                         reason="participants disagree on batch synchronization ownership")
            return
        if any("batch_sync" not in endpoint for endpoint in endpoints):
            self.counts[f"{origin}_unobserved"] += 1
            return
        self.check_values([endpoint["batch_sync"] for endpoint in endpoints], endpoints[0]["work"], origin)

    def check_values(self, records, identity, origin):
        first = records[0]
        if (prod(first["layout"]) != len(records)
                or any(record["layout"] != first["layout"] or record["group"] != first["group"] for record in records)):
            raise ValueError("batch synchronization values disagree on participant layout")
        if any(record["active"] != [1] * len(records) for record in records):

            # CPU activity mirrors do not prove device-side fault-recovery masks; no fallback values are invented.
            self.counts[f"{origin}_inactive_unverified"] += 1
            return
        for record in records:
            fields = record["input"]
            counts = ("num_tokens", "num_tokens_for_logprob", "local_forward_mode")
            flags = ("can_cuda_graph", "is_extend_in_batch", "local_can_run_tbo", "can_run_breakable_cuda_graph")
            if (fields.keys() != set(counts + flags)
                    or any(type(fields[name]) is not int or fields[name] < 0 for name in counts)
                    or any(type(fields[name]) is not bool for name in flags)):
                raise ValueError("batch synchronization input lacks the seven native scalar fields")

        # Native coordination consumes the first attention-TP/CP participant of each DP group.
        _, tp, cp = first["layout"]
        rows = [record["input"] for record in records[::tp * cp]]
        expected = {
            "global_num_tokens": [row["num_tokens"] for row in rows],
            "global_num_tokens_for_logprob": [row["num_tokens_for_logprob"] for row in rows],
            "can_cuda_graph": all(row["can_cuda_graph"] for row in rows),
            "is_extend_in_batch": any(row["is_extend_in_batch"] for row in rows),
            "can_run_breakable_cuda_graph": all(row["can_run_breakable_cuda_graph"] for row in rows),
        }
        self.counts[f"{origin}_checked"] += 1
        for record in records:
            if record["output"] != expected:
                self.finding("batch_sync_values", identity, origin=origin, expected=expected, actual=record["output"])
                return
        self.counts[f"{origin}_matched"] += 1
