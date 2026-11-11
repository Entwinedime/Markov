"""Observe common native resource interfaces without implementing their decisions."""

import inspect
from contextvars import ContextVar
from functools import partial, update_wrapper

from markov_adapters.sglang.calls import ActionCall


class ResourceCalls:
    def __init__(self, calls) -> None:
        self.calls = calls
        self.in_resource = ContextVar("native_resource_implementation", default=False)

    def read(self, description, unit, original, *args, **kwargs):
        if not self.calls.enabled or self.in_resource.get():
            return original(*args, **kwargs)
        parent = self.calls.current.get()
        work = self.calls.scheduler_work() if parent is None else dict(parent.work)
        with self.calls.read("resource", original, work, read=description) as query:
            result = original(*args, **kwargs)

            # The native allocator reports tokens; the common resource service reports whole pages.
            query["result"] = int(result) // unit
        return result

    def bind_reader(self, owner, method, description, unit=1):
        original = getattr(owner, method)
        setattr(owner, method, update_wrapper(partial(self.read, description, unit, original), original))

    def internal(self, original, *args, **kwargs):
        previous = self.in_resource.set(True)
        try:
            return original(*args, **kwargs)
        finally:
            self.in_resource.reset(previous)

    def bind_internal(self, owner, methods):
        for method in methods:
            original = getattr(owner, method)
            setattr(owner, method, update_wrapper(partial(self.internal, original), original))

    def bind_field(self, field, allocator):
        self.bind_reader(allocator, "available_size", {"kind": "state_slots", "field": field, "operation": "available"})
        self.bind_internal(allocator, ("alloc", "free"))

    def invoke(self, operation, owner, original, signature, *args, **kwargs):
        parent = self.calls.current.get()
        if parent is None:
            return original(*args, **kwargs)
        parameters = dict(parent.parameters)
        work = dict(parent.work)
        guards = []
        if operation in {"cache.finished", "cache.unfinished"}:
            request = signature.bind(*args, **kwargs).arguments["req"]
            work["context"] = {"request": self.calls.progress((request,))[0]}
        if operation == "pages.free":
            arguments = signature.bind(*args, **kwargs)
            indices = arguments.arguments[next(iter(signature.parameters))]
            tokens = int(indices.numel())
            grouped = int(not owner.is_not_in_free_group)
            parameters.update(release_tokens=tokens, release_grouped=grouped)

            # Native free first skips empty indices, then either queues them or releases their pages.
            guards = [["eq", "release_tokens", 0] if tokens == 0 else ["lt", 0, "release_tokens"],
                      ["eq", "release_grouped", grouped]]
        elif operation == "pages.flush":
            parameters.update(
                release_calls=len(owner.free_group),
                release_tokens=sum(int(indices.numel()) for indices in owner.free_group),
            )
        elif operation == "pages.sort":

            # Native calls expose array lengths; the control view receives the core's actual ordering result.
            ordering = args[0] if args else {"ready_pages": len(owner.free_pages), "released_pages": len(owner.release_pages)}
            parameters.update(ordering)
            guards = [["lt", 0, "released_pages"] if ordering["released_pages"] else ["eq", "released_pages", 0]]
        with self.calls.call(operation, original, parameters=parameters, work=work, guards=tuple(guards)):
            return original(*args, **kwargs)

    def match(self, owner, original, params):
        parent = self.calls.current.get()
        if not self.calls.enabled or parent is None:
            return original(params)
        boundary = ActionCall(
            "cache_match",
            {"query_tokens": len(params.key), "matched_tokens": 0, "page_size": owner.page_size},
            self.calls.work(None),
        )
        boundary.work["context"] = (
            {"request": self.calls.progress((params.req,))[0]} if params.req is not None else dict(parent.work.get("context", {}))
        )
        with self.calls.separate(boundary.operation, boundary.work), self.calls.boundary(original, boundary):
            result = self.internal(original, params)
            matched = int(result.device_indices.numel())
            boundary.parameters["matched_tokens"] = matched

            # A cache hit materializes indices; a miss returns the existing empty buffer.
            boundary.guards = (["lt", 0, "matched_tokens"] if matched else ["eq", "matched_tokens", 0],)
        return result

    def bind(self, cache, allocator, slots) -> None:

        original = cache.match_prefix
        cache.match_prefix = update_wrapper(partial(self.match, cache, original), original)

        for owner, method, resource, unit in (
            (slots, "available_size", "slots", 1),
            (allocator, "available_size", "pages", allocator.page_size),
            (cache, "evictable_size", "evictable_tokens", 1),
            (cache, "protected_size", "protected_tokens", 1),
        ):
            self.bind_reader(owner, method, {"kind": "capacity", "resource": resource}, unit)

        # The core owns these resource implementations; their internal checks are not native scheduling reads.
        for owner, methods in (
            (slots, ("alloc",)),
            (allocator, ("alloc", "alloc_extend", "alloc_decode")),
            (cache, ("insert", "inc_lock_ref", "dec_lock_ref", "evict")),
        ):
            self.bind_internal(owner, methods)

        # Select the actual objects through their shared ABI, rather than enumerating concrete cache classes.
        for owner, methods in (
            (cache, {"cache_finished_req": "cache.finished", "cache_unfinished_req": "cache.unfinished"}),
            (allocator, {"free": "pages.free", "free_group_end": "pages.flush", "merge_and_sort_free": "pages.sort"}),
            (slots, {"free": "slots.free"}),
        ):
            for name, operation in methods.items():
                original = getattr(owner, name)
                wrapper = partial(self.invoke, operation, owner, original, inspect.signature(original))
                setattr(owner, name, update_wrapper(wrapper, original))
