"""Observe common native resource interfaces without implementing their decisions."""

import inspect
from contextvars import ContextVar
from functools import partial, update_wrapper

from markov_adapters.sglang.calls import ActionCall


class CacheInputs:
    """Describe the request-owned inputs to RadixCache bookkeeping."""

    fields = ("page_size", "cache_tokens", "protected_tokens")
    operations = {"cache.finished", "cache.unfinished"}

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
        "eviction": ("eviction_tokens",),
        "cache.finished": (),
        "cache.unfinished": (),
        "slots.free": (),
        "pages.free": ("page_size", "release_tokens", "release_grouped", "release_index_bytes"),
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
        for scope in scopes:
            operation = scope["bindings"].get("segment", {}).get("owner")
            if operation in cls.declarations:
                owners[scope["id"]] = operation
                owners[scope["parent"]] = operation

        result = []
        for scope in scopes:
            if scope["id"] in owners:
                operation = owners[scope["id"]]
                parameters = cls.bind(operation, scope["parameters"], enabled)
                if enabled and operation in CacheInputs.operations:
                    scope = {**scope, "guards": CacheInputs.guards(operation, parameters)}
                scope = {**scope, "parameters": parameters,
                         "constants": {name: value for name, value in scope["constants"].items() if name in parameters},
                         "arguments": {name: value for name, value in scope["arguments"].items() if name in parameters}}
            result.append(scope)
        return result


class ResourceCalls:
    def __init__(self, calls) -> None:
        self.calls = calls
        self.in_resource = ContextVar("native_resource_implementation", default=False)
        self.prefix_split = ContextVar("native_prefix_split", default=None)

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

    def evict(self, original, tree_cache, num_tokens):
        if self.calls.current.get() is None:
            return original(tree_cache, num_tokens)

        # The scalar resource check owns its CPU loop; nested page releases retain their own execution graphs.
        with self.calls.call("eviction", original, parameters={"eviction_tokens": num_tokens}, host_only=True):
            return original(tree_cache, num_tokens)

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
            width = indices.element_size() if tokens and not grouped else 0
            parameters.update(release_tokens=tokens, release_grouped=grouped, release_index_bytes=width)

            # The immediate path copies indices of their original dtype before combining pages.
            guards = [["eq", "release_tokens", 0] if tokens == 0 else ["lt", 0, "release_tokens"],
                      ["eq", "release_grouped", grouped]]
            if width:
                guards.append(["eq", "release_index_bytes", width])
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

    def record_split(self, prefix, suffix):
        if self.prefix_split.get() is not None:
            self.prefix_split.set((prefix, suffix))

    def split_node(self, original, key, child, split_len):
        self.record_split(split_len, len(key) - split_len)
        return original(key, child, split_len)

    def prefix(self, operation, owner, original, params):
        parent = self.calls.current.get()
        if not self.calls.enabled or parent is None:
            return self.internal(original, params)
        inserting = operation == "cache_insert"
        tokens = len(params.key)
        boundary = ActionCall(
            operation,
            {"query_tokens": tokens, "page_size": owner.page_size},
            self.calls.work(None),
        )
        request = None if inserting else params.req
        boundary.work["context"] = (
            {"request": self.calls.progress((request,))[0]} if request is not None else dict(parent.work.get("context", {}))
        )
        with self.calls.separate(boundary.operation, boundary.work), self.calls.boundary(original, boundary):
            previous = self.prefix_split.set((0, 0))
            try:
                result = self.internal(original, params)
                prefix, suffix = self.prefix_split.get()
            finally:
                self.prefix_split.reset(previous)

            if inserting:
                boundary.parameters["existing_tokens"] = result.prefix_len
                aligned = ["mul", ["floor_div", "query_tokens", "page_size"], "page_size"]
                has_tail = result.prefix_len < tokens // owner.page_size * owner.page_size
                guard = ["lt" if has_tail else "eq", "existing_tokens", aligned]
            else:
                matched = int(result.device_indices.numel())
                boundary.parameters["matched_tokens"] = matched
                guard = ["lt", 0, "matched_tokens"] if matched else ["eq", "matched_tokens", 0]

            # A split copies both sides of an existing node; an insertion can also copy a new suffix.
            boundary.guards = (guard,)
            if not owner.disable:
                boundary.parameters.update(split_prefix_tokens=prefix, split_suffix_tokens=suffix)
                boundary.guards += (["lt", 0, "split_prefix_tokens"] if prefix else ["eq", "split_prefix_tokens", 0],)
        return result

    def bind(self, cache, allocator, slots, *, native_tree=True) -> None:
        operations = {"match_prefix": "cache_match"}
        if not cache.disable:
            operations["insert"] = "cache_insert"
            if native_tree:
                split = cache._split_node
                cache._split_node = update_wrapper(partial(self.split_node, split), split)
            else:
                cache.record_split = self.record_split
        for name, operation in operations.items():
            original = getattr(cache, name)
            setattr(cache, name, update_wrapper(partial(self.prefix, operation, cache, original), original))

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
            (cache, ("inc_lock_ref", "dec_lock_ref", "evict")),
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

    def install(self):
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        # Register before the framework applies hooks; cache objects are bound later during worker construction.
        HookRegistry.register("sglang.srt.mem_cache.common.evict_from_tree_cache", self.evict, HookType.AROUND)
