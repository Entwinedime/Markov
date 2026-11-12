"""Compare actual ingress with native receive evidence, independently of supplied timing files."""

import json
from collections import defaultdict

from actions import ActionIdentity


class IngressEvidence:
    def __init__(self, capture, instances):
        origins, self.sources, self.expected = set(), {}, {}
        self.actual = defaultdict(list)
        self.input_attempts = {}
        self.native_reads = {}
        self.actual_reads = defaultdict(list)
        self.received = defaultdict(set)
        self.findings = []
        for index, instance in enumerate(instances):
            directory = capture / "instances" / str(index)
            session = json.loads((directory / "session.json").read_text())
            delivery, = json.loads((directory / "request-delivery.json").read_text())["capture"]
            if delivery["clock"] != "monotonic_raw_ns":
                raise ValueError("ingress comparison requires the shared RAW clock")
            requests = {request["id"]: request for request in session["content"]["requests"]}
            aliases = dict(zip(session["source_requests"], requests, strict=True))
            self.sources[instance["id"]] = aliases, requests, delivery["origin_ns"]
            origins.add(delivery["origin_ns"])
        self.origin_ns, = origins

    @staticmethod
    def rounds(records, aliases, requests, origin_ns):
        result, received = [], set()
        for record in records:
            time_ns = record["received_raw_ns"] - origin_ns
            members = [aliases[name] for name in record["requests"]]
            if not members or time_ns < 0 or (result and time_ns < result[-1]["time_ns"]):
                raise ValueError("receive rounds require nonempty members and nondecreasing times")
            for member in members:
                if member in received or time_ns < requests[member]["arrival_ns"]:
                    raise ValueError(f"receive repeats {member} or precedes its declared arrival")
                received.add(member)
            result.append({"time_ns": time_ns, "requests": members})
        if received != requests.keys():
            raise ValueError("receive rounds do not cover every request exactly once")
        return result

    def native(self, instance, raw):
        if raw["request_ingress"]:
            if instance in self.expected:
                raise ValueError("instance has more than one observed external ingress")
            self.expected[instance] = self.rounds(raw["request_ingress"], *self.sources[instance])
            self.input_attempts[instance] = self.attempts(raw)
            aliases, _, origin_ns = self.sources[instance]
            self.native_reads[instance] = self.reads(instance, raw, aliases, origin_ns)

    @staticmethod
    def attempts(raw):
        records = raw.get("request_input_attempts")
        if records is None:
            return {"status": "unobserved"}
        received, previous_end = [], None
        empty, unknown_context, control_only = 0, 0, 0
        for record in records:
            begin, end, messages = record["begin_raw_ns"], record["received_raw_ns"], record["message_count"]
            if (any(type(value) is not int or value < 0 for value in (begin, end, messages))
                    or begin > end or (previous_end is not None and begin < previous_end)):
                raise ValueError("input attempts require ordered RAW intervals and nonnegative message counts")
            previous_end = end
            context = record["context"]
            if context is not None and not isinstance(context, dict):
                raise ValueError("input attempt context must be a native state snapshot or explicitly unobserved")
            unknown_context += context is None
            empty += messages == 0
            control_only += messages > 0 and not record["requests"]
            if record["requests"]:
                if messages == 0:
                    raise ValueError("empty input attempt cannot receive workload requests")
                received.append({"requests": record["requests"], "received_raw_ns": end})
        if received != raw["request_ingress"]:
            raise ValueError("input attempts disagree with the observed external receive cohorts")

        # These are read intervals, not socket availability, CPU cost, or a schedule to replay.
        return {"status": "partial" if unknown_context else "observed", "attempts": len(records),
                "empty": empty, "control_only": control_only, "unknown_context": unknown_context}

    @staticmethod
    def reads(instance, raw, aliases, origin_ns):
        result, received = [], set()
        for record in raw.get("request_input_attempts", ()):
            members = [aliases[request] for request in record["requests"]]
            time_ns = record["received_raw_ns"] - origin_ns
            if time_ns < 0:
                raise ValueError("input read return precedes the workload clock")

            # Prior deliveries distinguish otherwise equal empty queues; neither the result nor poll ordinal is a key.
            identity = None if record["context"] is None else {
                "instance": instance, "context": record["context"], "received": sorted(received),
            }
            result.append({"identity": identity, "time_ns": time_ns, "requests": members})
            received.update(members)
        return result

    @staticmethod
    def groups(reads):
        groups = defaultdict(list)
        for row in reads:
            if row["identity"] is not None:
                groups[json.dumps(row["identity"], sort_keys=True)].append(row)
        return groups

    def observe(self, row):
        if row["kind"] == "resources":
            self.requests = [request["id"] for request in row["requests"]]
        elif row["kind"] == "input_read":
            identity = row["identity"]
            instance = identity["instance"]
            if instance not in self.sources:
                raise ValueError("ingress names an unknown serving instance")
            indices = row["request_indices"]
            if any(type(index) is not int or not 0 <= index < len(self.requests) for index in indices):
                raise ValueError("ingress has an invalid request index")
            members = [self.requests[index] for index in indices]
            previous = self.actual_reads[instance]
            in_flow = row["in_flow"]
            if (type(in_flow) is not bool or (members and not in_flow)
                    or not isinstance(identity["context"], dict) or identity["received"] != sorted(self.received[instance])
                    or len(set(members)) != len(members) or self.received[instance].intersection(members)
                    or type(row["time_ns"]) is not int or row["time_ns"] < 0
                    or (previous and row["time_ns"] < previous[-1]["time_ns"])):
                raise ValueError("input read disagrees with its entering delivery history or clock")
            observed = {"requests": members, "time_ns": row["time_ns"]}
            previous.append({"identity": identity, "in_flow": in_flow, **observed})
            self.received[instance].update(members)
            if members:
                self.actual[instance].append(observed)

    def compare_reads(self, instance, pairs):
        reads = [row for row in self.actual_reads[instance] if row["in_flow"]]
        outside = len(self.actual_reads[instance]) - len(reads)
        if self.input_attempts[instance]["status"] == "unobserved":
            self.findings.append(("input_read_evidence", {"instance": instance}, {"reason": "native input attempts were not recorded"}))
            return {"status": "unobserved", "predicted_reads": len(reads), "outside_flow": outside}
        native = self.groups(self.native_reads[instance])

        # Capture retains the first source pull and active-flow reads; keep later driver polls in the trace only.
        actual = self.groups(reads)
        if pairs is not None:

            # Only a separately verified complete consumption prefix can identify otherwise repeatable reads.
            native = {str(index): [row] for index, row in enumerate(self.native_reads[instance])}
            actual = {str(source): [self.actual_reads[instance][index]] for index, source in pairs.items()}
        matched, ambiguous, unmatched_native, unmatched_predicted, differences = 0, 0, 0, 0, []
        for key in sorted(native.keys() | actual.keys()):
            left, right = native.get(key, []), actual.get(key, [])
            reason = None
            if not left:
                unmatched_predicted += len(right)
                reason = "no native read with this entering context and delivery history"
            elif not right:
                unmatched_native += len(left)
                reason = "no predicted read with this entering context and delivery history"
            elif len(left) != 1 or len(right) != 1:
                ambiguous += 1
                reason = "logical read identity repeats"
            elif pairs is None and not left[0]["requests"]:
                ambiguous += 1
                reason = "read returns no workload requests and may repeat"
            elif pairs is None and not ActionIdentity.request_progress(left[0]["identity"]["context"]):
                ambiguous += 1
                reason = "entering context has no request work anchor for an exact read time"
            elif (left[0]["identity"] == right[0]["identity"] and left[0]["time_ns"] == right[0]["time_ns"]
                  and left[0]["requests"] == right[0]["requests"]):
                matched += 1
            else:
                differences.append({"identity": left[0]["identity"], "native": left[0], "predicted": right[0]})

            # Explain incomplete correspondence without treating an unmatched read as a proven control error.
            if reason is not None:
                self.findings.append(("input_read_evidence", json.loads(key), {
                    "reason": reason, "native_calls": len(left), "predicted_calls": len(right),
                }))
        unknown = sum(row["identity"] is None for row in self.native_reads[instance])
        if unknown:
            self.findings.append(("input_read_evidence", {"instance": instance}, {
                "reason": "native entering context was not observed", "native_calls": unknown,
            }))
        incomplete = ambiguous or unmatched_native or unmatched_predicted or unknown
        return {"status": "different" if differences else "partial" if incomplete else "matched", "matched": matched,
                "outside_flow": outside,
                "ambiguous": ambiguous, "unmatched_native": unmatched_native, "unmatched_predicted": unmatched_predicted,
                "unknown_native_context": unknown, "differences": differences}

    def compare(self, read_pairs=None):
        results = {}
        for instance in sorted(self.sources.keys() | self.actual.keys()):
            native, actual = self.expected.get(instance), self.actual[instance]
            if native is None:
                results[instance] = {"status": "missing_native_evidence"}
                continue

            # Time is paired only after request identities and receive cohorts agree, never by round number alone.
            cohorts = [row["requests"] for row in native] == [row["requests"] for row in actual]
            differences = []
            if cohorts:
                for left, right in zip(native, actual, strict=True):
                    if left["time_ns"] != right["time_ns"]:
                        differences.append({"requests": left["requests"], "native_ns": left["time_ns"],
                                            "predicted_ns": right["time_ns"], "delta_ns": right["time_ns"] - left["time_ns"]})
            results[instance] = {"status": "matched" if cohorts and not differences else "different", "cohorts_match": cohorts,
                                 "native_rounds": len(native), "predicted_rounds": len(actual), "time_differences": differences,
                                 "native_input_attempts": self.input_attempts[instance],
                                 "input_reads": self.compare_reads(instance, None if read_pairs is None else read_pairs[instance])}
            if not cohorts:
                results[instance].update(native_cohorts=[row["requests"] for row in native],
                                         predicted_cohorts=[row["requests"] for row in actual])
        return results
