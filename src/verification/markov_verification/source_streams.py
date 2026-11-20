"""Check retained device coordinates against raw SDK occurrences and elided native waits."""

from bisect import bisect_left, bisect_right
from collections import Counter, defaultdict
from dataclasses import dataclass

from markov_collection.adapters.runtimes.ascend.queues import queue_marker


@dataclass(frozen=True, slots=True)
class OmittedRecord:
    thread: int
    order: int
    posted_ns: int
    completed_device_ns: int


@dataclass(frozen=True, slots=True)
class DeviceRelease:
    submitted: int
    previous: int | None
    preceding_window_ns: tuple[int, int] | None
    first_command: bool
    stream_wait: bool
    wait_finish: int | None
    omitted: tuple[OmittedRecord, ...]


class SdkStreams:
    fields = ("connection_id", "Batch Id", "Task Id", "Subtask Id")
    boundaries = frozenset({"EVENT_RECORD", "EVENT_WAIT", "PROFILING_ENABLE", "PROFILING_DISABLE"})

    def __init__(self, rows, clock, flips):
        self.clock = clock
        self.records = {}
        self.connections = defaultdict(list)
        self.counts = Counter()
        self.logical_gaps = {}
        self.logical_slots = {}
        streams = defaultdict(list)
        for row in rows:
            args = row.get("args", {})
            if row.get("ph") != "X" or "Task Type" not in args:
                continue
            stream = int(args["Physic Stream Id"])
            if queue_marker(args, flips):
                continue
            streams[stream].append(row)

        # SDK numbers can be reused; this table preserves each occurrence within its own stream.
        for stream, tasks in streams.items():
            tasks.sort(key=lambda row: (clock.interval(row)[0], tuple(row["args"][name] for name in self.fields[1:])))
            self.counts[stream] = len(tasks)
            for order, row in enumerate(tasks):
                identity = stream, order
                self.records[identity] = row
                self.connections[row["args"]["connection_id"]].append(identity)

    @classmethod
    def key(cls, args):
        return tuple(args[name] for name in cls.fields)

    def event(self, connection):
        identity, = self.connections[connection]
        if self.records[identity]["args"]["Task Type"] != "EVENT_RECORD":
            raise ValueError("native event has a different SDK task kind")
        return identity

    def span(self, identity):
        stream, occurrence = identity
        position = occurrence + bisect_right(self.logical_gaps.get(stream, ()), occurrence)
        return 2 * position, 2 * position + 1

    def task(self, stream, order):
        slot = order // 2
        logical = self.logical_slots.get(stream, ())
        index = bisect_left(logical, slot)
        if index < len(logical) and logical[index] == slot:
            return None
        return self.records[stream, slot - index]

    def omitted_records(self, stream, ordinal, coordinates, queues, observer_joins):
        records = []
        resource = f"stream/{stream}"
        boundary = self.span((stream, ordinal))[0]
        for previous in range(ordinal - 1, -1, -1):
            identity = stream, previous
            begin, end = self.span(identity)
            if end + 1 != boundary:
                break
            retained = coordinates.get((resource, end))
            if retained is not None and retained not in observer_joins:
                return retained, tuple(reversed(records))

            # A projected observer join may still have recorded on the physical stream.
            task = self.records[identity]
            args = task["args"]
            if args["Task Type"] != "EVENT_RECORD" or args["connection_id"] not in queues.apis:
                break
            completed = self.clock.interval(task)[1]
            if not self.clock.begin_ns <= completed <= self.clock.end_ns:
                break
            try:
                post = queues.release(args)
            except ValueError:
                break
            records.append(OmittedRecord(post.thread, post.begin, post.record["begin_raw_ns"], completed))
            boundary = begin
        return None, ()

    def release_evidence(self, graph, launches, queues, calibration, observer_joins):
        coordinates = {(graph["resources"][event["resource"]]["identity"], point["order"]): index
                       for index, (event, point) in enumerate(zip(graph["events"], graph["evidence"]["events"], strict=True))}
        releases = {}
        for identity, (post, order) in launches.items():
            stream, ordinal = identity
            wait = self.records[identity]["args"]["Task Type"] == "EVENT_WAIT"
            target = coordinates[f"stream/{stream}", order]
            wait_finish = coordinates.get((f"stream/{stream}", self.span(identity)[1])) if wait else None
            submitted = coordinates[f"thread/{post.thread}", post.begin]
            preceding = stream, ordinal - 1
            prior = self.records.get(preceding)
            previous, seen, omitted = None, None, ()
            if prior is not None:
                previous = coordinates.get((f"stream/{stream}", self.span(preceding)[1]))
                completed = self.clock.interval(prior)[1]
                if self.clock.begin_ns <= completed <= self.clock.end_ns:
                    seen = self.clock.bounds(completed)
                    entered = post.record["begin_raw_ns"]
                    if seen[1] > entered:

                        # Retained causal edges tighten the same measured clock range.
                        seen = calibration.point_window(calibration.point(completed, True))
                        if seen[1] > entered and (previous is None or previous in observer_joins):
                            previous, omitted = self.omitted_records(stream, ordinal, coordinates, queues, observer_joins)
            releases[target] = DeviceRelease(submitted, previous, seen, ordinal == 0, wait, wait_finish, omitted)
        return releases

    def inspect_coordinates(self, facts, apis, commands):
        logical = defaultdict(dict)
        for effect in facts["native_effects"]:
            for command in effect["commands"]:
                if command["sdk"] is not None:
                    continue
                begin, end = command["begin"], command["end"]
                stream = int(facts["resources"][effect["resource"]]["identity"].split("/")[1])
                if begin % 2 or end != begin + 1 or begin // 2 in logical[stream]:
                    raise ValueError("elided native waits repeat or change a stream command slot")
                logical[stream][begin // 2] = apis[effect["connection"]]

        positions = {stream: sorted(calls) for stream, calls in logical.items()}
        for stream, slots in positions.items():
            before, after = [], []
            for (peer, order), row in self.records.items():
                if peer != stream:
                    continue
                api = apis.get(row["args"]["connection_id"])
                if api is not None:
                    before.append((api.end, order + 1))
                    after.append((api.begin, order))
            before.sort()
            after.sort()
            lower, upper = [0], [self.counts[stream]] * (len(after) + 1)
            for _, order in before:
                lower.append(max(lower[-1], order))
            for index in range(len(after) - 1, -1, -1):
                upper[index] = min(upper[index + 1], after[index][1])
            ends, starts = [time for time, _ in before], [time for time, _ in after]

            # A zero-submission wait occupies a slot only at the unique gap proved by native API ordering.
            for index, slot in enumerate(slots):
                api = logical[stream][slot]
                left = lower[bisect_right(ends, api.begin)]
                right = upper[bisect_left(starts, api.end)]
                if slot - index != left or left != right:
                    raise ValueError("elided native wait changes or lacks its unique raw SDK position")

        self.logical_gaps = {stream: [slot - index for index, slot in enumerate(slots)] for stream, slots in positions.items()}
        self.logical_slots = positions
        for identity, span in commands.items():
            if span != self.span(identity):
                raise ValueError("native command changes its actual SDK occurrence coordinate")

        checked = 0
        for point in facts["measurements"]["coordinates"]:
            resource = facts["resources"][point["resource"]]
            if resource["kind"] != "stream":
                continue
            stream, order = int(resource["identity"].split("/")[1]), point["order"]
            task = self.task(stream, order)
            measured = None if task is None else self.clock.interval(task)[order % 2]
            if point["time_ns"] != measured:
                raise ValueError("device boundary changes or loses its original SDK measurement")
            checked += measured is not None
        return checked
