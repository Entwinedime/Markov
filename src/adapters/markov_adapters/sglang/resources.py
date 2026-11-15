"""Observe common native resource interfaces without implementing their decisions."""

import inspect
from contextvars import ContextVar
from functools import partial, update_wrapper

from markov_adapters.sglang.calls import ActionCall


class CacheInputs:
    """Lower RadixCache request inputs and observed match returns to scalar copy extents."""

    fields = ("page_size", "cache_tokens", "protected_tokens")
    operations = {"cache.finished", "cache.unfinished"}

    def __init__(self, page_size):
        self.page_size = page_size
        self.protected = {}
        self.calls = {}

    def observe(self, scope):
        if scope["role"] == "cache_match":
            request = scope["work"]["context"]["request"]["id"]
            self.protected[request] = scope["parameters"]["matched_tokens"]

    def parameters(self, scope, owner, operation):
        if owner not in self.calls:
            request = scope["work"]["context"]["request"]

            # Native get_fill_ids ends at extend.end; finished requests release their committed token prefix.
            tokens = request["extend"][1] if operation == "cache.unfinished" else request["committed_tokens"]
            self.calls[owner] = {"page_size": self.page_size, "cache_tokens": tokens,
                                 "protected_tokens": self.protected[request["id"]]}
        return self.calls[owner]

    @staticmethod
    def native(operation, owner, request):
        tokens = len(request.get_fill_ids()) if operation == "cache.unfinished" else request._cache_commit_len()
        return {"page_size": owner.page_size, "cache_tokens": tokens, "protected_tokens": request.cache_protected_len}

    @staticmethod
    def guards(operation, parameters):
        page, tokens, protected = (parameters[name] for name in CacheInputs.fields)
        aligned = tokens // page * page
        if not 0 <= protected <= aligned:
            raise ValueError("native cache inputs have an invalid protected prefix")
        extent = ["mul", ["floor_div", "cache_tokens", "page_size"], "page_size"]
        guards = [["eq", extent, 0] if aligned == 0 else ["lt", 0, extent]]
        if operation == "cache.unfinished":
            tail = ["mod", "cache_tokens", "page_size"]
            guards.append(["eq", tail, 0] if tokens % page == 0 else ["lt", 0, tail])
            guards.append(["eq", extent, "protected_tokens"] if aligned == protected else ["lt", "protected_tokens", extent])
        return guards


class ResourceInputs:
    """Scalar inputs owned by the resource ABI; request context travels separately in work."""

    declarations = {
        "cache.finished": (),
        "cache.unfinished": (),
        "slots.free": (),
        "pages.free": ("page_size", "release_tokens", "release_grouped"),
        "pages.flush": ("page_size", "release_calls", "release_tokens"),
        "pages.sort": ("page_size", "ready_pages", "released_pages"),
    }

    @classmethod
    def bind(cls, operation, values, cache_enabled):
        fields = CacheInputs.fields if cache_enabled and operation in CacheInputs.operations else cls.declarations[operation]
        return {name: values[name] for name in fields}

    @classmethod
    def scopes(cls, scopes, environment):

        # Native records retain the enclosing observation; only formal inputs cross a resource call boundary.
        owners = {}
        enabled = not environment["implementation"]["cache"]["disabled"]
        cache = CacheInputs(environment["implementation"]["page_size"]) if enabled else None
        for scope in scopes:
            operation = scope["bindings"].get("segment", {}).get("owner")
            if operation in cls.declarations:
                owner = scope["parent"]
                owners[scope["id"]] = operation, owner
                owners[owner] = operation, owner

        result = []
        for scope in scopes:
            if cache is not None:
                cache.observe(scope)
            if scope["id"] in owners:
                operation, owner = owners[scope["id"]]
                if cache is not None and operation in CacheInputs.operations:
                    parameters = cache.parameters(scope, owner, operation)
                    guards = list(scope.get("guards", ()))
                    guards.extend(guard for guard in CacheInputs.guards(operation, parameters) if guard not in guards)
                    scope = {**scope, "guards": guards,
                             "arguments": {name: name for name in parameters} if scope["id"] != owner else {}}
                else:
                    parameters = cls.bind(operation, scope["parameters"], enabled)
                scope = {**scope, "parameters": parameters,
                         "constants": {name: value for name, value in scope["constants"].items() if name in parameters},
                         "arguments": {name: value for name, value in scope["arguments"].items() if name in parameters}}
            result.append(scope)
        return result


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
        parameters = {}
        if "page_size" in ResourceInputs.declarations[operation]:
            parameters["page_size"] = owner.page_size
        work = dict(parent.work)
        guards = []
        cache_enabled = operation in CacheInputs.operations and not owner.disable
        if operation in CacheInputs.operations:
            request = signature.bind(*args, **kwargs).arguments["req"]
            work["context"] = {"request": self.calls.progress((request,))[0]}
            if cache_enabled:
                parameters.update(CacheInputs.native(operation, owner, request))
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
        with self.calls.call(operation, original, parameters=ResourceInputs.bind(operation, parameters, cache_enabled), work=work, guards=tuple(guards)):
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
