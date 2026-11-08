"""Expose native selection as control continuations around independent batch actions."""

from markov_adapters.sglang.batches import BatchTensors
from markov_adapters.sglang.calls import ActionCall


class BatchPreparation(ActionCall):
    def __init__(self, calls, batch, mode) -> None:
        if mode == "decode":
            contexts = batch.seq_lens_cpu.tolist()
            lengths = [1] * len(batch.reqs)
        else:
            contexts = [len(req.prefix_indices) for req in batch.reqs]
            lengths = [req.extend_range.length for req in batch.reqs]
        members = [
            {"id": req.rid, "context_tokens": context, "new_tokens": tokens, "phase": mode}
            for req, context, tokens in zip(batch.reqs, contexts, lengths, strict=True)
        ]
        super().__init__(
            "preparation", BatchTensors.parameters(batch, sum(lengths)),
            calls.work(batch, members, phase=mode),
        )
        self.batch = batch
        if mode == "decode":

            # Keep the inputs before native code increments lengths in place.
            self.inputs = {
                name: item for name, item in BatchTensors.boundaries(batch).items()
                if name in {"request_slots", "sequence_lengths"}
            }

    def complete(self):
        self.outputs = BatchTensors.boundaries(self.batch)


class BatchTransform(ActionCall):
    def __init__(self, calls, operation, batch, other=None) -> None:
        super().__init__(
            "batch_transform",
            {
                **BatchTensors.capacity(batch.token_to_kv_pool_allocator, batch.req_to_token_pool),
                "left_members": len(batch.reqs), "right_members": 0 if other is None else len(other.reqs),
                "members": len(batch.reqs),
            },
            calls.work(batch),
        )
        self.calls = calls
        self.transformation = operation
        self.batch = batch
        self.work["context"] = {"operation": operation, "left": self.work["context"]["members"]}
        if other is not None:
            self.work["context"]["right"] = calls.progress(other.reqs)
        for side, value in (("left", batch), ("right", other)):
            if value is None or not value.reqs:
                continue
            self.inputs.update({
                f"{side}.{name}": (tensor, [f"{side}_members"])
                for name, (tensor, _) in BatchTensors.boundaries(value).items()
                if name in {"request_slots", "sequence_lengths"}
            })
            if operation == "mix":
                self.parameters[f"{side}_tokens"] = int(value.out_cache_loc.numel())
                self.inputs[f"{side}.cache_locations"] = value.out_cache_loc, [f"{side}_tokens"]
        if operation == "mix":
            self.parameters["input_tokens"] = self.parameters["left_tokens"] + self.parameters["right_tokens"]

    def complete(self):
        before, after = self.parameters["left_members"], len(self.batch.reqs)
        self.parameters["members"] = after
        outcome = self.filter_outcome(before, after) if self.transformation == "filter" else self.transformation
        self.condition = {"transformation": {"operation": self.transformation, "outcome": outcome}}

        # Replace the context: the caller's preceding fragment retains the original input snapshot.
        self.work["context"] = {**self.work["context"], "result": self.calls.progress(self.batch.reqs)}

        # Native early returns neither read batch tensors nor produce new ones, even when stale tensors remain.
        if outcome in {"empty", "unchanged"}:
            self.inputs.clear()
            return
        self.outputs = {
            name: item for name, item in BatchTensors.boundaries(self.batch).items()
            if self.transformation == "mix" or name in {"request_slots", "sequence_lengths"}
        }

    @staticmethod
    def filter_outcome(before, after):
        return "unchanged" if after == before else "empty" if after == 0 else "subset"


class BatchSelection:
    def __init__(self, calls) -> None:
        self.calls = calls

    def select(self, original, scheduler, *args, **kwargs):
        if not self.calls.enabled:
            return original(scheduler, *args, **kwargs)

        work = self.calls.scheduler_work()

        # Native admission and loops choose child actions; the envelope has no second execution graph.
        with self.calls.call("selection", original, parameters={}, work=work, host_only=True):
            return original(scheduler, *args, **kwargs)

    def prepare(self, original, batch):
        if not self.calls.enabled:
            return original(batch)
        mode = {"prepare_for_extend": "prefill", "prepare_for_decode": "decode", "prepare_for_idle": "idle"}[original.__name__]
        boundary = BatchPreparation(self.calls, batch, mode)
        with self.calls.call(
            boundary.operation, original, parameters=boundary.parameters, work=boundary.work,
            tensor_inputs=boundary.inputs,
        ) as frame:

            # Native eviction may interrupt preparation; each resource call owns its device work and waits.
            result = original(batch)
            boundary.complete()
            frame.tensor_outputs = boundary.outputs
        return result

    def transform(self, operation, original, batch, arguments, keywords, other=None):
        if not self.calls.enabled or self.calls.current.get() is None:
            return original(batch, *arguments, **keywords)
        boundary = BatchTransform(self.calls, operation, batch, other)
        with self.calls.separate(boundary.operation, boundary.work), self.calls.boundary(original, boundary):

            # mix owns its nested merge; independently submitting it would count the same work twice.
            result = original(batch, *arguments, **keywords)
            boundary.complete()
        return result

    def filter(self, original, batch, *args, **kwargs):
        return self.transform("filter", original, batch, args, kwargs)

    def merge(self, original, batch, other):
        return self.transform("merge", original, batch, (other,), {}, other)

    def mix(self, original, batch, other):
        return self.transform("mix", original, batch, (other,), {}, other)

    def synchronize(self, original, adapter, batch):
        if not self.calls.enabled or self.calls.current.get() is None:
            return original(adapter, batch)
        work = self.calls.work(batch)

        # An idle participant has an observed empty batch, not an unknown logical context.
        if batch is None:
            work["context"] = {"members": []}

        with self.calls.call("batch_sync", original, parameters={}, work=work):
            return original(adapter, batch)

    def gather(self, original, info, device, group):
        if not self.calls.enabled or self.calls.current.get() is None:
            return original(info, device, group)
        work = self.calls.current.get().work
        observing = self.calls.request_state.observe
        inputs = None
        if observing:
            inputs = {name: getattr(info, name) for name in (
                "num_tokens", "num_tokens_for_logprob", "can_cuda_graph", "is_extend_in_batch",
                "local_can_run_tbo", "local_forward_mode", "can_run_breakable_cuda_graph",
            )}

        with self.calls.call("batch_sync_exchange", original, parameters={}, work=work) as frame:
            result = original(info, device, group)
            if observing:
                from sglang.srt.distributed.parallel_state import get_tp_group

                # Native all_gather already materialized these Python results; observation performs no device read.
                frame.values["batch_sync"] = {
                    "group": self.calls.groups.name(group),
                    "layout": [info.dp_size, info.tp_size, info.cp_size],
                    "active": get_tp_group().active_ranks_cpu.tolist(),
                    "input": inputs,
                    "output": {
                        "global_num_tokens": list(info.global_num_tokens),
                        "global_num_tokens_for_logprob": list(info.global_num_tokens_for_logprob),
                        **{name: getattr(info, name) for name in (
                            "can_cuda_graph", "is_extend_in_batch", "can_run_breakable_cuda_graph",
                        )},
                    },
                }
            return result

    def install(self) -> None:
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        HookRegistry.register(
            "sglang.srt.managers.scheduler_components.dp_attn.MLPSyncBatchInfo.all_gather",
            self.gather, HookType.AROUND,
        )
        owner = "sglang.srt.managers.schedule_batch.ScheduleBatch."
        for method, hook in (
            ("prepare_for_extend", self.prepare), ("prepare_for_decode", self.prepare), ("prepare_for_idle", self.prepare),
            ("filter_batch", self.filter), ("merge_batch", self.merge), ("mix_with_running", self.mix),
        ):
            HookRegistry.register(owner + method, hook, HookType.AROUND)
        HookRegistry.register("sglang.srt.managers.scheduler.Scheduler.get_next_batch_to_run", self.select, HookType.AROUND)
        HookRegistry.register(
            "sglang.srt.managers.scheduler_components.dp_attn.SchedulerDPAttnAdapter.prepare_mlp_sync_batch",
            self.synchronize, HookType.AROUND,
        )
