"""Add scopes to specific native statements while retaining their original bodies."""

from __future__ import annotations

import ast
import inspect
import textwrap
import types
from abc import abstractmethod
from collections.abc import Callable
from typing import Any

from markov_collection.instrumentation.blocks import BlockWrapper, BoundBlock
from markov_collection.instrumentation.recording import Recorder


class SGLangInlineRegion(BlockWrapper):
    def __init__(self, observer: Any, contract: dict[str, Any], binding: dict[str, Any]) -> None:
        parameters = tuple(parameter["name"] for parameter in contract["definition"]["parameters"])
        super().__init__((BoundBlock(binding["id"], binding["step"], binding["phase"], parameters),))
        self.observer = observer
        self.binding = binding

    @abstractmethod
    def parameters(self, *args: Any) -> tuple[dict[str, int], str | None]:
        """Read only host metadata for this inline region."""

    @abstractmethod
    def rewrite(self, method: ast.FunctionDef) -> None:
        """Wrap one declared native statement or loop body in place."""

    def context(self, *args: Any) -> tuple[Recorder, dict[str, Any]] | None:
        recorder = self.observer.recorder
        if recorder is None or not recorder.in_scope():
            return None

        parameters, request_id = self.parameters(*args)
        return recorder, {
            "module_path": self.binding["id"],
            "module_type": self.binding["module_type"],
            "method": self.binding["method"],
            "module_parameters": {},
            "call_parameters": parameters,
            "inputs": {},
            "outputs": {},
            "cpu_values": {},
            "request_id": request_id,
        }

    def scope_statement(self, body: list[ast.stmt], arguments: list[str]) -> ast.With:
        scope = ast.Call(
            func=ast.Subscript(
                value=ast.Name(id="_markov_region_scopes", ctx=ast.Load()),
                slice=ast.Constant(value=self.binding["id"]),
                ctx=ast.Load(),
            ),
            args=[ast.Name(id=name, ctx=ast.Load()) for name in arguments],
            keywords=[],
        )
        region = ast.With(items=[ast.withitem(context_expr=scope)], body=body)
        return ast.copy_location(region, body[0])

    def wrap_prefix(self, method: ast.FunctionDef, boundary_call: str, arguments: list[str]) -> None:
        expected = ast.dump(ast.parse(boundary_call, mode="eval").body)
        positions = [
            index
            for index, statement in enumerate(method.body)
            if any(isinstance(node, ast.Call) and ast.dump(node) == expected for node in ast.walk(statement))
        ]
        first = method.body[0]
        # Keep a native docstring as function metadata, outside the runtime scope.
        start = int(
            isinstance(first, ast.Expr) and isinstance(first.value, ast.Constant) and isinstance(first.value.value, str)
        )
        if len(positions) != 1 or positions[0] <= start:
            raise ValueError("native prefix boundary does not match the declared binding")

        boundary = positions[0]
        method.body = [
            *method.body[:start],
            self.scope_statement(method.body[start:boundary], arguments),
            *method.body[boundary:],
        ]


class SGLangInlineMethod:
    """Compile all declared regions of one native method in a single rewrite."""

    def __init__(self, regions: list[SGLangInlineRegion]) -> None:
        self.regions = regions
        self.instrumented: Callable[..., Any] | None = None

    def instrument(self, original: Callable[..., Any]) -> Callable[..., Any]:
        source, line = inspect.getsourcelines(original)
        tree = ast.parse(textwrap.dedent("".join(source)))
        method = tree.body[0]
        if not isinstance(method, ast.FunctionDef):
            raise ValueError("inline observation requires a native function")

        native = original
        if method.decorator_list:
            expected = ast.parse(f'scheduler_nvtx_method("scheduler.{method.name}")', mode="eval").body
            if len(method.decorator_list) != 1 or ast.dump(method.decorator_list[0]) != ast.dump(expected):
                raise ValueError("inline observation does not support this native decorator")
            # Reapply the native profiler decorator from its original AST. Only
            # its immediate body is accepted; another plugin wrapper is not skipped.
            native = original.__wrapped__
        if native.__closure__:
            raise ValueError("inline observation requires a native body without a closure")

        for region in self.regions:
            region.rewrite(method)
        ast.increment_lineno(tree, line - 1)

        # Capture only the region scopes. Native globals remain the original live
        # module dictionary; no helper is injected into that module's namespace.
        factory = ast.parse("def _markov_bind(_markov_region_scopes):\n    pass\n").body[0]
        factory.body = [method, ast.Return(value=ast.Name(id=method.name, ctx=ast.Load()))]
        module = ast.fix_missing_locations(ast.Module(body=[factory], type_ignores=[]))
        namespace: dict[str, Any] = {}
        code = compile(module, native.__code__.co_filename, "exec")
        exec(code, namespace)
        bind = types.FunctionType(namespace["_markov_bind"].__code__, native.__globals__)
        return bind({region.binding["id"]: region.scope for region in self.regions})

    def invoke(self, original: Callable[..., Any], *args: Any, **kwargs: Any) -> Any:
        if self.instrumented is None:
            self.instrumented = self.instrument(original)

        return self.instrumented(*args, **kwargs)


class SGLangRequestPublication(SGLangInlineRegion):
    def parameters(self, req: Any, index: int) -> tuple[dict[str, int], str | None]:
        return {"request_index": index}, req.rid

    def rewrite(self, method: ast.FunctionDef) -> None:
        # The other prefill loop serves embedding/reward models. Match structure,
        # not source line numbers, and preserve continue/exception cleanup.
        prefill = self.binding["method"] == "process_batch_result_prefill"
        iterator = "enumerate(zip(batch.reqs, next_token_ids))" if prefill else "enumerate(batch.reqs)"
        expected = ast.dump(ast.parse(iterator, mode="eval").body)
        loops = [node for node in ast.walk(method) if isinstance(node, ast.For) and ast.dump(node.iter) == expected]
        if len(loops) != 1:
            raise ValueError("native request publication loop does not match the declared binding")

        loop = loops[0]
        expected_target = "(i, (req, next_token_id))" if prefill else "(i, req)"
        target = ast.parse(f"for {expected_target} in requests: pass").body[0].target
        if ast.dump(loop.target) != ast.dump(target):
            raise ValueError("native request publication loop variables do not match the declared binding")

        loop.body = [self.scope_statement(loop.body, ["req", "i"])]


class SGLangBatchTransition(SGLangInlineRegion):
    def parameters(self, scheduler: Any) -> tuple[dict[str, int], str | None]:
        previous = scheduler.last_batch
        pending = previous is not None and previous.forward_mode.is_extend()
        return {
            "running_requests": len(scheduler.running_batch.reqs),
            "has_pending_prefill": int(pending),
            "pending_prefill_requests": len(previous.reqs) if pending else 0,
            "batch_is_full": int(scheduler.running_batch.batch_is_full),
        }, None

    def rewrite(self, method: ast.FunctionDef) -> None:
        self.wrap_prefix(method, "self.get_new_batch_prefill()", ["self"])


class SGLangAdmissionPrefix(SGLangInlineRegion):
    def parameters(self, scheduler: Any) -> tuple[dict[str, int], str | None]:
        return {
            "waiting_requests": len(scheduler.waiting_queue),
            "running_requests": len(scheduler.running_batch.reqs),
            "batch_is_full": int(scheduler.running_batch.batch_is_full),
        }, None

    def rewrite(self, method: ast.FunctionDef) -> None:
        self.wrap_prefix(method, "new_batch.prepare_for_extend()", ["self"])

        region = next(statement for statement in method.body if isinstance(statement, ast.With))
        region.items[0].optional_vars = ast.Name(id="_markov_admission", ctx=ast.Store())
        initialize = ast.parse(
            "if _markov_admission is not None:\n"
            '    _markov_admission.result["admission_work"] = {\n'
            '        "candidate_checks": 0, "request_checks": 0, "queue_compaction_items": 0,\n'
            '        "native_stop": "early_return"\n'
            "    }\n"
        ).body

        # Count executed native statements, including a candidate rejected before
        # add_one_req. No extra event scopes or scheduler-owned state are needed.
        expected_loop = ast.dump(ast.parse("self.waiting_queue", mode="eval").body)
        loops = [
            node for node in ast.walk(region) if isinstance(node, ast.For) and ast.dump(node.iter) == expected_loop
        ]
        if len(loops) != 1:
            raise ValueError("native admission candidate loop does not match the declared binding")

        loop = loops[0]
        # Preserve the native distinction between capacity and AddReqResult.
        # OTHER has several causes; it must not be relabelled as input budget.
        exhausted = ast.parse(
            "if _markov_admission is not None:\n"
            '    _markov_admission.result["admission_work"]["native_stop"] = "queue_exhausted"\n'
        ).body
        position = region.body.index(loop)
        region.body[position:position] = exhausted

        exits = [
            node
            for node in ast.walk(loop)
            if isinstance(node, ast.If) and any(isinstance(item, ast.Break) for item in node.body)
        ]
        expected_exits = {
            "not self.enable_priority_preemption or not adder.preempt_to_schedule(req, self.server_args)": (
                '"request_capacity"'
            ),
            "res != AddReqResult.CONTINUE": "res.name",
        }
        if {ast.unparse(node.test) for node in exits} != set(expected_exits):
            raise ValueError("native admission exits do not match the declared binding")

        for branch in exits:
            reason = expected_exits[ast.unparse(branch.test)]
            position = next(index for index, statement in enumerate(branch.body) if isinstance(statement, ast.Break))
            branch.body[position:position] = ast.parse(
                "if _markov_admission is not None:\n"
                f'    _markov_admission.result["admission_work"]["native_stop"] = {reason}\n'
            ).body

        loop.body[:0] = ast.parse(
            "if _markov_admission is not None:\n"
            '    _markov_admission.result["admission_work"]["candidate_checks"] += 1\n'
        ).body

        requests = [
            index
            for index, statement in enumerate(loop.body)
            if isinstance(statement, ast.Assign)
            and isinstance(statement.value, ast.Call)
            and ast.unparse(statement.value.func) == "adder.add_one_req"
        ]
        compactions = [
            index
            for index, statement in enumerate(region.body)
            if isinstance(statement, ast.Assign)
            and ast.unparse(statement) == "self.waiting_queue = [x for x in self.waiting_queue if x not in can_run_set]"
        ]
        if len(requests) != 1 or len(compactions) != 1:
            raise ValueError("native admission work statements do not match the declared binding")

        loop.body[requests[0] : requests[0]] = ast.parse(
            'if _markov_admission is not None:\n    _markov_admission.result["admission_work"]["request_checks"] += 1\n'
        ).body
        region.body[compactions[0] : compactions[0]] = ast.parse(
            "if _markov_admission is not None:\n"
            '    _markov_admission.result["admission_work"]["queue_compaction_items"] = len(self.waiting_queue)\n'
        ).body
        region.body[:0] = initialize


class SGLangAdmissionBudget(SGLangInlineRegion):
    def parameters(self, adder: Any, req: Any) -> tuple[dict[str, int], str | None]:
        return {
            "running_requests": len(adder.running_batch.reqs) if adder.running_batch is not None else 0,
            "accepted_requests": len(adder.can_run_list),
            "input_tokens": req.extend_input_len,
        }, None

    def rewrite(self, method: ast.FunctionDef) -> None:
        expected = ast.dump(ast.parse("enumerate(self.req_states)", mode="eval").body)
        loops = [node for node in ast.walk(method) if isinstance(node, ast.For) and ast.dump(node.iter) == expected]
        if len(loops) != 1:
            raise ValueError("native admission horizon loop does not match the declared binding")

        # One region per budget call, not one event per comparison. Rejected
        # candidates belong to admission and need not be members of the batch.
        loops[0].body[:0] = ast.parse(
            'if _markov_budget is not None:\n    _markov_budget.result["horizon_checks"] += 1\n'
        ).body
        self.record_returns(method)
        region = self.scope_statement(method.body, ["self", "req"])
        region.items[0].optional_vars = ast.Name(id="_markov_budget", ctx=ast.Store())
        region.body[:0] = ast.parse(
            'if _markov_budget is not None:\n    _markov_budget.result["horizon_checks"] = 0\n'
        ).body
        method.body = [region]

    def record_returns(self, node: ast.AST) -> None:
        # Keep nested helpers intact. Their returns do not finish this budget
        # call, and the native result expression must be evaluated exactly once.
        for _, value in ast.iter_fields(node):
            if not isinstance(value, list):
                continue

            rewritten = []
            for statement in value:
                if isinstance(statement, ast.Return):
                    result = ast.parse(
                        "_markov_budget_return = None\n"
                        "if _markov_budget is not None:\n"
                        "    _markov_budget.result.update({\n"
                        f'        "return_site": {statement.lineno},\n'
                        '        "native_result": _markov_budget_return.name,\n'
                        '        "candidate_accepted": bool(self.can_run_list and self.can_run_list[-1] is req),\n'
                        "    })\n"
                        "return _markov_budget_return\n"
                    ).body
                    result[0].value = statement.value
                    rewritten.extend(ast.copy_location(item, statement) for item in result)
                else:
                    if isinstance(statement, ast.stmt) and not isinstance(
                        statement, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)
                    ):
                        self.record_returns(statement)
                    rewritten.append(statement)
            value[:] = rewritten


class SGLangPrefillStatistics(SGLangInlineRegion):
    def parameters(self, scheduler: Any) -> tuple[dict[str, int], str | None]:
        return {
            "waiting_requests": len(scheduler.waiting_queue),
            "running_requests": len(scheduler.running_batch.reqs),
            "has_chunked_request": int(scheduler.chunked_req is not None),
        }, None

    def rewrite(self, method: ast.FunctionDef) -> None:
        assignments = [
            index
            for index, statement in enumerate(method.body)
            if isinstance(statement, ast.Assign)
            and len(statement.targets) == 1
            and ast.unparse(statement.targets[0]) == "new_batch.prefill_stats"
            and isinstance(statement.value, ast.Call)
            and ast.unparse(statement.value.func) == "PrefillStats.from_adder"
        ]
        if len(assignments) != 1:
            raise ValueError("native prefill statistics do not match the declared binding")

        index = assignments[0]
        # Keep the post-Prepare scan separate from the admission prefix. Its
        # queue length is observed here, after native selection and compaction.
        method.body[index] = self.scope_statement([method.body[index]], ["self"])


class SGLangDecodeUpdatePrefix(SGLangInlineRegion):
    def parameters(self, batch: Any) -> tuple[dict[str, int], str | None]:
        return {
            "request_count": len(batch.reqs),
            "batch_is_full": int(batch.batch_is_full),
            "filter_prefix_count": len(self.observer._schedule_filters),
        }, None

    def rewrite(self, method: ast.FunctionDef) -> None:
        self.wrap_prefix(method, "batch.prepare_for_decode()", ["batch"])


class SGLangLastLocations(SGLangInlineRegion):
    def parameters(self, batch: Any) -> tuple[dict[str, int], str | None]:
        return {
            "request_count": len(batch.reqs),
            "cold_prefix": int(all(len(req.prefix_indices) == 0 for req in batch.reqs)),
        }, None

    def assignment(self, method: ast.FunctionDef) -> ast.Assign:
        expected = ast.parse(
            "[t[-1:] if len(t) > 0 else torch.tensor([-1], device=batch.device) for t in prefix_tensors]",
            mode="eval",
        ).body
        assignments = [
            node
            for node in ast.walk(method)
            if isinstance(node, ast.Assign)
            and len(node.targets) == 1
            and isinstance(node.targets[0], ast.Name)
            and node.targets[0].id == "last_loc"
        ]
        if len(assignments) != 1 or ast.dump(assignments[0].value) != ast.dump(expected):
            raise ValueError("native last-location construction does not match the declared binding")

        return assignments[0]

    def rewrite(self, method: ast.FunctionDef) -> None:
        assignment = self.assignment(method)
        # Preserve the comprehension itself, including its local variable scope.
        # Cat remains outside this region with its own existing operation cut.
        for parent in ast.walk(method):
            for _, value in ast.iter_fields(parent):
                if isinstance(value, list) and assignment in value:
                    value[value.index(assignment)] = self.scope_statement([assignment], ["batch"])
                    return


class SGLangLastLocation(SGLangLastLocations):
    def parameters(self, batch: Any, index: int, prefix: Any) -> tuple[dict[str, int], str | None]:
        req = batch.reqs[index]
        return {"request_index": index, "cold_prefix": int(len(prefix) == 0)}, req.rid

    def rewrite(self, method: ast.FunctionDef) -> None:
        assignment = self.assignment(method)
        comprehension = assignment.value
        helper = ast.parse("def _markov_last_location(t, _markov_request_index):\n    pass\n").body[0]
        # The native expression runs inside its own scope. Returning its tensor
        # leaves list insertion and iterator advancement in the aggregate region.
        expression_return = ast.Return(value=comprehension.elt)
        helper.body = [self.scope_statement([expression_return], ["batch", "_markov_request_index", "t"])]
        comprehension.elt = ast.parse("_markov_last_location(t, _markov_request_index)", mode="eval").body
        generator = comprehension.generators[0]
        generator.target = ast.Tuple(
            elts=[ast.Name(id="_markov_request_index", ctx=ast.Store()), generator.target],
            ctx=ast.Store(),
        )
        generator.iter = ast.Call(func=ast.Name(id="enumerate", ctx=ast.Load()), args=[generator.iter], keywords=[])

        for parent in ast.walk(method):
            for _, value in ast.iter_fields(parent):
                if isinstance(value, list) and assignment in value:
                    value.insert(value.index(assignment), ast.copy_location(helper, assignment))
                    return
