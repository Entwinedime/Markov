"""Use PyTorch fake implementations to produce finite, offline shape relations."""

from __future__ import annotations

import inspect
import sys
from dataclasses import dataclass
from typing import Any

import sympy
import torch
from torch._subclasses.fake_tensor import FakeTensorMode
from torch.fx.experimental.symbolic_shapes import ShapeEnv
from torch.overrides import TorchFunctionMode
from torch.utils import _pytree
from torch.utils._python_dispatch import _disable_current_modes
from torch.utils.weak import WeakIdKeyDictionary

from markov_collection.adapters.runtimes.pytorch.factories import TensorCreationCall
from markov_collection.adapters.runtimes.pytorch.indexing import TensorAssignmentCall
from markov_collection.adapters.runtimes.pytorch.shape_arguments import ShapeArguments


@dataclass(frozen=True)
class GeometryCall:
    function: Any
    arguments: tuple[Any, ...]
    keywords: dict[str, Any]
    origin: str
    operations: frozenset[str]


class ShapeFunctions(TorchFunctionMode):
    view_operations = frozenset({"aten::view", "aten::_unsafe_view", "aten::reshape", "aten::flatten"})
    factory_operations = frozenset({"aten::empty", "aten::zeros", "aten::ones", "aten::full"})

    def __init__(self, values: Any) -> None:
        super().__init__()
        self.values = values
        self.sizes: dict[int, tuple[Any, tuple[Any, ...]]] = {}
        self.python_arguments = ShapeArguments(values, self.sizes)
        self.geometry: GeometryCall | None = None
        self.scalar_slice: tuple[Any, dict[int, Any]] | None = None

    def reset(self) -> None:
        self.sizes.clear()
        self.python_arguments.bindings.clear()
        self.geometry = None
        self.scalar_slice = None

    @staticmethod
    def geometry_arguments(
        schema: Any, arguments: list[Any], shape: list[Any], geometry: GeometryCall, relations: ShapeRelations
    ) -> list[Any] | None:

        if geometry.origin == "scalar_slice":
            result = list(arguments)
            result[3] = {"scalar": relations.expression(geometry.arguments[3])}
            return result

        # The resolved geometry retains whole-Size provenance and inferred view dimensions.
        for index, parameter in enumerate(schema.arguments):
            if parameter.name in {"size", "shape"} and index < len(arguments):
                result = list(arguments)
                result[index] = {"shape": shape}
                return result

        return None

    def scalar_geometry(self, operation: Any, args: tuple[Any, ...], kwargs: dict[str, Any]) -> GeometryCall | None:
        if self.scalar_slice is None or operation._schema.name != "aten::slice":
            return None

        tensor, bounds = self.scalar_slice
        if args[0] is not tensor or args[1] not in bounds:
            return None

        arguments = list(args)
        arguments[3] = bounds[args[1]]
        arguments = _pytree.tree_map_only(torch.Tensor, self.values.shapes.tensor, tuple(arguments))
        keywords = _pytree.tree_map_only(torch.Tensor, self.values.shapes.tensor, kwargs)
        return GeometryCall(operation, arguments, keywords, "scalar_slice", frozenset({"aten::slice"}))

    def size_arguments(self, value: Any) -> Any:
        captured = self.sizes.get(id(value))
        if captured is not None and captured[0] is value:
            return captured[1]
        if isinstance(value, (tuple, list)):
            return type(value)(self.size_arguments(item) for item in value)
        if isinstance(value, dict):
            return {name: self.size_arguments(item) for name, item in value.items()}

        return value

    def __torch_function__(self, function: Any, types: Any, args: tuple = (), kwargs: dict | None = None) -> Any:
        kwargs = kwargs or {}
        if self.values.recording.current_scope.get() is None:
            return function(*args, **kwargs)

        if function in (torch.Tensor.cpu, torch.Tensor.tolist) and args[0].device.type != "cpu":
            with self.values.metadata():
                source = self.values.value(args[0])

            # The host-read boundary owns materialization; its wall time can include a device dependency.
            with self.values.recording.scope("HostRead", source=source, path=function.__name__):
                return function(*args, **kwargs)

        if function == torch._C._VariableFunctions.tensor:
            data = args[0] if args else kwargs["data"]
            origin = self.values.tensor_geometry
            geometry = origin.data_geometry(data) if origin is not None else None
            return TensorCreationCall(
                self.values.recording,
                self.values,
                "torch.tensor",
                function,
                {},
                interface="tensor_from_data",
                geometry=geometry,
            )(*args, **kwargs)

        if self.values.interface is not None:
            return function(*args, **kwargs)

        if function is torch.Tensor.__getitem__:
            indices = args[1] if isinstance(args[1], tuple) else (args[1],)
            bounds = {
                axis: self.values.shapes.scalars[index.stop]
                for axis, index in enumerate(indices)
                if isinstance(index, slice)
                and isinstance(index.stop, torch.Tensor)
                and index.stop in self.values.shapes.scalars
            }
            if bounds:
                if any(not isinstance(index, (slice, torch.Tensor)) for index in indices):
                    raise NotImplementedError("scalar slice provenance requires explicit Tensor/slice axes")

                # Keep the native read and indexing operations; only fake geometry receives the declared scalar.
                previous = self.scalar_slice
                self.scalar_slice = (args[0], bounds)
                try:
                    return function(*args, **kwargs)
                finally:
                    self.scalar_slice = previous

        if function is torch.Tensor.contiguous:
            metadata = self.values.recording.regions[self.values.recording.current_scope.get()]["metadata"]
            with self.values.metadata(), _disable_current_modes():
                try:
                    memory_format = kwargs.get("memory_format", torch.contiguous_format)
                    if memory_format != torch.contiguous_format:
                        raise NotImplementedError(
                            "contiguous condition requires a layout provider for this memory format"
                        )

                    condition = self.values.shapes.contiguous_condition(args[0])
                    observed = args[0].is_contiguous(memory_format=memory_format)
                    if isinstance(condition, int):
                        if bool(condition) != observed:
                            raise ValueError("symbolic contiguous layout disagrees with the native input")
                    else:
                        metadata.setdefault("guards", []).append({"condition": condition, "expected": observed})
                except (NotImplementedError, RuntimeError, ValueError) as error:
                    metadata.setdefault("requirements", []).append(f"contiguous layout: {str(error).splitlines()[0]}")

            return function(*args, **kwargs)

        # Indexing privately strips leading size-one dimensions; the public assignment has stable ports.
        if function is torch.Tensor.__setitem__:
            return TensorAssignmentCall(
                self.values.recording,
                self.values,
                "torch.Tensor.__setitem__",
                function,
                {},
                interface="tensor_assignment",
                mutations=(0,),
            )(*args, **kwargs)

        if getattr(function, "__self__", None) in (torch.Tensor.shape, torch.Tensor.ndim) or function in (
            torch.Tensor.size,
            torch.Tensor.dim,
            torch.Tensor.numel,
        ):
            result = function(*args, **kwargs)
            if args[0] not in self.values.shapes.unavailable:
                with self.values.metadata():
                    shape = self.values.shapes.tensor(args[0]).shape

                    # Keep the actual Size object. Its identity carries provenance without integer matching or taint.
                    if isinstance(result, torch.Size):
                        self.sizes[id(result)] = (result, tuple(shape))
                    try:
                        self.python_arguments.remember(sys._getframe(1), function)
                    except (OSError, SyntaxError):
                        pass

            return result

        views = {torch.Tensor.view, torch.Tensor.reshape, torch.reshape, torch.Tensor.flatten, torch.flatten}
        factories = {inspect.unwrap(function) for function in (torch.empty, torch.zeros, torch.ones, torch.full)}
        if function not in views | factories or any(
            value in self.values.shapes.unavailable
            for value in _pytree.tree_leaves(args)
            if isinstance(value, torch.Tensor)
        ):
            return function(*args, **kwargs)

        with self.values.metadata():
            arguments = self.size_arguments(args)
            keywords = self.size_arguments(kwargs)
            if function in (torch.Tensor.view, torch.Tensor.reshape, torch.reshape):
                try:
                    arguments, keywords = self.python_arguments.resolve(sys._getframe(1), function, args, kwargs)
                except (NotImplementedError, OSError, SyntaxError) as error:
                    metadata = self.values.recording.regions[self.values.recording.current_scope.get()]["metadata"]
                    metadata.setdefault("requirements", []).append(f"view dimensions: {error}")

            origin = self.values.tensor_geometry
            declared = origin.factory_geometry(function) if origin is not None else None
            if declared is not None:
                shape = tuple(
                    self.values.shapes.symbol(value) if isinstance(value, str) else value for value in declared
                )
                if arguments:
                    arguments = (shape, *arguments[1:])
                else:
                    keywords["size"] = shape

            fake_args = _pytree.tree_map_only(torch.Tensor, self.values.shapes.tensor, arguments)
            fake_kwargs = _pytree.tree_map_only(torch.Tensor, self.values.shapes.tensor, keywords)

        # Keep symbolic arguments active only while the matching native function dispatches its operators.
        previous = self.geometry
        self.geometry = GeometryCall(
            function,
            fake_args,
            fake_kwargs,
            function.__name__,
            self.factory_operations if function in factories else self.view_operations,
        )
        try:
            return function(*args, **kwargs)
        finally:
            self.geometry = previous


class ShapeRelations:
    def __init__(self) -> None:
        self.environment = ShapeEnv()
        self.mode = FakeTensorMode(shape_env=self.environment, allow_fallback_kernels=False)

        self.tensors: Any = WeakIdKeyDictionary()
        self.unavailable: Any = WeakIdKeyDictionary()
        self.scalars: Any = WeakIdKeyDictionary()

        self.symbols: dict[str, Any] = {}
        self.names: dict[Any, str] = {}

    def symbol(self, name: str) -> Any:
        if name not in self.symbols:
            value = self.environment.create_unbacked_symint()
            self.symbols[name] = value
            self.names[value.node.expr] = name

        return self.symbols[name]

    def bind(self, tensor: torch.Tensor, axes: dict[int, str]) -> None:
        shape = [self.symbol(axes[index]) if index in axes else size for index, size in enumerate(tensor.shape)]

        # Source interfaces own dynamic axes. Equal observed integers never create bindings.
        self.bind_shape(tensor, shape)

    def bind_shape(self, tensor: torch.Tensor, shape: Any) -> None:

        # Native interfaces bind the leading work axis; their observed strides remain part of that layout.
        with torch._C.DisableTorchFunction(), _disable_current_modes(), self.mode:
            self.tensors[tensor] = torch.empty_strided(shape, tensor.stride(), dtype=tensor.dtype, device=tensor.device)

        self.unavailable.pop(tensor, None)

    def bind_contiguous(self, tensor: torch.Tensor, shape: Any) -> None:

        # A contiguous source allocation derives its strides and storage from every bound axis.
        with torch._C.DisableTorchFunction(), _disable_current_modes(), self.mode:
            self.tensors[tensor] = torch.empty(shape, dtype=tensor.dtype, device=tensor.device)
        self.unavailable.pop(tensor, None)

    def tensor(self, tensor: torch.Tensor) -> Any:
        if tensor in self.unavailable:
            raise RuntimeError(f"input shape relation unavailable: {self.unavailable[tensor]}")
        if tensor not in self.tensors:
            with torch._C.DisableTorchFunction(), _disable_current_modes():
                self.tensors[tensor] = self.mode.from_tensor(tensor, static_shapes=True)

        return self.tensors[tensor]

    def propagate(
        self,
        operation: Any,
        args: tuple[Any, ...],
        kwargs: dict[str, Any],
        result: Any,
        geometry: GeometryCall | None = None,
    ) -> str | None:
        real_leaves, real_tree = _pytree.tree_flatten(result)

        # Scalar/void calls introduce no output geometry; their state writes are recorded from the schema.
        if not any(isinstance(value, torch.Tensor) for value in real_leaves):
            return None

        try:
            if geometry is None:
                fake_args = _pytree.tree_map_only(torch.Tensor, self.tensor, args)
                fake_kwargs = _pytree.tree_map_only(torch.Tensor, self.tensor, kwargs)
            else:
                operation, fake_args, fake_kwargs = geometry.function, geometry.arguments, geometry.keywords

            with torch._C.DisableTorchFunction(), _disable_current_modes(), self.mode:
                unchanged_alias = (
                    isinstance(operation, torch._ops.OpOverload)
                    and operation.namespace == "aten"
                    and operation._schema.name.endswith("_")
                    and torch.Tag.inplace_view not in operation.tags
                    and isinstance(result, torch.Tensor)
                    and any(result is value for value in _pytree.tree_leaves((args, kwargs)))
                )

                # Ordinary in-place writes retain the destination geometry; broadcasting adds no output axis.
                predicted = self.tensor(result) if unchanged_alias else operation(*fake_args, **fake_kwargs)

            # Reject source-specific specialization before publishing a supposedly reusable relation.
            specialized = {
                self.names[symbol]: str(value)
                for symbol, value in self.environment.replacements.items()
                if symbol in self.names
            }
            if specialized:
                raise ValueError(f"shape analysis specialized source parameters: {specialized}")

            for value in _pytree.tree_leaves(predicted):
                if isinstance(value, torch.Tensor):
                    for size in (*value.shape, *value.stride()):
                        self.expression(size)

            fake_leaves, fake_tree = _pytree.tree_flatten(predicted)
            if real_tree != fake_tree:
                raise ValueError("fake result structure differs from native result")
        except (NotImplementedError, RuntimeError, ValueError) as error:
            reason = f"{type(error).__name__}: {str(error).splitlines()[0]}"

            # Unsupported fake/custom operations remain explicit production requirements.
            for value in _pytree.tree_leaves(result):
                if isinstance(value, torch.Tensor):
                    self.unavailable[value] = str(operation)
                    self.tensors.pop(value, None)

            return reason

        for real, fake in zip(real_leaves, fake_leaves, strict=True):
            if isinstance(real, torch.Tensor):
                self.tensors[real] = fake

        return None

    def expression(self, value: Any) -> Any:
        if value is sympy.true or value is sympy.false:
            return int(value is sympy.true)
        if isinstance(value, int):
            return value
        if isinstance(value, torch.SymInt):
            value = value.node.expr
        if value in self.names:
            return self.names[value]
        if isinstance(value, sympy.Integer):
            return int(value)

        operations = {
            sympy.Add: "add",
            sympy.Mul: "mul",
            sympy.Max: "max",
            sympy.Min: "min",
            sympy.Equality: "eq",
            sympy.StrictLessThan: "lt",
            sympy.LessThan: "le",
            sympy.And: "and",
            sympy.Or: "or",
        }
        name = operations.get(value.func)
        if value.func.__name__ in ("FloorDiv", "Mod", "Min", "Max"):
            name = {"FloorDiv": "floor_div", "Mod": "mod", "Min": "min", "Max": "max"}[value.func.__name__]
        if name is None:
            raise ValueError(f"shape relation is outside the finite expression contract: {value}")

        return [name, *(self.expression(argument) for argument in value.args)]

    def contiguous_condition(self, tensor: torch.Tensor) -> Any:
        fake = self.tensor(tensor)
        shape = [size.node.expr if isinstance(size, torch.SymInt) else size for size in fake.shape]
        strides = [stride.node.expr if isinstance(stride, torch.SymInt) else stride for stride in fake.stride()]

        # Size-one axes impose no stride constraint, and an empty tensor is contiguous.
        expected = sympy.Integer(1)
        axes = []
        for size, stride in reversed(list(zip(shape, strides, strict=True))):
            axes.append(sympy.Or(sympy.Eq(size, 1), sympy.Eq(stride, expected)))
            expected *= size

        empty = sympy.Or(*(sympy.Eq(size, 0) for size in shape))
        return self.expression(sympy.Or(empty, sympy.And(*axes)))

    def shape(self, tensor: torch.Tensor) -> list[Any] | None:
        if tensor in self.unavailable:
            return None
        fake = self.tensors.get(tensor)
        if fake is None:
            return None

        return [self.expression(size) for size in fake.shape]

    def strides(self, tensor: torch.Tensor) -> list[Any] | None:
        if tensor in self.unavailable:
            return None
        fake = self.tensors.get(tensor)
        if fake is None:
            return None

        return [self.expression(stride) for stride in fake.stride()]
