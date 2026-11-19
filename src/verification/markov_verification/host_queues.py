"""Find actual software-queue notifications independently of the projected graph."""

from bisect import bisect_left, bisect_right
from collections import Counter, defaultdict
from itertools import pairwise

from markov_collection.adapters.runtimes.ascend.host_alignment import ThreadService
from markov_collection.adapters.runtimes.ascend.trace import Interval, ThreadIntervals


class HostQueueEvidence:
    def __init__(self, source, activity, runnable):
        self.source = source
        recorded = {"eventfd_read": [], "eventfd_write": []}
        for row in source.runtime:
            if row["api"] in recorded:
                recorded[row["api"]].append(Interval(row["begin_raw_ns"], row["end_raw_ns"], row["fd"], row))
        self.notifications = sum(map(len, recorded.values()))
        self.reads = recorded["eventfd_read"]
        self.read_index, self.write_index = (ThreadIntervals(records) for records in recorded.values())
        self.takes, self.completions = defaultdict(list), defaultdict(list)
        for correlation, command in source.dequeue_ids.items():
            self.takes[command.thread].append(command)
            if source.window.begin <= command.begin <= command.end <= source.window.end:
                self.completions[command.thread].append((self.stamp(command, "end"), correlation))
        for commands in self.takes.values():
            commands.sort(key=lambda command: command.begin)
        for commands in self.completions.values():
            commands.sort()

        self.sleep, self.native_threads, self.wakes = {}, {}, defaultdict(list)
        for row in activity:
            thread = row["thread"]
            pauses = set(runnable[thread])
            self.sleep[thread] = [
                (out, resume) for (out, running), (resume, resumed) in pairwise(row["switches"])
                if not running and resumed and (out, resume) not in pauses
            ]
            for kind, stamp, *payload in row.get("kernel_events", ()):
                if kind == "switch_out":
                    self.native_threads[thread] = payload[0]
                elif kind == "wake":
                    self.wakes[thread, payload[0]].append(stamp)
        for stamps in self.wakes.values():
            stamps.sort()

    def producer(self, read, sleep, resume):
        if any(other is not read and other.begin < read.end and read.begin < other.end
               for other in self.read_index.overlapping(read)):
            return None

        # A single credit on this descriptor must wake the blocked consumer.
        writes = [row for row in self.write_index.overlapping(read) if row.begin < read.end and read.begin < row.end]
        if len(writes) != 1:
            return None
        write, = writes
        if not (read.record["value"] == write.record["value"] == 1 and write.begin <= resume and sleep <= write.end):
            return None

        # A write can enter before the reader switches out. Its kernel wake identifies when the credit took effect.
        peer = self.native_threads.get(read.record["thread"])
        stamps = self.wakes.get((write.record["thread"], peer), ())
        first = bisect_left(stamps, max(sleep, write.begin))
        last = bisect_right(stamps, min(resume, write.end), lo=first)
        if last == first + 1:
            return write, stamps[first]
        if sleep <= write.begin:
            return write, write.begin
        return None

    def stamp(self, command, side):
        return self.source.host_measurements[f"thread/{command.thread}", getattr(command, side)]

    def notified_enqueue(self, read, write):
        takes = self.takes[read.thread]
        index = bisect_left(takes, read.end, key=lambda command: command.begin)
        if index == len(takes) or (index and takes[index - 1].end > read.begin):
            return None
        take = takes[index]
        producer = self.source.enqueue_ranges.enclosing(self.source.interval(write.record))
        if producer is None or producer.thread == read.thread or take.end > self.source.window.end:
            return None

        # The writer is inside the exact enqueue whose command the worker takes after the read.
        correlation = take.record["args"]["correlation_id"]
        owner = self.source.owner(producer)
        if producer is not self.source.enqueues[correlation] or owner is None or owner in self.source.closed_scopes:
            return None
        return correlation

    def observe(self):
        counts, notifications, arrivals = Counter(), [], set()
        for read in self.reads:
            interval = self.source.interval(read.record)
            if not (self.source.window.begin <= interval.begin < interval.end <= self.source.window.end):
                continue
            if self.source.excluded.enclosing(interval) is not None:
                continue
            if interval.thread in self.takes:
                sleeps = self.blocked(read, interval.thread)
                notice = self.producer(read, *sleeps[0]) if len(sleeps) == 1 else None
                correlation = self.notified_enqueue(interval, notice[0]) if notice is not None else None
                if correlation is not None:
                    arrivals.add(correlation)
                continue
            scope = self.source.owner(interval)
            if scope is None or scope in self.source.closed_scopes:
                continue
            counts["reads"] += 1
            if interval.thread not in self.sleep:
                counts["unknown_thread_activity"] += 1
                continue
            sleeps = self.blocked(read, interval.thread)
            if not sleeps:
                continue
            counts["blocked_reads"] += 1
            notice = self.producer(read, *sleeps[0]) if len(sleeps) == 1 else None
            if notice is None:
                counts["unpaired_notifications"] += 1
                continue
            write, released = notice

            # The SDK closes its dequeue mark before the repository retires the command and notifies the waiter.
            producer = write.record["thread"]
            commands = self.completions.get(producer, ())
            index = bisect_right(commands, write.begin, key=lambda command: command[0]) - 1
            if index < 0 or producer == interval.thread:
                counts["unpaired_queue_completions"] += 1
                continue
            finish, correlation = commands[index]
            enqueue = self.source.enqueues[correlation]
            if self.source.excluded.enclosing(enqueue) is not None:
                counts["observer_producers"] += 1
                continue
            counts["dequeue_mark_before_wait"] += finish < read.begin
            notifications.append({
                "scope": scope, "site": self.source.native["scopes"][scope]["site"],
                "descriptor": read.thread, "consumer": interval.thread, "producer": producer,
                "sdk_correlation": correlation, "dequeue_order": self.source.dequeue_ids[correlation].end,
                "dequeue_finished_ns": finish,
                "wait_ns": [read.begin, read.end], "off_cpu_ns": list(sleeps[0]),
                "notification_ns": [released, min(write.end, sleeps[0][1])],
            })

        notifications.sort(key=lambda row: row["wait_ns"])
        return {
            "notification_records": self.notifications, **counts, "queue_completion_notifications": len(notifications),
            "queue_arrival_notifications": len(arrivals),
            "first": notifications[0] if notifications else None,
            "notifications": notifications,
        }, arrivals

    def blocked(self, read, thread):
        return [(first, last) for first, last in ThreadService.within(self.sleep.get(thread, ()), read.begin, read.end)
                if read.begin <= first < last <= read.end]

    def handoffs(self, graph, positions, arrivals, activity):
        delays, counts = {}, Counter()
        edges = {(edge["source"], edge["target"], edge["kind"]) for edge in graph["edges"]
                 if edge["kind"] in {"submission", "order"}}
        for takes in self.takes.values():
            for previous, take in pairwise([None, *takes]):
                target = positions.get((take.thread, take.begin))
                if target is None:
                    continue
                correlation = take.record["args"]["correlation_id"]
                post = self.source.enqueues[correlation]
                sender = positions.get((post.thread, post.begin))
                if post.thread == take.thread or (sender, target, "submission") not in edges:
                    continue

                finish = self.stamp(take, "begin")
                if correlation in arrivals or previous is None or previous.end <= post.begin:
                    elapsed = finish - self.stamp(post, "begin")
                    self.source.require(elapsed >= 0, "observed queue take precedes its original enqueue")
                    delays[sender, target] = {"queue_handoff_ns": elapsed}
                    counts["notified_active_takes"] += correlation in arrivals
                    continue

                # An already posted command follows the previous worker's measured retirement activity.
                preceding = positions.get((take.thread, previous.end))
                if post.end > previous.end or (preceding, target, "order") not in edges:
                    continue
                begin = self.stamp(previous, "end")
                running = activity.duration(take.thread, begin, finish, False)
                if running is not None and running == finish - begin:
                    delays[preceding, target] = {"queue_retirement_ns": running}
                    counts["running_retirements"] += 1
        return delays, dict(counts)
