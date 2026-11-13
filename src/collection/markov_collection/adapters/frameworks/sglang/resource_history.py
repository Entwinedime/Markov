"""Observe actual resource reads and mutations without replaying their policy."""

import threading
import time
from contextlib import contextmanager
from dataclasses import dataclass
from functools import partial, update_wrapper


@dataclass
class IndexValues:
    tensor: object
    offset: int

    def document(self):
        return [int(value) - self.offset for value in self.tensor.tolist()]


class NativeResourceHistory:
    def __init__(self, observation) -> None:
        self.observation = observation
        self.events = []
        self.reads = []
        self.bindings = []
        self.initial = None
        self.final = None
        self.fields = {}
        self.state_events = []
        self.state_free = {}

    @property
    def enabled(self):
        return self.observation.active and self.observation.selected

    def indices(self, values, offset):
        if values is None:
            return None

        # Freeze mutable device views asynchronously; CPU materialization happens only after the window drains.
        with self.observation.tensors.metadata():
            return IndexValues(values.detach().clone(), offset)

    @staticmethod
    def node(value):
        return None if value is None else value.id

    def available(self):

        # Diagnostic snapshots call the original readers, never the hooks for real control observations.
        return {name: int(original()) // unit for name, (original, unit) in self.capacity_readers.items()}

    def begin(self):
        self.initial = {
            "page_size": self.page_size, "page_capacity": self.allocator.size // self.page_size,
            "free_slots": [int(slot) - 1 for slot in self.slots.free_slots],
            "free_pages": self.indices(self.allocator.free_pages, 1),
            "released_pages": self.indices(self.allocator.release_pages, 1),
            "available": self.available(),
            "state_slots": {field: {"capacity": pool.size, "free": list(pool.free_slots)} for field, pool in self.fields.items()},
        }

    def bind_field(self, field, allocator):
        self.fields[field] = allocator
        original = allocator.available_size
        operation = {"kind": "state_slots", "field": field, "operation": "available"}
        allocator.available_size = update_wrapper(partial(self.read_capacity, operation, 1, original), original)
        for name, operation in (("alloc", "allocate"), ("free", "release")):
            original = getattr(allocator, name)
            setattr(allocator, name, update_wrapper(partial(self.state_operation, field, operation, original), original))

    def state_operation(self, field, operation, original, *args):
        if not self.enabled:
            return original(*args)
        record = {"field": field, "operation": operation}
        if operation == "release":
            record["slot"], = args
        self.state_events.append(record)
        result = original(*args)
        if operation == "allocate":
            record["slot"] = result
        return result

    def state_binding(self, field, request, slot):
        if self.enabled:
            self.state_events.append({
                "field": field, "operation": "bind", "request": self.observation.content.aliases[request.rid], "slot": slot,
            })

    def batch(self, scope, batch):
        if not self.enabled:
            return

        # Read the table consumed by attention, including cached prefixes, rather than replaying allocator results.
        with self.observation.tensors.metadata():
            for req, slot, length in zip(batch.reqs, batch.req_pool_indices_cpu.tolist(), batch.seq_lens_cpu.tolist(), strict=True):
                self.bindings.append({
                    "scope": scope, "request": self.observation.content.aliases[req.rid],
                    "slot": int(slot) - 1, "begin": 0,
                    "indices": self.indices(self.slots.req_to_token[slot, :length], self.page_size),
                })

    @staticmethod
    def page_ranges(indices, page_size, capacity):
        ranges = []
        for index in indices:
            if not 0 <= index < capacity:
                raise ValueError("native request binding points outside the KV pool")
            page, offset = divmod(index, page_size)
            if ranges and ranges[-1][0] == page and ranges[-1][2] == offset:
                ranges[-1][2] += 1
            else:
                ranges.append([page, offset, offset + 1])
        return ranges

    @contextmanager
    def event(self, operation, *, query=False):
        capture = self.observation.capture
        scope = capture.current.get()

        # Resource wrappers can suspend the current segment; its invocation still encloses the whole operation.
        if capture.scopes[scope]["role"] == "segment":
            scope = capture.scopes[scope]["parent"]
        record = {
            "scope": scope, "thread": threading.get_native_id(),
            "operation": operation, "begin_raw_ns": time.clock_gettime_ns(time.CLOCK_MONOTONIC_RAW),
        }
        (self.reads if query else self.events).append(record)
        yield record
        record["end_raw_ns"] = time.clock_gettime_ns(time.CLOCK_MONOTONIC_RAW)
        if not query:
            record["available"] = self.available()

    def read_capacity(self, operation, unit, original, *args, **kwargs):
        if not self.enabled:
            return original(*args, **kwargs)
        with self.event(operation, query=True) as record:
            record["control"] = not self.observation.calls.resources.in_resource.get()
            result = original(*args, **kwargs)
        record["result"] = int(result) // unit
        return result

    def allocate_slots(self, original, reqs):
        if not self.enabled:
            return original(reqs)
        requests = [self.observation.content.aliases[req.rid] for req in reqs]
        with self.event({"kind": "slots", "requests": requests}) as record:
            result = original(reqs)
        record["result"] = None if result is None else [int(slot) - 1 for slot in result]
        return result

    def release_slot(self, original, req):
        if not self.enabled:
            return original(req)
        operation = {
            "kind": "release_request", "request": self.observation.content.aliases[req.rid],
            "slot": int(req.req_pool_idx) - 1,
        }
        with self.event(operation):
            return original(req)

    def allocate_pages(self, original, *args, **kwargs):
        if not self.enabled:
            return original(*args, **kwargs)
        with self.event({"kind": "allocate", "entry": original.__name__}) as record:
            result = original(*args, **kwargs)
        record["indices"] = self.indices(result, self.page_size)
        return result

    def release_pages(self, original, free_index):
        if not self.enabled:
            return original(free_index)
        operation = {
            "kind": "release_pages", "grouped": not self.allocator.is_not_in_free_group,
            "indices": self.indices(free_index, self.page_size),
        }
        with self.event(operation):
            return original(free_index)

    def match(self, original, params):
        if not self.enabled:
            return original(params)
        operation = {"kind": "prefix", "operation": "match", "key_tokens": list(params.key.raw_token_ids())}
        with self.event(operation) as record:
            result = original(params)
        record["result"] = {"node": self.node(result.last_device_node), "indices": self.indices(result.device_indices, self.page_size)}
        return result

    def insert(self, original, params):
        if not self.enabled:
            return original(params)
        operation = {
            "kind": "prefix", "operation": "insert", "key_tokens": list(params.key.raw_token_ids()),
            "indices": self.indices(params.value, self.page_size),
        }
        with self.event(operation) as record:
            result = original(params)
        record["result"] = {"node": self.node(result.last_device_node), "existing_tokens": result.prefix_len}
        return result

    def lock(self, acquire, original, node, *args, **kwargs):
        if not self.enabled:
            return original(node, *args, **kwargs)
        with self.event({"kind": "prefix", "operation": "lock", "node": self.node(node), "acquire": acquire}) as record:
            result = original(node, *args, **kwargs)
        record["result"] = result.delta
        return result

    def evict(self, original, params):
        if not self.enabled:
            return original(params)
        with self.event({"kind": "prefix", "operation": "evict", "tokens": params.num_tokens}) as record:
            result = original(params)
        record["result"] = result.num_tokens_evicted
        return result

    def bind(self, cache, allocator, slots):
        self.cache, self.allocator, self.slots = cache, allocator, slots
        self.page_size = allocator.page_size
        self.capacity_readers = {}
        for owner, method, resource, unit in (
            (slots, "available_size", "slots", 1),
            (allocator, "available_size", "pages", self.page_size),
            (cache, "evictable_size", "evictable_tokens", 1),
            (cache, "protected_size", "protected_tokens", 1),
        ):
            original = getattr(owner, method)
            self.capacity_readers[resource] = original, unit
            operation = {"kind": "capacity", "resource": resource}
            setattr(owner, method, update_wrapper(partial(self.read_capacity, operation, unit, original), original))

        for owner, methods in (
            (slots, {"alloc": self.allocate_slots, "free": self.release_slot}),
            (allocator, {"alloc": self.allocate_pages, "alloc_extend": self.allocate_pages,
                         "alloc_decode": self.allocate_pages, "free": self.release_pages}),
            (cache, {"match_prefix": self.match, "insert": self.insert, "inc_lock_ref": partial(self.lock, True),
                     "dec_lock_ref": partial(self.lock, False), "evict": self.evict}),
        ):
            for name, hook in methods.items():
                original = getattr(owner, name)
                setattr(owner, name, update_wrapper(partial(hook, original), original))

    @classmethod
    def materialize(cls, value):
        if isinstance(value, IndexValues):
            return value.document()
        if isinstance(value, dict):
            return {key: cls.materialize(item) for key, item in value.items()}
        if isinstance(value, list):
            return [cls.materialize(item) for item in value]
        return value

    def close(self):

        # Native flush resets the pools next; preserve the end-of-serving state before it runs.
        self.final = self.available()
        self.state_free = {field: list(pool.free_slots) for field, pool in self.fields.items()}
        self.initial = self.materialize(self.initial)
        self.events = self.materialize(self.events)
        self.bindings = self.materialize(self.bindings)
        for binding in self.bindings:
            binding["ranges"] = self.page_ranges(binding.pop("indices"), self.page_size, self.allocator.size)

    def document(self):
        return {
            "rank": self.observation.parallel["rank"], "clock": "monotonic_raw_ns",
            "initial": self.initial, "final": self.final, "events": self.events, "bindings": self.bindings,
            "state_events": self.state_events,
            "state_free": self.state_free,
            "reads": self.reads,
        }
