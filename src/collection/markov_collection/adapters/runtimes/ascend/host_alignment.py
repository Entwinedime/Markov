"""Resolve source CPU cuts against a lightweight native timing run."""

from bisect import bisect_right
from collections import Counter, defaultdict
from itertools import pairwise

from markov_collection.adapters.runtimes.ascend.trace import Interval, ThreadIntervals


def merged(ranges):
    result = []
    for begin, end in sorted(ranges):
        if begin >= end:
            continue
        if result and begin <= result[-1][1]:
            result[-1] = result[-1][0], max(end, result[-1][1])
        else:
            result.append((begin, end))
    return result


class ThreadService:
    def __init__(self, rows, dequeues, excluded):
        self.activity = {}
        self.excluded = self.by_thread(excluded)
        self.dequeues = self.by_thread(dequeues)
        self.loss = []
        for row in rows:
            thread = row["thread"]
            switches = row["switches"]
            if row["lost"] or any(left[0] > right[0] or left[1] == right[1] for left, right in pairwise(switches)):
                self.loss.append(thread)
                continue
            if not switches:
                continue
            begin, end = row["window_ns"]
            running = not switches[0][1]
            cursor = begin
            intervals = []
            for stamp, next_state in switches:
                if stamp > end:
                    break
                if stamp >= begin:
                    if running:
                        intervals.append((cursor, stamp))
                    cursor = stamp
                running = next_state
            if running:
                intervals.append((cursor, end))
            self.activity[thread] = (begin, end, intervals)

    @staticmethod
    def by_thread(intervals):
        ranges = defaultdict(list)
        for thread, begin, end in intervals:
            ranges[thread].append((begin, end))
        return {thread: merged(values) for thread, values in ranges.items()}

    @staticmethod
    def intersect(first, second):
        left, right = iter(first), iter(second)
        a, b = next(left, None), next(right, None)
        while a is not None and b is not None:
            begin, end = max(a[0], b[0]), min(a[1], b[1])
            if begin < end:
                yield begin, end
            if a[1] <= b[1]:
                a = next(left, None)
            else:
                b = next(right, None)

    @staticmethod
    def within(intervals, begin, end):
        first = bisect_right(intervals, begin, key=lambda interval: interval[1])
        last = bisect_right(intervals, end - 1, lo=first, key=lambda interval: interval[0])
        return intervals[first:last]

    def running_intervals(self, thread, begin, end):
        if thread not in self.activity or end <= begin:
            return None
        opened, closed, active = self.activity[thread]
        if not opened <= begin < end <= closed:
            return None
        return self.intersect(self.within(active, begin, end), [(begin, end)])

    def duration(self, thread, begin, end, submission):
        covered = self.running_intervals(thread, begin, end)
        if covered is None:
            return None
        if submission:

            # An idle or spinning submission thread between two queue commands performs no modeled work.
            covered = self.intersect(covered, self.within(self.dequeues.get(thread, ()), begin, end))
        covered = list(covered)
        occupied = sum(y - x for x, y in covered)
        excluded = sum(y - x for x, y in self.intersect(covered, self.within(self.excluded.get(thread, ()), begin, end)))
        return occupied - excluded


class AscendHostAlignment:
    def __init__(self, trace, scopes, pairs, commands, runtime, activity, auxiliary, queries):
        self.trace = trace
        self.misses = Counter()
        self.points = {}
        observed = {scope["id"]: Interval(*scope["raw_interval"], scope["thread"], scope) for scope in scopes[1:]}
        parts = {source: observed[target] for source, target in pairs.items()}

        # Unmatched children still own their records; they cannot transfer queue work to a matched parent.
        owners = ThreadIntervals(list(observed.values()))
        auxiliary = [Interval(begin, end, thread, {}) for thread, begin, end in auxiliary]
        excluded = ThreadIntervals(auxiliary)
        filtered = []
        for command in commands:
            name, enqueue, dequeue = command
            entered = Interval(enqueue[1], enqueue[2], enqueue[0], {})
            if excluded.enclosing(entered) is not None:
                auxiliary.append(Interval(dequeue[1], dequeue[2], dequeue[0], {}))
            else:
                filtered.append(command)
        excluded_time = [(row.thread, row.begin, row.end) for row in auxiliary]
        for row in runtime:
            thread = row["thread"]
            before, after = row["before_raw_ns"], row["after_raw_ns"]
            if row["api"] in trace.host_waits or (row["api"] == "aclrtMemcpy" and row["bytes"]):
                excluded_time.append((thread, before, after))
            else:
                excluded_time.extend(((thread, before, row["begin_raw_ns"]), (thread, row["end_raw_ns"], after)))
        self.service = ThreadService(activity, (dequeue for _, _, dequeue in filtered), excluded_time)
        runtime_intervals = []
        for row in runtime:
            call = Interval(row["begin_raw_ns"], row["end_raw_ns"], row["thread"], row)
            if row["result"] == 0 and excluded.enclosing(call) is None:
                runtime_intervals.append(call)
        self.runtime = ThreadIntervals(runtime_intervals)
        self.apis = trace.host_waits | trace.stream_waits | {
            "aclrtMemcpy", "aclrtMemcpyAsync", "drvMemcpy", "halMemcpy", "halSqTaskSend",
        }
        self.align_scopes(parts, owners, filtered)
        for query in queries:
            scope = query["scope"]
            if scope in parts:
                previous = self.trace.scopes[scope]
                self.anchors(Interval(query["begin_counter"], query["end_counter"], previous.thread, {}), parts[scope])

    def point(self, thread, position, target_thread, target_position):
        key, mapped = (thread, position), (target_thread, target_position)
        if self.points.setdefault(key, mapped) != mapped:
            self.points[key] = None

    def anchors(self, source, target):
        self.point(source.thread, source.begin, target.thread, target.begin)
        self.point(source.thread, source.end, target.thread, target.end)

    @staticmethod
    def signature(call):
        row = call.record
        return row["api"], row.get("tasks", 0), row.get("bytes", 0), row.get("direction", -1)

    def calls(self, values):
        return [row for row in values if row.record["api"] in self.apis]

    def align_runtime(self, old, new):
        original = [self.signature(row) for row in old]
        measured = [self.signature(row) for row in new]
        if original != measured:
            self.misses["runtime_sequence"] += 1
            left, right = Counter(original), Counter(measured)
            shared = {key for key, count in left.items() if count == right[key]}

            # A transient runtime call can disappear after warmup. Pair only cuts with equal multiplicity and preserved order.
            if [key for key in original if key in shared] != [key for key in measured if key in shared]:
                return
            old = [row for row, key in zip(old, original, strict=True) if key in shared]
            new = [row for row, key in zip(new, measured, strict=True) if key in shared]
        for previous, current in zip(old, new, strict=True):
            self.anchors(previous, current)

    def align_scopes(self, parts, owners, commands):
        previous, current = defaultdict(list), defaultdict(list)
        for key, enqueue in sorted(self.trace.enqueue.items(), key=lambda row: row[1].begin):
            scope = self.trace.owner(enqueue)
            if scope not in parts or self.trace.excluded_ranges.enclosing(enqueue) is not None:
                continue
            dequeue = self.trace.dequeue[key]
            name = dequeue.record["name"].removeprefix("Dequeue@")
            previous[scope].append((name, enqueue, dequeue))
        for name, enqueue, dequeue in sorted(commands, key=lambda row: row[1][1]):
            entered = Interval(enqueue[1], enqueue[2], enqueue[0], {})
            owner = owners.enclosing(entered)
            if owner is not None:
                finished = Interval(dequeue[1], dequeue[2], dequeue[0], {})
                current[owner.record["id"]].append((name, entered, finished))

        for scope, measured in parts.items():
            old_scope = self.trace.scopes[scope]
            old, new = previous[scope], current[measured.record["id"]]
            if [row[0] for row in old] != [row[0] for row in new]:
                self.misses["queue_sequence"] += 1
                continue
            self.anchors(old_scope, measured)
            old_runtime = [row for row in self.calls(self.trace.runtime_ranges.inside(old_scope))
                           if self.trace.owner(row) == scope and self.trace.excluded_ranges.enclosing(row) is None]
            new_runtime = [row for row in self.calls(self.runtime.inside(measured)) if owners.enclosing(row) is measured]
            self.align_runtime(old_runtime, new_runtime)
            for (_, entered, finished), (_, produced, consumed) in zip(old, new, strict=True):
                self.anchors(entered, produced)
                self.anchors(finished, consumed)
                self.align_runtime(self.calls(self.trace.runtime_ranges.inside(finished)), self.calls(self.runtime.inside(consumed)))

    def duration(self, thread, begin, end, submission):
        begin = self.points.get((thread, begin))
        end = self.points.get((thread, end))
        if begin is None or end is None or begin[0] != end[0]:
            return None
        return self.service.duration(begin[0], begin[1], end[1], submission)
