"""Observe the actual branches around SGLang batch-input preparation."""

import os
from pathlib import Path
import time

from markov.instrumentation.events import EventWriter
from markov.adapters.sglang.preparation_probe import PreparationProbe
from markov.adapters.sglang.request_probe import RequestProbe


class InputFactsProbe:
    def __init__(self, writer: EventWriter):
        self.writer = writer

    def extend(self, original, batch):
        return self.schedule(original, batch, "prefill")

    def decode(self, original, batch):
        return self.schedule(original, batch, "decode")

    def schedule(self, original, batch, phase):
        from sglang.srt.server_args import get_global_server_args

        requests = batch.reqs
        server = get_global_server_args()
        fields = {
            "phase": phase,
            "request_ids": [request.rid for request in requests],
            "batch_size": len(requests),
            "prompt_tokens": sum(len(request.origin_input_ids) for request in requests),
            "query_tokens": sum(request.extend_input_len for request in requests)
            if phase == "prefill"
            else len(requests),
            "return_logprob": batch.return_logprob,
            "deterministic_sampling": server.enable_deterministic_inference,
            "input_embeds_present": any(request.input_embeds is not None for request in requests),
            "position_overrides_present": any(request.positional_embed_overrides is not None for request in requests),
            "multimodal_present": any(request.multimodal_inputs is not None for request in requests),
            "mamba_pool_present": any(request.mamba_pool_idx is not None for request in requests),
            "retracted_requests": sum(request.retracted_stain for request in requests),
            "cache_breakdown_pending": sum(not request._cache_breakdown_computed for request in requests),
            "encoder_decoder": batch.model_config.is_encoder_decoder,
            "mamba_extra_buffer": server.enable_mamba_extra_buffer(),
            "dllm_present": batch.dllm_config is not None,
            "speculative": not batch.spec_algorithm.is_none(),
            "custom_logit_processor": server.enable_custom_logit_processor,
            "logit_bias_present": any(request.sampling_params.logit_bias is not None for request in requests),
        }
        begin = time.time_ns()
        if phase == "prefill":
            self.writer.write(
                "runtime.phase.batch_input_start",
                begin,
                begin,
                {
                    "request_ids": fields["request_ids"],
                    "page_size_tokens": batch.tree_cache.page_size,
                    "query_tokens": [request.extend_input_len for request in requests],
                    "sequence_tokens": [request.fill_len for request in requests],
                    "prompt_token_ids": [list(request.get_fill_ids()) for request in requests],
                },
            )
        cpu = time.thread_time_ns()
        try:
            result = original(batch)
        except BaseException:
            end = time.time_ns()
            fields.update(status="raised", thread_cpu_ns=time.thread_time_ns() - cpu)
            self.writer.write("runtime.phase.batch_inputs", begin, end, fields)
            raise

        end = time.time_ns()
        fields.update(status="returned", thread_cpu_ns=time.thread_time_ns() - cpu)

        # Overlap strips the orchestrator from the later forward-only copy.
        fields["penalties_required"] = batch.sampling_info.penalizer_orchestrator.is_required
        self.writer.write("runtime.phase.batch_inputs", begin, end, fields)
        return result


def install() -> None:
    """The framework loads this plugin in each worker; ordinary inference is unchanged."""
    output = os.environ.get("MARKOV_INPUT_FACTS")
    if not output:
        return

    from sglang.srt.plugins.hook_registry import HookRegistry, HookType

    directory = Path(output)
    if not directory.is_dir():
        raise NotADirectoryError(directory)

    writer = EventWriter(directory)
    probe = InputFactsProbe(writer)
    HookRegistry.register(
        "sglang.srt.managers.schedule_batch.ScheduleBatch.prepare_for_extend", probe.extend, HookType.AROUND
    )
    HookRegistry.register(
        "sglang.srt.managers.schedule_batch.ScheduleBatch.prepare_for_decode", probe.decode, HookType.AROUND
    )
    PreparationProbe(writer).install()
    RequestProbe(writer).install()
