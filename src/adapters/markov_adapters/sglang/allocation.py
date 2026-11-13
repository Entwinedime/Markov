"""Declare native allocation branches and expose parameter-dependent iterations."""

import __future__
import ast
import inspect
import sys
import textwrap


class AllocationABI:
    def __init__(self, implementation: str) -> None:
        if implementation != "sglang.srt.hardware_backend.npu.allocator_npu.NPUPagedTokenToKVPoolAllocator":
            raise ValueError(f"allocation structure has no declared implementation: {implementation}")
        self.implementation = implementation

    def branch(self, members, page_size, decode):
        if page_size == 1:
            return "token"
        pages = sum(
            (member["context_tokens"] + member["new_tokens"] + page_size - 1) // page_size
            - (member["context_tokens"] + page_size - 1) // page_size
            for member in members
        )
        if decode:
            return "decode_pages" if pages else "decode_tail"

        return self.extend_branch(pages)

    @staticmethod
    def extend_branch(pages):

        # NPUPagedTokenToKVPoolAllocator.alloc_extend selects its native kernel below 200 new pages.
        return "extend_kernel" if pages < 200 else "extend_naive"


class AllocationPrefixes:
    """Wrap each native prefix expression, leaving its branch and enclosing loop native."""

    def __init__(self, calls) -> None:
        from sglang.srt.mem_cache.common import alloc_for_extend

        self.calls = calls
        self.original = inspect.unwrap(alloc_for_extend)
        lines, start = inspect.getsourcelines(self.original)
        native = ast.parse(textwrap.dedent("".join(lines))).body[0]
        ast.increment_lineno(native, start - 1)
        assignment, = [node for node in ast.walk(native) if isinstance(node, ast.Assign)
                       and any(isinstance(target, ast.Name) and target.id == "last_loc" for target in node.targets)
                       and isinstance(node.value, ast.ListComp)]
        comprehension = assignment.value
        generator, = comprehension.generators
        item, batch = generator.target.id, native.args.args[0].arg

        # Compile the installed expression, rather than copying its index/empty-prefix algorithm into this adapter.
        value = ast.parse("def prefix_value(prefix, batch):\n    return None").body[0]
        value.args.args[0].arg, value.args.args[1].arg = item, batch
        value.body[0].value = comprehension.elt
        ast.copy_location(value, comprehension.elt)
        ast.copy_location(value.body[0], comprehension.elt)
        index = "markov_prefix_index"
        arguments = [ast.Name(name, ast.Load()) for name in ("prefix_value", index, item, batch)]
        comprehension.elt = ast.copy_location(ast.Call(ast.Name("observe", ast.Load()), arguments, []), comprehension.elt)
        generator.iter = ast.copy_location(ast.Call(ast.Name("enumerate", ast.Load()), [generator.iter], []), generator.iter)
        generator.target = ast.copy_location(ast.Tuple([ast.Name(index, ast.Store()), generator.target], ast.Store()), generator.target)

        # The closure holds only the instrumentation callback; native globals stay live for the framework's other hooks.
        factory = ast.parse("def instrument(observe):\n    pass").body[0]
        ast.copy_location(factory, native)
        factory.body = [value, native, ast.copy_location(ast.Return(ast.Name(native.name, ast.Load())), native.body[-1])]
        module = ast.fix_missing_locations(ast.Module(body=[factory], type_ignores=[]))
        namespace = {}
        flags = self.original.__code__.co_flags & __future__.annotations.compiler_flag
        exec(compile(module, self.original.__code__.co_filename, "exec", flags=flags, dont_inherit=True),
             self.original.__globals__, namespace)
        self.instrumented = namespace["instrument"](self.prefix)

    def install(self):
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        HookRegistry.register("sglang.srt.mem_cache.common.alloc_for_extend", self.allocate, HookType.REPLACE)

    def allocate(self, batch):
        if not self.calls.enabled or self.calls.current.get() is None:
            return self.original(batch)
        return self.instrumented(batch)

    def prefix(self, native, index, prefix, batch):
        parent = self.calls.current.get()
        work = {**parent.work, "context": {"member": parent.work["members"][index]}}

        # The private index value is consumed after this synchronous construction/view returns; only its causal boundary escapes.
        with self.calls.call("allocation_prefix", self.original, parameters={}, work=work, condition={"cached": len(prefix) > 0}):
            return native(prefix, batch)


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
