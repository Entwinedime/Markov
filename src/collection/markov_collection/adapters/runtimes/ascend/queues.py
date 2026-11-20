"""Resolve actual queue submissions and transport services from native driver evidence."""

from bisect import bisect_left, bisect_right
from collections import defaultdict
from dataclasses import dataclass
from functools import cached_property
from itertools import accumulate, pairwise


def queue_marker(task, flips):
    return (task["Task Type"] == "PLACE_HOLDER_SQE"
            and (int(task["Physic Stream Id"]), int(task["Task Id"]), int(task["Batch Id"])) in flips)


class NativeQueues:
    def __init__(self, runtime, apis, rows, flips) -> None:
        self.apis, self.flips = apis, flips
        self.device = [row["args"] for row in rows if row.get("ph") == "X" and "Task Type" in row.get("args", {})]
        self.resolved = {}
        queues = {}
        self.submissions = []
        calls = defaultdict(list)
        for native in sorted(runtime, key=lambda value: value.begin):
            row = native.record
            if not row["api"].startswith("halSq"):
                continue
            if row["result"] != 0:
                raise ValueError("recorded driver queue operation failed")
            key = row["device"], row["task_system"], row["queue"]
            if row["api"] == "halSqCqAllocate":
                queues[key] = row["stream"]
            elif row["api"] == "halSqCqFree":
                del queues[key]
            elif row["api"] == "halSqTaskSend":
                stream = queues[key]
                if type(row["tasks"]) is not int or row["tasks"] < 0:
                    raise ValueError("driver queue posting requires an observed nonnegative SQE count")
                self.submissions.append((stream, native))
                calls[native.thread, stream].append(native)
        self.calls = {key: ([call.begin for call in values], values) for key, values in calls.items()}

    def inside(self, api, stream: int) -> list:
        starts, calls = self.calls.get((api.thread, stream), ([], []))
        selected = calls[bisect_left(starts, api.begin):bisect_right(starts, api.end)]
        if any(call.end > api.end for call in selected):
            raise ValueError("native queue submission crosses its enclosing API boundary")
        return selected

    @staticmethod
    def slot(task):
        return int(task["Batch Id"]), int(task["Task Id"])

    @cached_property
    def physical(self):
        streams = defaultdict(lambda: defaultdict(set))
        for task in self.device:
            stream = int(task["Physic Stream Id"])
            connection = None if queue_marker(task, self.flips) else task["connection_id"]
            streams[stream][self.slot(task)].add(connection)

        # Several SDK subtasks can describe one physical SQE; a confirmed flip still consumes its own slot.
        physical = {}
        for stream, units in streams.items():
            ordered = sorted(units.items())
            owned = defaultdict(list)
            for index, (_, connections) in enumerate(ordered):
                for connection in connections:
                    if connection is not None:
                        owned[connection].append(index)
            physical[stream] = ordered, owned
        return physical

    @cached_property
    def issued(self):
        streams = defaultdict(list)
        for stream, post in self.submissions:
            if post.record["tasks"]:
                streams[stream].append(post)
        return {stream: ([post.begin for post in posts], posts) for stream, posts in streams.items()}

    def positive_posts(self, api, stream):
        return [post for post in self.inside(api, stream) if post.record["tasks"]]

    def posting_range(self, stream, posts):
        if not posts:
            return None
        begins, issued = self.issued[stream]
        first = bisect_left(begins, posts[0].begin)
        after = first + len(posts)
        if issued[first:after] != posts or any(left.end > right.begin for left, right in pairwise(posts)):
            return None
        return first, after

    def release(self, task):
        stream, connection = int(task["Physic Stream Id"]), task["connection_id"]
        key = stream, connection
        bindings = self.resolved.get(key)
        if bindings is None:
            api = self.apis[connection]
            posts = self.positive_posts(api, stream)
            if len(posts) == 1:
                return posts[0]
            bindings = self.assign_slots(stream, connection, posts)
            self.resolved[key] = bindings
        return bindings[self.slot(task)]

    def assign_slots(self, stream, connection, posts):
        def missing():
            api = self.apis[connection]
            return ValueError(f"stream {stream} lacks a unique driver release for {api.record['name']} connection {connection}")

        posting = self.posting_range(stream, posts)
        if posting is None:
            raise missing()

        first_post, after_post = posting
        _, issued = self.issued[stream]
        units, owned = self.physical[stream]
        matching = owned[connection]
        sizes = list(accumulate(post.record["tasks"] for post in posts))
        width = sizes[-1]
        initial, final = matching[0], matching[-1]
        starts = []
        for start in range(max(0, final + 1 - width), min(initial, len(units) - width) + 1):
            stop = start + width
            selected = units[start:stop]
            if any(owners not in ({connection}, {None}) for _, owners in selected):
                continue
            if not self.consecutive(selected):
                continue
            if width != len(matching):

                # An uncorrelated flip belongs here only if independent neighbor posts bound the whole group.
                if start == 0 or stop == len(units) or first_post == 0 or after_post == len(issued):
                    continue
                if not self.consecutive(units[start - 1:stop + 1]):
                    continue
                if not (self.anchors(stream, start - 1, issued[first_post - 1], posts[0])
                        and self.anchors(stream, stop, issued[after_post], posts[-1])):
                    continue
            starts.append(start)
        if len(starts) != 1:
            raise missing()

        start, = starts
        return {units[index][0]: posts[bisect_right(sizes, index - start)] for index in matching}

    @staticmethod
    def consecutive(units):
        for ((batch, task), _), ((next_batch, next_task), next_owners) in pairwise(units):
            if next_batch == batch and next_task == task + 1:
                continue
            if next_batch != batch + 1 or next_owners != {None}:
                return False
        return True

    def anchors(self, stream, position, neighbor, adjacent):
        units, owned = self.physical[stream]
        _, owners = units[position]
        if len(owners) != 1:
            return False
        connection, = owners
        api = self.apis.get(connection)
        if api is None:
            return False
        if neighbor.end <= adjacent.begin:
            boundary = -1
        elif adjacent.end <= neighbor.begin:
            boundary = 0
        else:
            return False

        posts = self.positive_posts(api, stream)
        if not posts or posts[boundary] is not neighbor:
            return False
        if len(posts) == 1:
            return True

        # A multi-post neighbor fixes the boundary when all of its own slots are positively identified.
        positions = owned[connection]
        tagged = units[positions[0]:positions[-1] + 1]
        return (position == positions[boundary] and len(tagged) == sum(post.record["tasks"] for post in posts)
                and all(connections == {connection} for _, connections in tagged)
                and self.consecutive(tagged) and self.posting_range(stream, posts) is not None)


@dataclass
class ServiceExecution:
    scope: int
    thread: int
    begin: int
    end: int


class ServiceExecutions:
    def __init__(self, scopes: list[dict], queues: NativeQueues) -> None:
        owners = {scope["id"]: scope for scope in scopes}
        self.executions = [
            ServiceExecution(scope["id"], call["thread"], call["begin_counter"], call["end_counter"])
            for scope in scopes for call in scope["service"]["executions"]
        ]
        by_owner = defaultdict(list)
        for index, call in enumerate(self.executions):
            interval = owners[call.scope]["host_interval"]
            if not interval["begin_counter"] <= call.begin <= call.end <= interval["end_counter"]:
                raise ValueError("transport execution escapes its synchronous service boundary")
            by_owner[call.scope, call.thread].append((index, call))
        self.submissions = {}
        queue_owners = {}
        submitted = set()
        for stream, native in queues.submissions:
            row = native.record
            if row["service"] < 0:
                continue
            matches = [
                (index, call) for index, call in by_owner[row["service"], native.thread]
                if call.begin <= native.begin and native.end <= call.end
            ]
            if len(matches) != 1:
                raise ValueError("native queue submission lacks one explicit service execution")
            (index, call), = matches

            # Shared queues must remain ordered by the caller thread retained in the coarse graph.
            owner = owners[call.scope]["thread"]
            if queue_owners.setdefault(stream, owner) != owner:
                raise ValueError("transport queue crosses service callers; its ordering requires an explicit interface")
            submitted.add(index)
            self.submissions.setdefault(stream, []).append((native, index))

        self.streams = set(self.submissions)
        if len(submitted) != len(self.executions):
            raise ValueError("transport execution has no native queue submission evidence")
        if any(stream in self.streams and native.record["service"] < 0 for stream, native in queues.submissions):
            raise ValueError("a transport queue also carries work outside its declared services")

    def assign(self, tasks: list) -> None:
        grouped = {stream: [] for stream in self.streams}
        for task in tasks:
            if task.stream in grouped:
                grouped[task.stream].append(task)
        for stream, submissions in self.submissions.items():
            if any(left[0].end > right[0].begin for left, right in zip(submissions, submissions[1:])):
                raise ValueError("concurrent private-queue submissions have no established order")

            # Driver submission sizes define correspondence, independently of CPU/device clock calibration.
            payload = sorted(grouped[stream], key=lambda task: task.begin)
            expected = sum(native.record["tasks"] for native, _ in submissions)
            if len(payload) != expected:
                raise ValueError(f"private queue {stream} submission/task coverage differs: {expected}/{len(payload)}")
            position = 0
            for native, execution in submissions:
                count = native.record["tasks"]
                for task in payload[position:position + count]:
                    task.service = self.executions[execution].scope
                position += count
