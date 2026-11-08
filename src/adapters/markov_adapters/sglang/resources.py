"""Observe common native resource interfaces without implementing their decisions."""

import inspect
from functools import partial, update_wrapper

from markov_adapters.sglang.calls import ActionCall


class ResourceCalls:
    def __init__(self, calls) -> None:
        self.calls = calls

    def invoke(self, operation, original, signature, *args, **kwargs):
        parent = self.calls.current.get()
        if parent is None:
            return original(*args, **kwargs)
        parameters = dict(parent.parameters)
        work = dict(parent.work)
        guards = []
        owner = original.__self__
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
        with self.calls.call(operation, original, parameters=parameters, work=work, guards=tuple(guards)):
            return original(*args, **kwargs)

    def match(self, original, params):
        parent = self.calls.current.get()
        if not self.calls.enabled or parent is None:
            return original(params)
        boundary = ActionCall(
            "cache_match",
            {"query_tokens": len(params.key), "matched_tokens": 0, "page_size": original.__self__.page_size},
            self.calls.work(None),
        )
        boundary.work["context"] = (
            {"request": self.calls.progress((params.req,))[0]} if params.req is not None else dict(parent.work.get("context", {}))
        )
        with self.calls.separate(boundary.operation, boundary.work), self.calls.boundary(original, boundary):
            result = original(params)
            matched = int(result.device_indices.numel())
            boundary.parameters["matched_tokens"] = matched

            # A cache hit materializes indices; a miss returns the existing empty buffer.
            boundary.guards = (["lt", 0, "matched_tokens"] if matched else ["eq", "matched_tokens", 0],)
        return result

    def bind(self, cache, allocator, slots) -> None:

        original = cache.match_prefix
        cache.match_prefix = update_wrapper(partial(self.match, original), original)

        # Select the actual objects through their shared ABI, rather than enumerating concrete cache classes.
        for owner, methods in (
            (cache, {"cache_finished_req": "cache.finished", "cache_unfinished_req": "cache.unfinished"}),
            (allocator, {"free": "pages.free", "free_group_end": "pages.flush"}),
            (slots, {"free": "slots.free"}),
        ):
            for name, operation in methods.items():
                original = getattr(owner, name)
                wrapper = partial(self.invoke, operation, original, inspect.signature(original))
                setattr(owner, name, update_wrapper(wrapper, original))
