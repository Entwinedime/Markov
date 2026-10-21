"""Execute one native tensor-parallel shard inside a single-worker engine."""

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path
from typing import Any

from markov_collection.artifacts.json_file import write_json


class SGLangShardExecution:
    def __init__(self, description: dict[str, Any], markers: Any, diagnostics_directory: Path | None) -> None:
        self.description = description
        self.diagnostics_directory = diagnostics_directory
        self.markers = markers
        self.tp_size = description["tp_size"]
        self.rank = description["rank"]
        if self.tp_size < 1 or not 0 <= self.rank < self.tp_size:
            raise ValueError("invalid logical tensor-parallel shard")

        self._depth = 0
        self._communicators: dict[Any, Any] = {}

    @staticmethod
    def engine_arguments(prepared: dict[str, Any]) -> dict[str, Any]:
        arguments = {
            "model_path": prepared["model_path"],
            "tokenizer_path": prepared["tokenizer_path"],
            "skip_tokenizer_init": True,
            **prepared["configuration"],
        }
        if "shard_execution" in prepared:
            import json

            # Only the native worker count and representative repetition change.
            # Native parallel modules still construct the original model's shard.
            shard = prepared["shard_execution"]
            arguments.update(
                tp_size=1,
                load_format="dummy",
                json_model_override_args=json.dumps({"num_hidden_layers": shard["representative_layers"]}),
            )
        return arguments

    def execute(self, original: Callable[..., Any], *args: Any, **kwargs: Any) -> Any:
        from sglang.srt.runtime_context import get_parallel

        with get_parallel().override(
            tp_size=self.tp_size,
            tp_rank=self.rank,
            attn_tp_size=self.tp_size,
            attn_tp_rank=self.rank,
            moe_tp_size=self.tp_size,
            moe_tp_rank=self.rank,
        ):
            self._depth += 1
            try:
                return original(*args, **kwargs)
            finally:
                self._depth -= 1

    def attention_size(self, original: Callable[..., int]) -> int:
        return self.tp_size if self._depth else original()

    def load_model(self, original: Callable[..., Any], runner: Any) -> Any:
        from sglang.srt.layers.linear import ColumnParallelLinear, RowParallelLinear
        from sglang.srt.layers.radix_attention import RadixAttention
        from sglang.srt.layers.vocab_parallel_embedding import VocabParallelEmbedding

        result = self.execute(original, runner)
        if self.diagnostics_directory is None:
            return result

        components = (ColumnParallelLinear, RowParallelLinear, VocabParallelEmbedding, RadixAttention)
        modules = []
        for path, module in runner.model.named_modules():
            if not isinstance(module, components):
                continue

            fields = {
                name: getattr(module, name)
                for name in (
                    "tp_size",
                    "tp_rank",
                    "input_size",
                    "output_size",
                    "head_dim",
                    "tp_q_head_num",
                    "tp_k_head_num",
                    "tp_v_head_num",
                    "num_embeddings",
                    "num_embeddings_per_partition",
                )
                if hasattr(module, name)
            }
            if hasattr(module, "weight"):
                fields["weight_shape"] = list(module.weight.shape)
                fields["weight_dtype"] = str(module.weight.dtype)
            modules.append({"path": path, "type": type(module).__name__, **fields})

        write_json(
            self.diagnostics_directory / "shard_execution" / f"rank-{self.rank}-layout.json",
            {"description": self.description, "physical_tp_size": runner.tp_size, "modules": modules},
        )
        return result

    def all_reduce(self, original: Callable[..., Any], group: Any, tensor: Any) -> Any:
        if not self._depth or self.tp_size == 1 or group.world_size != 1:
            return original(group, tensor)

        return self._communicator(group).all_reduce(tensor)

    def all_gather(self, original: Callable[..., Any], group: Any, tensor: Any, dim: int = -1) -> Any:
        if not self._depth or self.tp_size == 1 or group.world_size != 1:
            return original(group, tensor, dim)

        return self._communicator(group).all_gather(tensor, dim)

    def _communicator(self, group: Any) -> Any:
        with self.markers.observer():
            from sglang.srt.distributed.device_communicators.npu_communicator import NpuCommunicator

            physical_group = group.device_group
            communicator = self._communicators.get(physical_group)
            if communicator is None:
                communicator = NpuCommunicator(physical_group)
                if communicator.world_size != 1:
                    raise ValueError("single-card collective transport requires a physical group of one")
                # PG1 retains the local native protocol, without qualifying
                # logical-TP numerical results or communication timing. Native
                # allocation and layout conversion retain the logical output.
                communicator.world_size = self.tp_size
                self._communicators[physical_group] = communicator

        return communicator

    def all_gather_into_tensor(
        self,
        original: Callable[..., Any],
        output_tensor: Any,
        input_tensor: Any,
        group: Any = None,
        async_op: bool = False,
    ) -> Any:
        if not self._depth or self.tp_size == 1 or group not in self._communicators:
            return original(output_tensor, input_tensor, group=group, async_op=async_op)
        if async_op:
            raise ValueError("single-card native all-gather requires synchronous transport")

        with self.markers.observer():
            chunks = output_tensor.view(self.tp_size, *input_tensor.shape)
            physical_output = chunks[0]

        # Keep native enqueue, stream events and Work.wait outside observation
        # overhead. This one-rank operation is not target communication timing.
        result = original(physical_output, input_tensor, group=group, async_op=False)
        with self.markers.observer():
            # Supply valid dummy contents without replacing the communicator's
            # subsequent logical-T reshape, movedim and final layout copy.
            chunks[1:].copy_(input_tensor)
        return result

    def install(self) -> None:
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        if self.tp_size > 1:
            from markov_collection.adapters.frameworks.sglang.input_parallel import SGLangInputParallel
            from markov_collection.adapters.frameworks.sglang.output_parallel import SGLangOutputParallel

            self._input = SGLangInputParallel(self.tp_size, self.rank, self.markers)
            self._input.install()
            SGLangOutputParallel(self.tp_size, self.rank, self.markers).install()

        hooks = {
            "sglang.srt.model_executor.model_runner.ModelRunner.load_model": self.load_model,
            "sglang.srt.model_executor.model_runner.ModelRunner.init_memory_pool": self.execute,
            "sglang.srt.model_executor.model_runner.ModelRunner.init_attention_backend": self.execute,
            "sglang.srt.model_executor.model_runner.ModelRunner.forward": self.execute,
            "sglang.srt.model_executor.forward_batch_info.ForwardBatch.init_new": self.execute,
            "sglang.srt.layers.dp_attention.get_attention_tp_size": self.attention_size,
            "sglang.srt.distributed.parallel_state.GroupCoordinator.all_reduce": self.all_reduce,
            "sglang.srt.distributed.parallel_state.GroupCoordinator.all_gather": self.all_gather,
            "torch.distributed.all_gather_into_tensor": self.all_gather_into_tensor,
        }
        for target, hook in hooks.items():
            HookRegistry.register(target, hook, HookType.AROUND)

    def install_input_dispatch(self) -> None:
        if self.tp_size > 1:
            self._input.install_dispatch()
