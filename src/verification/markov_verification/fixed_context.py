"""Project independently observed host conditions onto serial steps of a fixed graph."""

from bisect import bisect_left, bisect_right
from collections import defaultdict
from dataclasses import dataclass
from itertools import pairwise
import json

from markov_collection.adapters.runtimes.ascend.host_alignment import ThreadService

from .host_queues import HostQueueEvidence
from .ingress import IngressEvidence


@dataclass(frozen=True, slots=True)
class HostStep:
    start: int
    finish: int
    begin_ns: int
    end_ns: int
    modeled: bool
    service_ns: int | None


@dataclass(frozen=True, slots=True)
class HostProgress:
    start: int
    finish: int
    observed_ns: int
    service_prefix_ns: int
    fixed_prefix_ns: int
    service_ns: int


@dataclass(frozen=True, slots=True)
class ExternalWake:
    raw_ns: int
    kernel_thread: int | None = None


@dataclass(frozen=True, slots=True)
class KernelWake:
    raw_ns: int
    local_thread: int | None
    kernel_thread: int | None = None


@dataclass(frozen=True, slots=True)
class HostWait:
    stopped: HostProgress
    source: HostProgress | ExternalWake | int
    delay_ns: int
    take_entry: HostProgress | None = None


@dataclass(frozen=True, slots=True)
class IdleWait:
    source: int
    target: int
    stopped_raw_ns: int
    resumed_raw_ns: int
    head_ns: int
    tail_ns: int


class FixedThreadActivity(ThreadService):

    # Linux futex command and options recorded by the syscall tracepoint.
    futex_flags = 128 | 256
    futex_wait_ops = frozenset({0, 9})
    interrupt_flags = 0x08 | 0x10 | 0x40

    def __init__(self, rows, dequeues, excluded):
        super().__init__(rows, dequeues, excluded)
        covered = [row for row in rows if row["thread"] in self.activity]
        self.runnable, self.blocked_at, self.sleep = {}, {}, {}
        self.kernel_threads = {row["thread"]: row["kernel_thread"] for row in rows if "kernel_thread" in row}
        self.wakes = defaultdict(list)
        self.futex_waits = defaultdict(list)
        for row in covered:
            thread = row["thread"]
            runnable, blocked = self.switch_points(row)
            self.runnable[thread], self.blocked_at[thread] = runnable, blocked
            pauses = set(runnable)
            self.sleep[thread] = [
                (out, resume) for (out, running), (resume, resumed) in pairwise(row["switches"])
                if not running and resumed and (out, resume) not in pauses
            ]
            pids, entered = set(), None
            for kind, stamp, *payload in row.get("kernel_events", ()):
                if kind == "switch_out":
                    pids.add(payload[0])
                elif kind == "wake":
                    self.wakes[payload[0]].append(KernelWake(stamp, thread))
                elif kind == "futex_enter":
                    operation = payload[1] & ~self.futex_flags
                    entered = stamp if operation in self.futex_wait_ops else None
                elif kind == "futex_exit":
                    if entered is not None and payload[0] == 0:
                        self.futex_waits[thread].append((entered, stamp))
                    entered = None
            if thread in self.kernel_threads and pids - {self.kernel_threads[thread]}:
                raise ValueError("worker activity changes its kernel thread identity")
            if len(pids) == 1:
                self.kernel_threads[thread], = pids

        local_threads = {kernel: local for local, kernel in self.kernel_threads.items()}
        if len(local_threads) != len(self.kernel_threads):
            raise ValueError("native worker repeats a kernel thread identity")
        for row in covered:
            if "target_wakes" not in row:
                continue
            recipient = self.kernel_threads[row["thread"]]
            for stamp, producer, flags in row["target_wakes"]:

                # Interrupt tracepoints carry the interrupted task's PID, not a user-space waker.
                if flags & self.interrupt_flags:
                    self.wakes[recipient].append(KernelWake(stamp, None))
                else:
                    self.wakes[recipient].append(KernelWake(stamp, local_threads.get(producer), producer))
        for stamps in self.wakes.values():
            stamps.sort(key=lambda wake: wake.raw_ns)
        self.report = {
            "threads": sum("kernel_events" in row for row in covered),
            "target_wakes": sum(len(row.get("target_wakes", ())) for row in covered),
            "additional_runnable_pauses": sum(len(self.runnable[row["thread"]]) - len(row["preemptions"]) for row in covered),
        }

    @staticmethod
    def switch_points(row):
        pauses = {tuple(pair) for pair in row["preemptions"]}
        blocked = {}
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
            if last == first + 1:
                stamp, state = states[first]
                if state == 0:
                    pauses.add((out, resume))
                else:
                    blocked[out] = stamp
        return sorted(pauses), blocked

    def waking(self, thread, begin, end):
        stamps = self.wakes.get(self.kernel_threads.get(thread), ())
        first = bisect_left(stamps, begin, key=lambda wake: wake.raw_ns)
        last = bisect_right(stamps, end, lo=first, key=lambda wake: wake.raw_ns)
        return stamps[first:last]

    def sleep_wakes(self, thread, out, resumed):
        for entered, returned in self.within(self.futex_waits.get(thread, ()), out, resumed):
            if entered <= out < resumed <= returned:

                # Perf can record the switch-out later than the kernel wake inside the same syscall.
                return self.waking(thread, entered, resumed)
        return self.waking(thread, self.blocked_at.get(thread, {}).get(out, out), resumed)


class FixedHostContext:
    def __init__(self, capture, rank, evidence):
        self.native = evidence.native
        self.control_thread = evidence.window.thread
        activity = evidence.native["host_observation"].get("activity")
        rows = []
        if activity is not None:
            if activity != f"rank-{rank}-thread-activity.jsonl":
                raise ValueError("fixed host activity file belongs to a different worker")
            with (capture / activity).open() as source:
                rows = [json.loads(line) for line in source]

        # Waits triggered by work on other resources still follow their original dependencies.
        observer, dequeues, waits = evidence.replay_host_intervals()
        self.active = FixedThreadActivity(rows, dequeues, [*observer, *waits])
        self.observer = ThreadService.by_thread(observer)
        self.queues = HostQueueEvidence(evidence, self.active)

    def interruptions(self, tid, begin, end):
        activity = [(max(begin, first), min(end, last))
                    for first, last in self.active.within(self.observer.get(tid, ()), begin, end)]
        overhead = sum(last - first for first, last in activity)

        # Explicit observation intervals include their own interruptions. Business pauses are counted once.
        pauses = [(first, last) for first, last in self.active.within(self.active.runnable.get(tid, ()), begin, end)
                  if begin <= first <= last <= end]
        paused = sum(last - first for first, last in pauses) - sum(
            last - first for first, last in self.active.intersect(pauses, activity))
        return overhead, paused

    def idle_waits(self, graph, events, work, notifications):
        if not self.native["request_ingress"] or IngressEvidence.attempts(self.native)["status"] == "unobserved":
            return {}

        thread = self.control_thread
        reads = self.native["request_input_attempts"]
        sleepers = self.active.sleep.get(thread, ())
        completions = {tuple(row["off_cpu_ns"]) for row in notifications}
        excluded = self.active.excluded.get(thread, ())
        window_start, window_end = graph["evidence"]["window_ns"]
        snapshots = {"waiting", "running", "previous", "chunked"}
        cuts = {}

        for prior, received in pairwise(reads):
            read_begin, read_end = received["begin_raw_ns"], received["received_raw_ns"]
            if not window_start < read_end <= window_end or prior["message_count"]:
                continue
            context = received["context"]
            if context is None or not snapshots <= context.keys() or any(context.values()):
                continue
            if received["message_count"] and not received["requests"]:
                continue

            # The receiver was idle across the same serial step, outside any accounted work.
            slot = bisect_right(events, read_begin, key=lambda event: event[1])
            if not 0 < slot < len(events):
                continue
            _, begin, source = events[slot - 1]
            _, end, target = events[slot]
            if read_end > end or (source, target) in work:
                continue
            scope = graph["scopes"][graph["events"][source]["scope"]]
            before = scope["work"].get("context")
            if before is None or any(before.get(name) != [] for name in context):
                continue
            sleeping = [
                (stopped, resumed) for stopped, resumed in self.active.within(sleepers, begin, end)
                if max(begin, prior["received_raw_ns"]) <= stopped < resumed <= read_begin
            ]
            if len(sleeping) != 1:
                continue
            (stopped, resumed), = sleeping
            if (stopped, resumed) in completions or self.active.within(excluded, stopped, resumed):
                continue

            head = self.unmodeled_host_time(thread, begin, stopped)
            tail = self.unmodeled_host_time(thread, resumed, end)
            if head == stopped - begin and tail == end - resumed:
                cuts[read_end] = IdleWait(source, target, stopped, resumed, head, tail), bool(received["requests"])

        if not cuts:
            return cuts
        edges = {(cut.source, cut.target) for cut, _ in cuts.values()}
        ordered = {(edge["source"], edge["target"]) for edge in graph["edges"]
                   if edge["kind"] == "order" and (edge["source"], edge["target"]) in edges}
        return {read_end: (cut, delivered) for read_end, (cut, delivered) in cuts.items()
                if (cut.source, cut.target) in ordered}

    def host_time(self, thread, begin, end, *, modeled, submission):
        if begin == end:
            return 0, {}

        occupied = self.active.duration(thread, begin, end, False)
        overhead, paused = self.interruptions(thread, begin, end)
        service = occupied if modeled else 0

        # Submission service uses dequeue occupancy. Preserve other observed CPU activity as a fixed condition.
        if modeled and submission:
            service = self.active.duration(thread, begin, end, True)
        outside = None if occupied is None or service is None else occupied - service
        conditions = {"observer_ns": overhead, "runnable_pause_ns": paused, "outside_work_ns": outside}
        return None if occupied is None else service, {name: value for name, value in conditions.items() if value}

    def unmodeled_host_time(self, thread, begin, end):
        service, conditions = self.host_time(thread, begin, end, modeled=False, submission=False)
        return None if service is None else sum(conditions.values())

    def progress(self, thread, step, stamp, submissions):
        if step.service_ns is None or (step.modeled and not step.service_ns):
            return None

        service, conditions = self.host_time(thread, step.begin_ns, stamp, modeled=step.modeled,
                                             submission=thread in submissions)
        if service is None:
            return None
        return HostProgress(step.start, step.finish, stamp, service, sum(conditions.values()), step.service_ns)

    @staticmethod
    def step_at(thread, stamp, steps):
        host = steps.get(thread, ())
        position = bisect_right(host, stamp, key=lambda step: step.begin_ns) - 1
        if position >= 0 and stamp < host[position].end_ns:
            return host[position]
        return None

    def progress_at(self, thread, stamp, steps, submissions):
        if step := self.step_at(thread, stamp, steps):
            return self.progress(thread, step, stamp, submissions)
        return None

    def kernel_wait(self, thread, blocked, stopped, steps, submissions, *, read_wake):
        out, resumed = blocked
        if self.active.within(self.active.excluded.get(thread, ()), out, resumed):
            return None
        wake = read_wake
        if wake is None:
            witnesses = self.active.sleep_wakes(thread, out, resumed)
            if len(witnesses) != 1:
                return None
            wake, = witnesses
        stamp, actor = wake.raw_ns, wake.local_thread
        if actor not in steps:

            # The reference resolves unfamiliar producers against all recorded workers.
            unowned = wake.kernel_thread if actor is None else None
            return HostWait(stopped, ExternalWake(stamp, unowned), resumed - stamp)
        progress = self.progress_at(actor, stamp, steps, submissions)
        return None if progress is None else HostWait(stopped, progress, resumed - stamp)

    def queue_wait(self, notice, stopped, positions, steps, submissions):
        producer = notice["producer"]
        taken, completed = notice["dequeue_started_ns"], notice["dequeue_finished_ns"]
        resumed = notice["off_cpu_ns"][1]
        endpoint = positions.get((producer, notice["dequeue_order"]))
        if endpoint is not None:
            return HostWait(stopped, endpoint, resumed - completed)

        arrival = self.queues.unowned_enqueue(notice["sdk_correlation"])
        if arrival is None or arrival[0] in steps or producer not in submissions:
            return None

        _, enqueued = arrival
        step = self.step_at(producer, taken, steps)
        if step is None or step.modeled or not enqueued <= taken < completed <= step.end_ns:
            return None

        entry = self.progress(producer, step, taken, submissions)
        retired = self.progress(producer, step, completed, submissions)
        if entry is None or retired.fixed_prefix_ns - entry.fixed_prefix_ns != completed - taken:
            return None

        # Hold the external command's observed entry and advance with measured serial service to retirement.
        return HostWait(stopped, retired, resumed - completed, take_entry=entry)

    def project(self, graph, host_fences, omitted, details):
        work = {(row["start"], row["finish"]): row["kind"] for row in graph["work"]}
        threads = {lane: int(resource["identity"].split("/")[1]) for lane, resource in enumerate(graph["resources"])
                   if resource["kind"] == "thread" and resource["serial"]}
        lanes, positions = defaultdict(list), {}
        for event, (node, point) in enumerate(zip(graph["events"], graph["evidence"]["events"], strict=True)):
            lane = node["resource"]
            if lane in threads:
                lanes[lane].append((point["order"], point["time_ns"], event))
                positions[threads[lane], point["order"]] = event

        report, arrivals, read_wakes = self.queues.observe()
        report["scheduler_trace"] = self.active.report
        notifications = defaultdict(list)
        for row in report["notifications"]:
            notifications[row["consumer"]].append(row)
        submissions = {thread for lane, thread in threads.items() if graph["resources"][lane]["role"] == "submission"}
        delays, counts = self.queues.handoffs(graph, positions, arrivals)
        waits, idle_inputs = [], {}
        steps = {thread: [] for thread in threads.values()}
        for lane, events in lanes.items():
            events.sort()
            thread = threads[lane]
            idle_steps = set()
            if thread == self.control_thread:
                for read_end, (cut, delivered) in self.idle_waits(graph, events, work, notifications.get(thread, ())).items():
                    idle_steps.add((cut.source, cut.target))
                    if delivered:
                        idle_inputs[read_end] = cut
                    else:
                        delays[cut.source, cut.target] = {"idle_wait_ns": cut.resumed_raw_ns - cut.stopped_raw_ns}
            for (_, begin, source), (_, end, target) in pairwise(events):
                kind = work.get((source, target))
                if begin is None or end is None or begin >= end or kind not in {None, "host"}:
                    continue
                if (source, target) in host_fences:
                    continue

                modeled = kind == "host"
                service, conditions = self.host_time(thread, begin, end, modeled=modeled,
                                                     submission=thread in submissions)
                if conditions:
                    delays.setdefault((source, target), {}).update(conditions)
                if (source, target) not in idle_steps:
                    steps[thread].append(HostStep(source, target, begin, end, modeled, service))

        bound, kernel, external = 0, 0, 0
        for tid, host in steps.items():
            queue_waits = {tuple(row["off_cpu_ns"]): row for row in notifications.get(tid, ())}
            for step in host:
                begin, end = step.begin_ns, step.end_ns
                for asleep, resumed in self.active.within(self.active.sleep.get(tid, ()), begin, end):
                    if not begin <= asleep < resumed <= end:
                        continue
                    stopped = self.progress(tid, step, asleep, submissions)
                    if stopped is None:
                        continue
                    notification = queue_waits.get((asleep, resumed))
                    if notification is not None:
                        wait = self.queue_wait(notification, stopped, positions, steps, submissions)
                    else:
                        blocked = asleep, resumed
                        wait = self.kernel_wait(tid, blocked, stopped, steps, submissions,
                                                read_wake=read_wakes.get((tid, blocked)))
                    if wait is None:
                        continue

                    waits.append(wait)
                    bound += notification is not None
                    kernel += notification is None
                    external += notification is None and isinstance(wait.source, ExternalWake)

        report["bound_to_fixed_work"] = bound
        report["kernel_wakes_bound"] = kernel
        report["external_kernel_wakes_bound"] = external
        report.update(counts)
        if not details:
            del report["notifications"]
        posts = {}
        for record in omitted:
            boundary = positions.get((record.thread, record.order))
            if boundary is None:
                boundary = self.progress_at(record.thread, record.posted_ns, steps, submissions)
            if boundary is not None:
                posts[record] = boundary
        return delays, waits, idle_inputs, posts, report
