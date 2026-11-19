"""Project independently observed host conditions onto serial steps of a fixed graph."""

from bisect import bisect_left, bisect_right
from collections import defaultdict
from dataclasses import dataclass
from itertools import pairwise
import json

from markov_collection.adapters.runtimes.ascend.host_alignment import ThreadService

from .host_queues import HostQueueEvidence


@dataclass(frozen=True, slots=True)
class QueueResumption:
    producer: int
    service_prefix_ns: int
    fixed_prefix_ns: int
    retirement_ns: int
    dispatch_ns: int


class FixedHostContext:
    def __init__(self, capture, rank, evidence):
        activity = evidence.native["host_observation"].get("activity")
        rows = []
        if activity is not None:
            if activity != f"rank-{rank}-thread-activity.jsonl":
                raise ValueError("fixed host activity file belongs to a different worker")
            with (capture / activity).open() as source:
                rows = [json.loads(line) for line in source]

        # Waits triggered by work on other resources still follow their original dependencies.
        observer, dequeues, waits = evidence.replay_host_intervals()
        self.active = ThreadService(rows, dequeues, [*observer, *waits])
        self.observer = ThreadService.by_thread(observer)
        covered = [row for row in rows if row["thread"] in self.active.activity]
        self.runnable = {row["thread"]: self.runnable_pauses(row) for row in covered}
        self.scheduler_evidence = {
            "threads": sum("kernel_events" in row for row in covered),
            "additional_runnable_pauses": sum(len(self.runnable[row["thread"]]) - len(row["preemptions"]) for row in covered),
        }
        self.queues = HostQueueEvidence(evidence, covered, self.runnable)

    @staticmethod
    def runnable_pauses(row):
        pauses = {tuple(pair) for pair in row["preemptions"]}
        states = [(stamp, payload[-1]) for kind, stamp, *payload in row.get("kernel_events", ()) if kind == "switch_out"]
        stamps = [stamp for stamp, _ in states]
        on_cpu = row["window_ns"][0]
        for (out, running), (resume, _) in pairwise(row["switches"]):
            if running:
                on_cpu = out
                continue

            # The matching scheduler sample belongs to this run slice on the same per-thread perf buffer.
            first = bisect_left(stamps, on_cpu)
            last = bisect_right(stamps, out, lo=first)
            if last == first + 1 and states[first][1] == 0:
                pauses.add((out, resume))
        return sorted(pauses)

    def fence_service(self, call, sdk_entry):
        begin, end = call["begin_raw_ns"], call["end_raw_ns"]
        if not begin <= sdk_entry <= end:
            return None
        running = self.active.running_intervals(call["thread"], begin, end)
        if running is not None and sum(last - first for first, last in running) == end - begin:

            # The Ascend SDK starts its API record before entering the runtime wait.
            return sdk_entry - begin, end - sdk_entry
        return None

    def interruptions(self, tid, begin, end):
        activity = [(max(begin, first), min(end, last))
                    for first, last in self.active.within(self.observer.get(tid, ()), begin, end)]
        overhead = sum(last - first for first, last in activity)

        # Explicit observation intervals include their own interruptions. Business pauses are counted once.
        pauses = [(first, last) for first, last in self.active.within(self.runnable.get(tid, ()), begin, end)
                  if begin <= first <= last <= end]
        paused = sum(last - first for first, last in pauses) - sum(
            last - first for first, last in self.active.intersect(pauses, activity))
        return overhead, paused

    def project(self, graph, details):
        work = {(row["start"], row["finish"]): row["kind"] for row in graph["work"]}
        threads = {lane: int(resource["identity"].split("/")[1]) for lane, resource in enumerate(graph["resources"])
                   if resource["kind"] == "thread" and resource["serial"]}
        lanes, positions = defaultdict(list), {}
        for event, (node, point) in enumerate(zip(graph["events"], graph["evidence"]["events"], strict=True)):
            lane = node["resource"]
            if lane in threads:
                lanes[lane].append((point["order"], point["time_ns"], event))
                positions[threads[lane], point["order"]] = event

        report, arrivals = self.queues.observe()
        report["scheduler_trace"] = self.scheduler_evidence
        notifications = defaultdict(list)
        for row in report["notifications"]:
            notifications[row["consumer"]].append(row)
        for queue in notifications.values():
            queue.sort(key=lambda row: row["off_cpu_ns"][0])

        delays, resumptions = {}, {}
        bound = 0
        for lane, events in lanes.items():
            tid = threads[lane]
            submission = graph["resources"][lane]["role"] == "submission"
            queue = notifications.get(tid, ())
            for (_, begin, source), (_, end, target) in pairwise(sorted(events)):
                kind = work.get((source, target))
                if begin is None or end is None or begin >= end or kind not in {None, "host"}:
                    continue

                overhead, paused = self.interruptions(tid, begin, end)
                outside = self.active.duration(tid, begin, end, submission) if kind is None else None
                conditions = {"observer_ns": overhead, "runnable_pause_ns": paused, "outside_work_ns": outside}
                if any(conditions.values()):
                    delays[source, target] = {name: value for name, value in conditions.items() if value}
                if kind is None:
                    continue

                first = bisect_left(queue, begin, key=lambda row: row["off_cpu_ns"][0])
                last = bisect_left(queue, end, lo=first, key=lambda row: row["off_cpu_ns"][0])
                if first == last:
                    continue
                occupied = self.active.duration(tid, begin, end, submission)
                if occupied is None:
                    continue

                resumes = []
                for notification in queue[first:last]:
                    asleep, resumed = notification["off_cpu_ns"]
                    producer = positions.get((notification["producer"], notification["dequeue_order"]))
                    if producer is None or resumed > end:
                        continue

                    # Once asleep, the thread can only continue after the actual queue producer notifies it.
                    observer, preemption = self.interruptions(tid, begin, asleep)
                    cpu = self.active.duration(tid, begin, asleep, submission) if asleep > begin else 0
                    notice = notification["notification_ns"][0]
                    resumes.append(QueueResumption(producer, cpu, observer + preemption,
                                                   notice - notification["dequeue_finished_ns"], resumed - notice))
                if resumes:
                    resumptions[source, target] = occupied, tuple(resumes)
                    bound += len(resumes)

        handoffs, counts = self.queues.handoffs(graph, positions, arrivals, self.active)
        for pair, conditions in handoffs.items():
            delays.setdefault(pair, {}).update(conditions)
        report["bound_to_fixed_work"] = bound
        report.update(counts)
        if not details:
            del report["notifications"]
        return delays, resumptions, report
