"""Observe native device collectives, keeping their participant contract outside graph algorithms."""

from __future__ import annotations

import inspect
from contextlib import contextmanager
from contextvars import ContextVar
from functools import partial
from typing import Any

from markov_adapters.sglang.pipeline import PipelineABI

from markov_collection.adapters.frameworks.sglang.module_scopes import tensor_ports
from markov_collection.adapters.runtimes.pytorch.layouts import GatherLayout


class NativeCommunications:
    def __init__(self, observation: Any) -> None:
        self.observation = observation
        self.enclosing = ContextVar("native_communication", default=False)
        self.orders = {}
        self.action = None

    def invoke(self, signature, original, group, *args, **kwargs):
        owner = self.observation
        if not owner.active or not owner.selected or group.world_size == 1 or self.enclosing.get():
            return original(group, *args, **kwargs)
        arguments = signature.bind(group, *args, **kwargs)
        arguments.apply_defaults()
        tensors = list(tensor_ports(arguments.arguments, "arguments"))
        if not any(tensor.device.type == "npu" for _, tensor in tensors):
            return original(group, *args, **kwargs)

        root_rank = group.ranks[arguments.arguments["src"]] if original.__name__ == "broadcast" else None
        gather = original.__name__ == "all_gather" and arguments.arguments["output_tensor_list"] is None
        standalone = gather and owner.capture.current.get() == 0
        parameters, relations, shape = None, None, None
        if gather:
            tensor = arguments.arguments["input_"]
            with owner.tensors.metadata():
                parameters = PipelineABI.tensor_parameters(tensor)
                shape = list(parameters)
                parent_shape = shape if standalone else owner.tensors.shape_rule(tensor)
                relations = {} if standalone else dict(zip(shape, parent_shape, strict=True))
                dim = arguments.arguments["dim"] % tensor.ndim

        suffix, use = ("", "device") if standalone else (".layout", "metadata")
        with self.exchange(
            original, group.device_group, root_rank=root_rank,
            role="gather" if standalone else "communication", parameters=parameters, arguments=relations,
        ) as call:
            if gather:
                with owner.tensors.metadata():
                    communicator = group.npu_communicator
                    if communicator is not None and not communicator.disabled and tensor.is_contiguous():

                        # This ABI reads geometry and the process group; PD queues and remote KV pools are outside the call.
                        call.record["bindings"]["local_implementation"] = {
                            "backend": f"{type(communicator).__module__}.{type(communicator).__qualname__}",
                            "input_layout": "contiguous", "dimension": dim,
                        }
                    call.record["guards"] = [GatherLayout.guard(tuple(tensor.shape), shape, dim, group.world_size)]
                    call.port(f"input{suffix}", "input", {**owner.tensors.observe(tensor, None), "shape_rule": shape}, use=use)
                    if standalone:
                        call.record["bindings"]["collective"] = PipelineABI.gather_condition(tensor, dim, call.record["bindings"]["collective_group"])
            result = original(group, *args, **kwargs)
            if shape is not None:
                output_shape = list(shape)
                output_shape[dim] = ["mul", group.world_size, shape[dim]]
                call.port(f"output{suffix}", "output", {**owner.tensors.observe(result, None), "shape_rule": output_shape}, use=use)

                # Local axis names belong to this call; later callers see the producer's enclosing expressions.
                with owner.tensors.metadata():
                    parent_output = list(parent_shape)
                    parent_output[dim] = ["mul", group.world_size, parent_shape[dim]]
                    owner.tensors.declare_shape(result, parent_output, scope=call.identity if standalone else call.record["parent"])
            return result

    def distributed(self, signature, original, *args, **kwargs):
        owner = self.observation
        if not owner.active or not owner.selected or self.enclosing.get():
            return original(*args, **kwargs)
        arguments = signature.bind(*args, **kwargs)
        arguments.apply_defaults()
        tensors = tensor_ports(arguments.arguments, "arguments")
        if not any(tensor.device.type == "npu" for _, tensor in tensors):
            return original(*args, **kwargs)
        if arguments.arguments["async_op"]:
            raise ValueError("asynchronous native collectives need an explicit work-completion boundary")

        import torch.distributed as distributed

        group = arguments.arguments["group"]
        if group is None:
            group = distributed.group.WORLD
        if distributed.get_world_size(group) == 1:
            return original(*args, **kwargs)
        root_rank = arguments.arguments.get("src")
        if arguments.arguments.get("group_src") is not None:
            root_rank = distributed.get_global_rank(group, arguments.arguments["group_src"])
        with self.exchange(original, group, root_rank=root_rank):
            return original(*args, **kwargs)

    @contextmanager
    def exchange(self, original, group, *, root_rank=None, role="communication", parameters=None, arguments=None):
        import torch.distributed as distributed

        owner = self.observation
        root = owner.capture.current.get()
        while owner.capture.scopes[root]["parent"] not in (None, 0):
            root = owner.capture.scopes[root]["parent"]
        if root != self.action:
            self.action = root
            self.orders.clear()
        offset = owner.description["rank_offset"]
        group_role = owner.groups.name(group)
        descriptor = owner.worker_roles[str(owner.parallel["rank"])]["groups"][group_role]
        members, channel = descriptor["members"], descriptor["channel"]
        order = self.orders.get(channel, 0)
        self.orders[channel] = order + 1

        # Public group methods may call each other; one native operation has one semantic completion contract.
        token = self.enclosing.set(True)
        try:
            work = owner.pipeline.messages.work() if role == "gather" and owner.description["pp_size"] > 1 else None
            with owner.call(role, original, parameters=parameters, arguments=arguments, work=work, device=True) as call:
                call.record["bindings"]["collective_group"] = group_role
                call.record["communication"] = {
                    "channel": channel, "operation": original.__name__, "members": members,
                    "required": members if root_rank is None else [offset + root_rank],
                    "rank": offset + distributed.get_rank(), "order": order,
                }
                yield call
        finally:
            self.enclosing.reset(token)

    def experts(self, original, dispatcher, *args, **kwargs):
        owner = self.observation
        if not owner.active or not owner.selected:
            return original(dispatcher, *args, **kwargs)

        from sglang.srt.distributed.parallel_state import get_tp_group

        # SGLang supplies this process group to DeepEP; dispatch/combine include their local preparation and completion waits.
        group = get_tp_group()
        implementation = dispatcher._get_impl()
        if implementation.group is not group.device_group:
            raise ValueError("DeepEP dispatcher uses a different native communication group")
        bound = inspect.signature(original).bind(dispatcher, *args, **kwargs)
        dispatch = original.__name__ == "dispatch"
        tensor = bound.arguments["hidden_states"] if dispatch else bound.arguments["combine_input"].hidden_states
        with self.exchange(original, implementation.group) as call:
            rows = "expert_input_tokens" if dispatch else "expert_received_tokens"
            owner.experts.port(call, "hidden_states", "input", tensor, rows)
            result = original(dispatcher, *args, **kwargs)
            rows = "expert_received_tokens" if dispatch else "expert_input_tokens"
            owner.experts.port(call, "hidden_states", "output", result.hidden_states if dispatch else result, rows)
            if dispatch and result.hidden_states_scale is not None:
                owner.experts.port(call, "scales", "output", result.hidden_states_scale, rows)
            return result

    def install(self) -> None:
        import torch.distributed as distributed
        from sglang.srt.distributed.parallel_state import GroupCoordinator
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        for name in ("all_reduce", "quant_all_reduce", "all_gather", "all_gather_into_tensor", "reduce_scatter", "reduce_scatter_tensor", "all_to_all_single", "broadcast"):
            target = f"sglang.srt.distributed.parallel_state.GroupCoordinator.{name}"
            HookRegistry.register(target, partial(self.invoke, inspect.signature(getattr(GroupCoordinator, name))), HookType.AROUND)

        # Device collectives can use ProcessGroup directly; nested framework and DeepEP calls remain one exchange.
        for name in ("all_gather_into_tensor", "all_reduce", "broadcast"):
            target = f"torch.distributed.{name}"
            HookRegistry.register(target, partial(self.distributed, inspect.signature(getattr(distributed, name))), HookType.AROUND)
        for name in ("dispatch", "combine"):
            target = f"sglang.srt.layers.moe.token_dispatcher.deepep.DeepEPDispatcher.{name}"
            HookRegistry.register(target, self.experts, HookType.AROUND)
