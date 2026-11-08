"""Keep native PP dictionary transport while submitting device transfers to the core."""

import torch
from markov_adapters.sglang.pipeline import PipelineABI
from markov_adapters.sglang.groups import ParallelGroups
from markov_control.adapters.runtimes.point_to_point import CpuPointToPoint


class TransferWork:
    def __init__(self, scheduler, direction, context) -> None:
        self.scheduler = scheduler
        self.direction = direction
        self.context = context
        self.completion = scheduler.signals.publish(self, context)

    def wait(self):
        work = self.scheduler.pipeline_messages.wait_work(self.context, device=False)
        work["signals"] = {"completion": self.completion}
        self.scheduler.action(f"transfer_{self.direction}_wait", {}, work)
        return True


class PipelineTransfers(CpuPointToPoint):
    def __init__(self, scheduler, world, ranks) -> None:
        super().__init__(scheduler.peer, world)
        self.scheduler = scheduler
        self.groups = ParallelGroups()
        self.ranks = dict(zip(world, ranks, strict=True))

    def post(self, tensor, group, tag, endpoint, group_endpoint, receiving):
        if not tensor.is_meta:
            return super().post(tensor, group, tag, endpoint, group_endpoint, receiving)
        message = self.describe(tensor, group, tag, endpoint, group_endpoint, receiving)
        members = [self.ranks[message["source"]], self.ranks[message["target"]]]
        direction = "receive" if receiving else "send"
        work = self.scheduler.pipeline_messages.work()
        handle = TransferWork(self.scheduler, direction, work["context"])
        work["signals"] = {"completion": handle.completion}
        work["transfer"] = {
            "channel": f"{self.scheduler.instance_id}/{message['group']}/{members[0]}>{members[1]}/{tag}",
            "members": members,
        }
        parameters = PipelineABI.tensor_parameters(tensor)
        payload = {"payload": tensor}

        # Posting releases the native actor at CPU return; Work.wait and data readers retain device dependencies.
        self.scheduler.action(
            f"transfer_{direction}", parameters, work,
            condition={"dtype": str(tensor.dtype)},
            tensor_inputs={} if receiving else payload,
            tensor_outputs=payload if receiving else {},
            allocations=payload if receiving else {},
        )
        return handle

    def gather(self, original, group, input_, dim=-1, output_tensor_list=None):
        if not input_.is_meta or group.world_size == 1:
            return original(group, input_, dim, output_tensor_list)
        if output_tensor_list is not None:
            raise ValueError("pipeline gather requires a single returned tensor")

        condition = {"collective": PipelineABI.gather_condition(input_, dim, self.groups.name(group.device_group))}
        parameters = PipelineABI.tensor_parameters(input_)
        layouts = self.scheduler.peer.call(
            "describe", action="gather", condition={"phase": "idle", "state_accesses": [], **condition}, parameters=parameters,
        )
        layout = layouts["output"]
        result = torch.empty(layout["shape"], dtype=getattr(torch, layout["dtype"].removeprefix("torch.")), device="meta")

        # The template owns gather geometry; native recv_tensor_dict keeps its final reshape and message lifecycle.
        self.scheduler.action(
            "gather", parameters, self.scheduler.pipeline_messages.work(), condition=condition,
            tensor_inputs={"input": input_}, tensor_outputs={"output": result},
        )
        return result

    def send_dict(self, original, scheduler, tensor_dict, *args, **kwargs):
        transported = dict(tensor_dict)
        for name, tensor in tensor_dict.items():
            if not isinstance(tensor, torch.Tensor) or tensor.is_meta:
                continue

            # Logical token values stay on CPU for request bookkeeping; their PP payload uses a device view.
            view = torch.empty_like(tensor, device="meta")
            scheduler.values.alias(tensor, view)
            transported[name] = view
        return original(scheduler, transported, *args, **kwargs)

    def install(self):
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        super().install()
        self.scheduler.pipeline_messages.install()
        HookRegistry.register(
            "sglang.srt.managers.scheduler.Scheduler._pp_send_dict_to_next_stage", self.send_dict, HookType.AROUND,
        )
        HookRegistry.register("sglang.srt.distributed.parallel_state.GroupCoordinator.all_gather", self.gather, HookType.AROUND)
