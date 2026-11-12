"""Check supplied services and completion windows against actual execution events."""

from collections import Counter, defaultdict


class ExecutionTimes:
    def __init__(self):
        self.starts = defaultdict(list)
        self.finishes = defaultdict(list)
        self.expected = Counter()
        self.bounds = {}
        self.counts = Counter()
        self.now_ns = 0

    def observe(self, record):
        kind = record["kind"]
        if kind in {"activation", "completion"}:
            if record["time_ns"] < self.now_ns:
                raise ValueError("execution clock moved backwards")
            self.now_ns = record["time_ns"]
        if kind == "graph":
            for work, service in zip(record["graph"]["work"], record["work_ns"], strict=True):
                if service is not None and (type(service) is not int or service < 0):
                    raise ValueError("work needs a nonnegative service duration or an explicit unknown value")
                start, finish = (record["event_ids"][work[name]] for name in ("start", "finish"))
                self.starts[start].append((finish, service))
                self.expected[finish] += 1
                self.counts["conditioned_work" if service is None else "given_services"] += 1
        elif kind == "boundary_time":
            event = record["event"]
            begin, end = record["begin_ns"], record["end_ns"]
            if event in self.bounds or not 0 <= begin <= end:
                raise ValueError("completion needs one ordered nonnegative time window")
            self.bounds[event] = begin, end
        elif kind == "completion":
            event, time_ns = record["event"], record["time_ns"]
            bound = self.bounds.pop(event, None)
            if bound is not None:
                if not bound[0] <= time_ns <= bound[1]:
                    raise ValueError("completed event violates its supplied time window")
                self.counts["checked_windows"] += 1
            for finish, service in self.starts.pop(event, ()):
                self.finishes[finish].append((time_ns, service))
            if event in self.expected:
                work = self.finishes.pop(event, ())
                if len(work) != self.expected.pop(event):
                    raise ValueError("work completed before all of its declared starts")

                # A completion condition cannot waive a supplied service lower bound.
                for start, service in work:
                    if service is None:
                        if bound is None:
                            raise ValueError("unknown service completed without its own explicit condition")
                    elif time_ns - start < service:
                        raise ValueError("completion condition waived a supplied service duration")
                    self.counts["checked_work"] += 1

    def finish(self, *, complete):
        if complete and (self.starts or self.expected or self.bounds):
            raise ValueError("execution has unfinished work or completion conditions")
        return {
            **self.counts, "unfinished_work": sum(self.expected.values()), "unfinished_windows": len(self.bounds),
            "scope": "Run-local service lower bounds and explicit completion windows. Unknown service remains unmeasured; no cost model or cross-run timing accuracy qualification.",
        }
