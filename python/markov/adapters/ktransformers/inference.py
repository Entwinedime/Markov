"""Run dense safetensors through native KTransformers Torch linear operators."""

from __future__ import annotations

import importlib
import time
from typing import TYPE_CHECKING

from markov.instrumentation.events import EventWriter

if TYPE_CHECKING:
    from torch import Tensor
    from transformers.cache_utils import Cache


class Inference:
    """Load the optional native SDK in its device process; each request starts with empty KV."""

    def __init__(self, settings: dict) -> None:
        import torch
        import torch_npu

        # Native KTransformers helpers still use CUDA device APIs on Ascend.
        importlib.import_module("torch_npu.contrib.transfer_to_npu")
        from transformers import AutoModelForCausalLM
        from ktransformers.operators.linear import KTransformersLinear
        from ktransformers.util.custom_loader import SafeTensorLoader
        from ktransformers.util.utils import set_module

        self.torch = torch
        self.settings = settings
        torch_npu.npu.set_device(0)
        torch_npu.npu.set_compile_mode(jit_compile=False)
        torch.set_num_threads(settings["cpu_threads"])
        dtype = getattr(torch, settings["dtype"])
        torch.set_default_dtype(dtype)
        self.model = AutoModelForCausalLM.from_pretrained(
            settings["model_path"],
            torch_dtype=dtype,
            attn_implementation=settings["attention_backend"],
            device_map={"": "npu:0"},
            local_files_only=True,
        )
        loader = SafeTensorLoader(settings["model_path"])
        linear_modules = [
            (name, module) for name, module in self.model.named_modules() if isinstance(module, torch.nn.Linear)
        ]
        for name, module in linear_modules:
            replacement = KTransformersLinear(
                key=name,
                gguf_loader=loader,
                config=self.model.config,
                orig_module=module,
                generate_device="npu:0",
                prefill_device="npu:0",
                generate_op="KLinearTorch",
                prefill_op="KLinearTorch",
            )

            # Use the native explicit-weight API. Its GGUF-oriented NPU loader
            # cannot directly resolve these HF safetensor parameter names.
            weights = (module.weight, module.bias) if module.bias is not None else module.weight
            replacement.load(w=weights)
            set_module(self.model, name, replacement)
        self.model.eval()
        self.loader = loader
        self.native_linears = len(linear_modules)
        self.writer: EventWriter | None = None
        self.request_id = ""
        self.first_phases: set[str] = set()

    def step(self, tokens: Tensor, cache: Cache, phase: str, emit: bool) -> int | None:
        torch = self.torch
        if self.writer is not None:
            query = tokens.shape[1]
            context = cache.get_seq_length() + query
            fields = {
                "request_ids": [self.request_id],
                "phase": phase,
                "query_tokens": query,
                "context_tokens": context,
                "first_phase": phase not in self.first_phases,
                "emit_token": emit,
                "attention_backend": self.settings["attention_backend"],
            }
            self.first_phases.add(phase)
            begin_ns = time.time_ns()
            cpu_begin_ns = time.thread_time_ns()

        output = self.model(input_ids=tokens, past_key_values=cache, use_cache=True, logits_to_keep=1)
        token = int(output.logits[:, -1, :].argmax(dim=-1).item()) if emit else None

        # This entry deliberately exposes synchronous eager phases, including
        # completion observation, rather than promising native serving overlap.
        torch.npu.synchronize()
        if self.writer is not None:
            end_ns = time.time_ns()
            fields["thread_cpu_ns"] = time.thread_time_ns() - cpu_begin_ns
            self.writer.write("runtime.ktransformers.phase", begin_ns, end_ns, fields)
        return token

    def generate(self, identity: str, input_ids: list[int], output_tokens: int, ignore_eos: bool) -> list[int]:
        from transformers import DynamicCache

        if len(input_ids) + output_tokens > self.settings["max_context_tokens"]:
            raise ValueError("The request exceeds max_context_tokens")
        self.request_id = identity
        torch = self.torch
        cache = DynamicCache(config=self.model.config)
        tokens = torch.tensor([input_ids], dtype=torch.int64, device="npu:0")
        chunk = self.settings["prefill_chunk_tokens"] or len(input_ids)
        result = []
        with torch.inference_mode():
            for begin in range(0, len(input_ids), chunk):
                end = min(len(input_ids), begin + chunk)
                token = self.step(tokens[:, begin:end], cache, "prefill", end == len(input_ids))
            result.append(token)
            eos = self.model.generation_config.eos_token_id
            endings = set(eos if isinstance(eos, list) else [eos])
            while len(result) < output_tokens and (ignore_eos or result[-1] not in endings):
                tokens = torch.tensor([[result[-1]]], dtype=torch.int64, device="npu:0")
                result.append(self.step(tokens, cache, "decode", True))
        return result
