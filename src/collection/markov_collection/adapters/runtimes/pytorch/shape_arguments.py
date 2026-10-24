"""Recover view dimensions from native Python expressions without replacing native arguments."""

from __future__ import annotations

import ast
import inspect
import operator
import textwrap
from collections import ChainMap, Counter
from types import ModuleType
from typing import Any

import torch


class ShapeArguments:
    arithmetic = {
        ast.Add: operator.add,
        ast.Sub: operator.sub,
        ast.Mult: operator.mul,
        ast.FloorDiv: operator.floordiv,
        ast.Mod: operator.mod,
    }

    def __init__(self, values: Any, sizes: dict[int, tuple[Any, tuple[Any, ...]]]) -> None:
        self.values = values
        self.sizes = sizes
        self.calls: dict[Any, tuple] = {}

        # Keep relations and code identities only; native frames and tensors retain their original lifetimes.
        self.bindings: dict[tuple[Any, int, str], Any] = {}

    @staticmethod
    def caller(frame: Any, function: Any) -> Any:
        while frame.f_code.co_name == "__torch_function__" and frame.f_locals.get("function") is function:
            frame = frame.f_back
        return frame

    def source(self, frame: Any) -> tuple:
        if frame.f_code not in self.calls:
            lines, first = inspect.getsourcelines(frame.f_code)
            tree = ast.parse(textwrap.dedent("".join(lines)))
            writes = Counter(
                node.id for node in ast.walk(tree) if isinstance(node, ast.Name) and isinstance(node.ctx, ast.Store)
            )
            statements = tree.body[0].body if isinstance(tree.body[0], (ast.FunctionDef, ast.AsyncFunctionDef)) else []
            assignments = {
                statement.targets[0].id: statement.value
                for statement in statements
                if isinstance(statement, ast.Assign)
                and len(statement.targets) == 1
                and isinstance(statement.targets[0], ast.Name)
                and writes[statement.targets[0].id] == 1
            }
            self.calls[frame.f_code] = (
                first,
                [node for node in ast.walk(tree) if isinstance(node, ast.Call)],
                assignments,
                writes,
            )
        return self.calls[frame.f_code]

    def remember(self, frame: Any, function: Any) -> None:
        frame = self.caller(frame, function)
        first, _, assignments, _ = self.source(frame)
        line = frame.f_lineno - first + 1
        for name, expression in assignments.items():
            if not expression.lineno <= line <= expression.end_lineno:
                continue
            try:
                value = self.expression(expression, frame)
            except NotImplementedError:
                continue
            if isinstance(value, (int, torch.SymInt)) or (
                isinstance(value, tuple) and all(isinstance(axis, (int, torch.SymInt)) for axis in value)
            ):

                # Snapshot the relation at the actual metadata read, before a tensor can be rebound or resized.
                self.bindings[frame.f_code, id(frame), name] = value

    def binding(self, name: str, frame: Any) -> Any:
        _, _, assignments, writes = self.source(frame)
        key = (frame.f_code, id(frame), name)
        if name in assignments and key in self.bindings:
            return self.bindings[key]
        if name in assignments and isinstance(assignments[name], ast.Constant):
            return assignments[name].value

        # Only unchanged formal parameters can inherit the expression supplied by their actual Python caller.
        arguments = frame.f_code.co_varnames[: frame.f_code.co_argcount + frame.f_code.co_kwonlyargcount]

        # shortcut: assigned locals need an observed metadata read; expand dataflow only for a required native path.
        if writes[name] or name not in arguments:
            raise NotImplementedError(f"Python scalar {name} needs a dimension source")
        caller = frame.f_back
        first, calls, _, _ = self.source(caller)
        line = caller.f_lineno - first + 1
        matches = []
        for call in calls:
            if not call.lineno <= line <= call.end_lineno:
                continue
            try:
                target = self.expression(call.func, caller)
            except NotImplementedError:
                continue
            if getattr(target, "__code__", None) is frame.f_code:
                matches.append((call, target))
        if len(matches) == 1:
            call, target = matches[0]
            for keyword in call.keywords:
                if keyword.arg == name:
                    return self.expression(keyword.value, caller)
            index = arguments.index(name) - int(inspect.ismethod(target))
            if index < len(call.args) and not any(isinstance(argument, ast.Starred) for argument in call.args):
                return self.expression(call.args[index], caller)
        raise NotImplementedError(f"Python parameter {name} needs one explicit caller argument")

    def resolve(self, frame: Any, function: Any, args: tuple, kwargs: dict) -> tuple[tuple, dict]:
        if not args or not isinstance(args[0], torch.Tensor):
            raise NotImplementedError("view dimensions require a positional input tensor")
        if len(args) == 2:
            captured = self.sizes.get(id(args[1]))
            if captured is not None and captured[0] is args[1]:
                return (args[0], captured[1]), kwargs

        frame = self.caller(frame, function)
        first, calls, _, _ = self.source(frame)
        line = frame.f_lineno - first + 1
        arity = len(args) - int(function is not torch.reshape)
        matches = [
            call
            for call in calls
            if call.lineno <= line <= call.end_lineno
            and isinstance(call.func, ast.Attribute)
            and call.func.attr == function.__name__
            and (len(call.args) == arity or any(isinstance(argument, ast.Starred) for argument in call.args))
        ]
        if len(matches) > 1:
            matches = [
                call
                for call in matches
                if (
                    self.expression(call.func, frame) is function
                    if function is torch.reshape
                    else self.expression(call.func.value, frame) is args[0]
                )
            ]
        if len(matches) != 1:
            raise NotImplementedError(f"view needs one shape callsite at {frame.f_code.co_filename}:{frame.f_lineno}")

        # Only metadata expressions are evaluated; the original tensor call keeps its actual arguments.
        call = matches[0]
        dimensions = call.args[1:] if function is torch.reshape else call.args
        resolved = [args[0]]
        for dimension in dimensions:
            if isinstance(dimension, ast.Starred):
                resolved.extend(self.expression(dimension.value, frame))
            else:
                resolved.append(self.expression(dimension, frame))
        keywords = {keyword.arg: self.expression(keyword.value, frame) for keyword in call.keywords}
        if None in keywords:
            raise NotImplementedError("expanded view keywords need an explicit geometry declaration")
        return tuple(resolved), keywords

    def expression(self, node: ast.expr, frame: Any) -> Any:
        if isinstance(node, ast.Constant):
            return node.value
        if isinstance(node, (ast.Tuple, ast.List)):
            return tuple(self.expression(item, frame) for item in node.elts)
        if isinstance(node, ast.Name):
            value = ChainMap(frame.f_locals, frame.f_globals, frame.f_builtins)[node.id]
            if isinstance(value, torch.Size):
                captured = self.sizes.get(id(value))
                if captured is not None and captured[0] is value:
                    return captured[1]
                raise NotImplementedError(f"Size {node.id} has no observed shape source")

            if isinstance(value, (int, float, tuple)):
                return self.binding(node.id, frame)
            if isinstance(value, list):
                raise NotImplementedError("mutable Python shape lists need an explicit geometry declaration")
            return value
        if isinstance(node, ast.Attribute):
            value = self.expression(node.value, frame)
            if isinstance(value, torch.Tensor):
                tensor = self.values.shapes.tensor(value)
                if node.attr == "shape":
                    return tensor.shape
                if node.attr == "ndim":
                    return tensor.ndim
            elif isinstance(value, torch.nn.Module):
                for fields in (vars(value), value._parameters, value._buffers, value._modules):
                    if node.attr in fields:
                        return fields[node.attr]
                member = inspect.getattr_static(value, node.attr, None)
                if inspect.isfunction(member) or isinstance(member, staticmethod):
                    return member.__get__(value, type(value))
            elif isinstance(value, ModuleType):
                return vars(value)[node.attr]
        if isinstance(node, ast.Subscript):
            value = self.expression(node.value, frame)
            index = self.expression(node.slice, frame)
            if isinstance(value, (tuple, list, torch.Size)) and isinstance(index, int):
                return value[index]
        if isinstance(node, ast.UnaryOp) and isinstance(node.op, ast.USub):
            return -self.expression(node.operand, frame)
        if isinstance(node, ast.BinOp) and type(node.op) in self.arithmetic:
            return self.arithmetic[type(node.op)](self.expression(node.left, frame), self.expression(node.right, frame))
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute):
            tensor = self.expression(node.func.value, frame)
            if isinstance(tensor, torch.Tensor) and node.func.attr in ("size", "dim", "numel") and not node.keywords:
                fake = self.values.shapes.tensor(tensor)
                return getattr(fake, node.func.attr)(*(self.expression(argument, frame) for argument in node.args))
        raise NotImplementedError(f"view dimension expression requires a provider: {ast.unparse(node)}")
