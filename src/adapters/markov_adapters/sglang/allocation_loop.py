"""Expose the native index helper's iterations without copying its branch or indexing algorithm."""

import ast
import inspect
import sys
import textwrap


class AllocationIndices:
    def __init__(self, calls):
        self.calls = calls

    def install(self):
        from sglang.srt.mem_cache.allocator.paged import alloc_extend_naive
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        original = inspect.unwrap(alloc_extend_naive)
        lines, start = inspect.getsourcelines(original)
        definition = ast.parse(textwrap.dedent("".join(lines))).body[0]
        loop, = [node for node in definition.body if isinstance(node, ast.For)]
        self.code = original.__code__
        self.begin, self.end = start + loop.lineno - 1, start + loop.end_lineno - 1
        self.parts = {}
        names = {"num1": "existing_page", "num2": "full_pages", "num3": "last_page"}
        for branch in loop.body:
            if isinstance(branch, ast.If) and isinstance(branch.test, ast.Name):
                name = names[branch.test.id]
                for statement in branch.body:
                    for line in range(start + statement.lineno - 1, start + statement.end_lineno):
                        self.parts[line] = name
        HookRegistry.register("sglang.srt.mem_cache.allocator.paged.alloc_extend_naive", self.invoke, HookType.AROUND)

    def invoke(self, original, prefix_lens, seq_lens, last_loc, free_pages, out_indices, page_size, device):
        arguments = (prefix_lens, seq_lens, last_loc, free_pages, out_indices, page_size, device)
        if not self.calls.enabled or self.calls.current.get() is None:
            return original(*arguments)
        previous = sys.gettrace()
        if previous is not None:
            raise RuntimeError("native allocation observation requires ownership of this thread's Python trace")
        iteration = AllocationIteration(self, original, out_indices)
        try:
            sys.settrace(iteration.observe)
            return original(*arguments)
        finally:
            error = sys.exc_info()
            sys.settrace(previous)
            iteration.finish(error)


class AllocationIteration:
    def __init__(self, owner, original, output):
        self.owner, self.original, self.output = owner, original, output
        self.parent = owner.calls.current.get()
        self.active = None

    def observe(self, native, event, argument):
        if native.f_code is not self.owner.code:
            return None
        if event == "line":
            line = native.f_lineno
            if line == self.owner.begin:
                self.finish((None, None, None))
            elif self.owner.begin < line <= self.owner.end:
                if self.active is None:
                    self.start(native.f_locals["i"])
                parts = self.active[1].condition["allocation_parts"]
                part = self.owner.parts.get(line)
                if part is not None and part not in parts:
                    parts.append(part)
        return self.observe

    def start(self, index):
        work = dict(self.parent.work)
        work["context"] = {"member": work["members"][index]}
        inputs = {name: value for name, value in self.parent.tensor_inputs.items() if name != "cache_locations"}
        outputs = {**inputs, "cache_locations": (self.output, ["input_tokens"])}

        # Every member borrows storage identity; only the first registers it, without reading uninitialized contents.
        metadata = {"cache_locations": outputs["cache_locations"]}
        allocations = {"cache_locations": self.output} if index == 0 else {}
        manager = self.owner.calls.call(
            "allocation_member", self.original, work=work, tensor_inputs=inputs, metadata_inputs=metadata,
            tensor_outputs=outputs, allocations=allocations,
            condition={"allocation_parts": []},
        )
        frame = manager.__enter__()
        frame.tensor_outputs = outputs
        self.active = manager, frame

    def finish(self, error):
        if self.active is not None:
            manager, self.active = self.active[0], None
            manager.__exit__(*error)
