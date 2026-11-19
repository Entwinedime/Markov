"""Measure notification service relative to its actual PD transport predecessors."""

from collections import defaultdict


class PdNotificationSamples:
    def __init__(self, observations):
        addresses = {tuple(row["endpoint"]): rank for rank, row in observations.items()}
        sessions = {row["session"]: rank for rank, row in observations.items()}
        if len(addresses) != len(observations) or len(sessions) != len(observations):
            raise ValueError("PD notification measurements repeat a native endpoint")

        self.states, self.chunks = {}, defaultdict(dict)
        self.reservations, self.receipts = defaultdict(dict), defaultdict(dict)
        self.senders = defaultdict(set)
        self.destinations, self.published_to = defaultdict(set), defaultdict(set)
        for rank, observation in observations.items():
            self.record_states(rank, observation["states"])
            for row in observation["reservations"]:
                recipient = addresses.get(tuple(row["target"]))
                key = rank, row["request"]
                self.published_to[key].add(recipient)
                observed = self.reservations[recipient, row["request"]]
                if rank in observed:
                    raise ValueError("PD notification source repeats a reservation publication")
                observed[rank] = row["begin_raw_ns"]
            for row in observation["receipts"]:
                publisher = sessions.get(row["session"])
                if publisher is not None:
                    recorded = self.receipts[rank, row["request"]]
                    if publisher in recorded:
                        raise ValueError("PD notification source repeats a reservation receipt")
                    recorded[publisher] = row["received_raw_ns"]
            for row in observation["transfers"]:
                key = rank, row["request"]
                ends = self.chunks[key]
                ends[row["chunk"]] = max(ends.get(row["chunk"], 0), row["end_raw_ns"])
                if row["kind"] == "metadata":
                    recipient = sessions.get(row["destination"])
                    self.destinations[key].add(recipient)
                    self.senders[recipient, row["request"]].add(rank)

    def record_states(self, rank, rows):
        for row in rows:
            state = row["after"]
            if state not in (2, 4) or state == row["before"]:
                continue
            key = rank, row["request"], state
            begin, end = row["begin_raw_ns"], row["end_raw_ns"]
            lower, upper = self.states.get(key, (begin, end))
            self.states[key] = max(lower, begin), min(upper, end)
            if self.states[key][1] < self.states[key][0]:
                raise ValueError("PD state transition observations do not overlap")

    def prerequisites(self, key, state):
        _, request = key
        if key in self.chunks:
            reservations = self.reservations[key]
            destinations = self.destinations[key]
            if not destinations or None in destinations or destinations != reservations.keys():
                return ()
            if state == 2:
                receipts = self.receipts[key]
                if receipts.keys() != reservations.keys():
                    return ()
                return [(time, time) for time in receipts.values()]
            return [(time, time) for time in self.chunks[key].values()]

        if state == 4:
            senders = self.senders[key]
            if senders and senders == self.published_to[key]:
                return [self.states.get((sender, request, 4)) for sender in senders]
        return ()

    @staticmethod
    def sample(operation, request, completion, prerequisites, **identity):
        bounds = None
        if prerequisites and None not in prerequisites:
            # Both operands are observed on the same RAW clock. Subtract only the service after the latest cause.
            low = max(0, completion[0] - max(upper for _, upper in prerequisites))
            high = completion[1] - max(lower for lower, _ in prerequisites)
            if high < low:
                raise ValueError(f"PD {operation} for request {request} precedes its observed cause")
            bounds = low, high
        return {"operation": operation, "request": request, "dependencies": len(prerequisites),
                "duration_bounds_ns": bounds, **identity}

    def intervals(self, rank):
        for (recipient, request), receipts in self.receipts.items():
            if recipient != rank:
                continue
            for publisher, received in receipts.items():
                sent = self.reservations[recipient, request].get(publisher)
                prerequisites = () if sent is None else ((sent, sent),)
                yield self.sample("kv_reservation_received", request, (received, received), prerequisites, publisher=publisher)

        for (actor, request, state), completion in self.states.items():
            if actor != rank or (state == 2 and (rank, request) not in self.chunks):
                continue
            operation = "kv_ready" if state == 2 else "kv_success"
            yield self.sample(operation, request, completion, self.prerequisites((rank, request), state))
