"""Describe actual eager compute calls using host metadata, never tensor snapshots."""

import os

from markov.adapters.sglang.preparation_probe import implementation
from markov.instrumentation.events import EventWriter


def tensor_fields(name, tensor):
    return {
        name + "_shape": None if tensor is None else list(tensor.shape),
        name + "_stride": None if tensor is None else list(tensor.stride()),
        name + "_dtype": None if tensor is None else str(tensor.dtype),
        name + "_device_type": None if tensor is None else tensor.device.type,
    }


class ComputeProbe:
    def __init__(self, writer: EventWriter):
        self.writer = writer
        self.input_tokens = 0
        self.batch_size = 0
        self.last_token_projection = False
        self.embedding_active = False
        self.collective_indices = {}
        self.attention_index = 0

    def rows(self, tensor, last_token=False):
        count = self.batch_size if last_token else self.input_tokens
        return None if not count else tensor.shape[0] / count

    def input_buffers(self, original, runner, batch, pp_proxy_tensors=None, **kwargs):
        import torch
        from sglang.srt.environ import envs

        self.input_tokens = batch.input_ids.shape[0]
        self.batch_size = batch.batch_size
        self.last_token_projection = False
        slots = []
        for slot in runner._eager_registry._slots.values():
            if not slot.enabled or slot.buffer is None:
                continue
            source = getattr(batch, slot.name, None)
            fields = {
                "axis": slot.axis,
                "copy_from_batch": slot.copy_from_fb,
                "custom_source": slot.source_fn is not None,
                "custom_slice": slot.slice_fn is not None,
                "post_fill": slot.post_fill is not None,
                "source_tensor": isinstance(source, torch.Tensor),
                "buffer_device": slot.buffer.device.type,
                "buffer_dtype": str(slot.buffer.dtype),
            }
            if isinstance(source, torch.Tensor):
                fields.update(
                    source_shape=list(source.shape), source_dtype=str(source.dtype), source_device=source.device.type
                )
            slots.append(fields)
        fields = {
            "implementation": "sglang.EagerRunner.load_batch",
            "copy_enabled": not envs.SGLANG_EAGER_INPUT_NO_COPY.get(),
            "pipeline_inputs_present": pp_proxy_tensors is not None,
            "input_tokens": self.input_tokens,
            "batch_size": self.batch_size,
            "slots": slots,
        }
        span = self.writer.span("runtime.phase.input_buffers", fields)
        with span:
            self.writer.write("runtime.phase.forward_input_boundary", span.begin, span.begin, {})
            return original(runner, batch, pp_proxy_tensors, **kwargs)

    def linear(self, original, method, layer, x, bias=None):
        fields = {**tensor_fields("input", x), **tensor_fields("weight", layer.weight), **tensor_fields("bias", bias)}
        fields["rows_per_token"] = self.rows(x)
        with self.writer.span("runtime.phase.linear", fields):
            return original(method, layer, x, bias)

    def normalization(self, original, layer, x, residual=None, post_residual_addition=None):
        import torch_npu

        fields = {
            **tensor_fields("input", x),
            **tensor_fields("weight", layer.weight),
            **tensor_fields("residual", residual),
            "implementation": "torch_npu.npu_rms_norm" if residual is None else "torch_npu.npu_add_rms_norm",
            "split_residual_norm": torch_npu.npu_add_rms_norm.__name__ == "npu_add_rms_norm_batch_invariant",
            "post_residual_addition": post_residual_addition is not None,
            "epsilon": layer.variance_epsilon,
            "rows_per_token": self.rows(x),
        }
        with self.writer.span("runtime.phase.normalization", fields):
            return original(layer, x, residual, post_residual_addition)

    def activation(self, original, layer, x):
        fields = {**tensor_fields("input", x), "implementation": "torch_npu.npu_swiglu", "rows_per_token": self.rows(x)}
        with self.writer.span("runtime.phase.gated_activation", fields):
            result = original(layer, x)
            fields.update(tensor_fields("output", result))
            return result

    def embedding(self, original, method, layer, indices):
        fields = {
            **tensor_fields("indices", indices),
            **tensor_fields("weight", layer.weight),
            "implementation": "torch.nn.functional.embedding",
        }
        with self.writer.span("runtime.phase.embedding", fields):
            return original(method, layer, indices)

    def embedding_forward(self, original, layer, indices):
        fields = {
            **tensor_fields("input", indices),
            "implementation": "sglang.VocabParallelEmbedding.forward",
            "quant_implementation": implementation(layer.quant_method),
            "tp_size": layer.tp_size,
        }
        self.embedding_active = True
        try:
            with self.writer.span("runtime.phase.embedding_forward", fields):
                result = original(layer, indices)
                fields.update(tensor_fields("output", result))
                return result
        finally:
            self.embedding_active = False

    def embedding_mask(self, original, indices, *args, **kwargs):
        fields = {**tensor_fields("input", indices), "implementation": "sglang.get_masked_input_and_mask.npu_eager"}
        with self.writer.span("runtime.phase.embedding_mask", fields):
            return original(indices, *args, **kwargs)

    def rotary_table(self, original, layer, positions):
        fields = {
            **tensor_fields("positions", positions),
            **tensor_fields("table", layer.cos_sin_cache),
            "implementation": "sglang.RotaryEmbedding.get_cos_sin_with_position",
        }
        with self.writer.span("runtime.phase.rotary_table", fields):
            return original(layer, positions)

    def reshape(self, original, x, head_dim):
        fields = {
            **tensor_fields("input", x),
            "implementation": "sglang.reshape_for_qk_norm",
            "head_dim": head_dim,
            "rows_per_token": self.rows(x),
        }
        with self.writer.span("runtime.phase.qk_reshape", fields):
            result = original(x, head_dim)
            fields["storage_shared"] = x.untyped_storage().data_ptr() == result.untyped_storage().data_ptr()
            return result

    def rotary(self, original, layer, positions, query, key, offsets=None, fused_set_kv_buffer_arg=None):
        fields = {
            **tensor_fields("query_input", query),
            **tensor_fields("key_input", key),
            **tensor_fields("positions", positions),
            **tensor_fields("table", layer.cos_sin_cache),
            "implementation": "sglang.RotaryEmbedding.forward_npu",
            "offsets_present": offsets is not None,
            "fused_kv_write": fused_set_kv_buffer_arg is not None,
            "cached_position_table": hasattr(layer, "sin_cos_cache"),
            "native_rotary_uncompiled": not hasattr(layer.forward_native, "_torchdynamo_orig_callable"),
            "rotary_dim": layer.rotary_dim,
            "head_dim": layer.head_size,
            "is_neox_style": layer.is_neox_style,
        }
        with self.writer.span("runtime.phase.rotary", fields):
            return original(layer, positions, query, key, offsets, fused_set_kv_buffer_arg)

    def qkv(
        self,
        original,
        x,
        sin,
        cos,
        q_hidden_size,
        kv_hidden_size,
        head_dim,
        eps=None,
        q_weight=None,
        k_weight=None,
        q_bias=None,
        k_bias=None,
        is_neox_style=True,
    ):
        queue = os.environ.get("TRITON_ENABLE_TASKQUEUE")
        fields = {
            **tensor_fields("input", x),
            **tensor_fields("sin", sin),
            **tensor_fields("cos", cos),
            **tensor_fields("query_weight", q_weight),
            **tensor_fields("key_weight", k_weight),
            **tensor_fields("query_bias", q_bias),
            **tensor_fields("key_bias", k_bias),
            "implementation": "sgl_kernel_npu.split_qkv_rmsnorm_rope",
            "query_width": q_hidden_size,
            "kv_width": kv_hidden_size,
            "head_dim": head_dim,
            "epsilon": eps,
            "is_neox_style": is_neox_style,
            "triton_task_queue_enabled": None if queue is None else queue.lower() in ("true", "1"),
        }
        with self.writer.span("runtime.phase.qkv_norm_rope", fields):
            return original(
                x,
                sin,
                cos,
                q_hidden_size,
                kv_hidden_size,
                head_dim,
                eps,
                q_weight,
                k_weight,
                q_bias,
                k_bias,
                is_neox_style,
            )

    def pruning(self, original, processor, hidden, before_norm, auxiliary, metadata):
        mode = metadata.forward_mode
        passthrough = mode.is_decode_or_idle() or mode.is_target_verify() or mode.is_draft_extend_v2()
        last = mode.is_extend() and not metadata.extend_return_logprob
        self.last_token_projection = mode.is_decode() or last
        fields = {
            **tensor_fields("input", hidden),
            **tensor_fields("length", metadata.extend_seq_lens),
            "implementation": "sglang.LogitsProcessor._get_pruned_states",
            "passthrough": passthrough,
            "last_token_selection": last,
            "before_norm_present": before_norm is not None,
            "auxiliary_count": 0 if auxiliary is None else len(auxiliary),
            "padded_static_len": metadata.padded_static_len,
        }
        with self.writer.span("runtime.phase.logits_pruning", fields):
            return original(processor, hidden, before_norm, auxiliary, metadata)

    def projection(self, original, processor, hidden, head, embedding_bias=None):
        fields = {
            **tensor_fields("input", hidden),
            **tensor_fields("weight", head.weight),
            **tensor_fields("bias", embedding_bias),
            "last_token_projection": self.last_token_projection,
            "rows_per_token": self.rows(hidden, self.last_token_projection),
        }
        with self.writer.span("runtime.phase.logits_projection", fields):
            return original(processor, hidden, head, embedding_bias)

    def logits_output(self, original, processor, logits, metadata):
        fields = {
            **tensor_fields("input", logits),
            "implementation": "sglang.LogitsProcessor._copy_logits_to_buffer",
            "output_buffer_present": metadata.next_token_logits_buffer is not None,
            "last_token_projection": self.last_token_projection,
            "vocab_size": processor.vocab_size,
        }
        with self.writer.span("runtime.phase.logits_output", fields):
            result = original(processor, logits, metadata)
            fields.update(tensor_fields("output", result))
            return result

    def argmax(self, original, x, dim=None, keepdim=False, *, out=None):
        # shortcut: supported runs use this route only for greedy sampling;
        # scope it to Sampler before admitting other argmax execution paths.
        if x.ndim != 2 or dim not in (-1, 1) or keepdim or out is not None:
            return original(x, dim, keepdim, out=out)
        fields = {**tensor_fields("input", x), "implementation": "sglang.Sampler.greedy_argmax"}
        with self.writer.span("runtime.phase.greedy_sampling", fields):
            result = original(x, dim, keepdim)
            fields.update(tensor_fields("output", result))
            return result

    def all_reduce(self, original, communicator, x):
        return self.collective(original, communicator, x, "all_reduce")

    def all_gather(self, original, communicator, x, dim=-1):
        return self.collective(original, communicator, x, "all_gather", dim)

    def collective(self, original, communicator, x, kind, dim=None):
        import torch.distributed as dist

        group = dist.distributed_c10d._get_process_group_name(communicator.group)
        key = (group, kind)
        index = self.collective_indices.get(key, 0)
        self.collective_indices[key] = index + 1
        fields = {
            **tensor_fields("input", x),
            "implementation": "sglang.NpuCommunicator." + kind,
            "group": group,
            "group_members": dist.get_process_group_ranks(communicator.group),
            "global_rank": dist.get_rank(),
            "world_size": communicator.world_size,
            "operation": "sum",
            "gather_dim": dim,
            "element_bytes": x.element_size(),
            "observed_" + kind + "_index": index,
            "last_token_projection": self.last_token_projection and not self.embedding_active,
            "rows_per_token": self.rows(x, self.last_token_projection and not self.embedding_active),
            "dense_role": "input_embedding" if self.embedding_active else "model",
        }
        with self.writer.span("runtime.phase.device_collective", fields):
            return original(communicator, x) if dim is None else original(communicator, x, dim)

    def attention_prefill(self, original, backend, q, k, v, layer, batch, save_kv_cache=True, **kwargs):
        return self.attention(original, backend, q, k, v, layer, batch, save_kv_cache, "prefill", kwargs)

    def attention_decode(self, original, backend, q, k, v, layer, batch, save_kv_cache=True, **kwargs):
        return self.attention(original, backend, q, k, v, layer, batch, save_kv_cache, "decode", kwargs)

    def attention(self, original, backend, q, k, v, layer, batch, save_kv_cache, phase, kwargs):
        import torch
        from sglang.srt.layers.radix_attention import AttentionType

        self.attention_index += 1
        pool = backend.token_to_kv_pool
        location = batch.encoder_out_cache_loc if layer.is_cross_attention else batch.out_cache_loc
        dispatch = torch.ops.npu.npu_fused_infer_attention_score
        fields = {
            **tensor_fields("query", q),
            "descriptor_id": self.attention_index,
            "descriptor_present": True,
            "phase": phase,
            "layer_id": layer.layer_id,
            "page_size_tokens": backend.page_size,
            "extend_query_lengths": batch.extend_seq_lens_cpu,
            "query_heads": layer.tp_q_head_num,
            "key_heads": layer.tp_k_head_num,
            "value_heads": layer.tp_v_head_num,
            "query_key_head_dim": layer.qk_head_dim,
            "value_head_dim": layer.v_head_dim,
            "causal_attention": not layer.is_cross_attention and layer.attn_type != AttentionType.ENCODER_ONLY,
            "use_fia": backend.use_fia,
            "use_mla": backend.use_mla,
            "use_alibi": backend.use_alibi,
            "graph_mode": backend.graph_mode,
            "cross_attention": layer.is_cross_attention,
            "encoder_present": batch.encoder_lens is not None,
            "sinks_present": kwargs.get("sinks") is not None,
            "mode_is_target_verify": batch.forward_mode.is_target_verify(),
            "mode_is_draft_extend": batch.forward_mode.is_draft_extend_v2(),
            "sliding_window_size": layer.sliding_window_size,
            "attention_cp_size": backend.attn_cp_size,
            "batch_size": batch.batch_size,
            "fia_torch_ops_dispatch": "batch_invariant"
            if dispatch.__name__ == "npu_fused_infer_attention_score_batch_invariant"
            else dispatch.__name__,
            "save_kv_cache": save_kv_cache,
            "key_present": k is not None,
            "value_present": v is not None,
            "cache_write": {
                **tensor_fields("key", k),
                **tensor_fields("value", v),
                "pool_implementation": implementation(pool),
                "use_fia": pool.use_fia,
                "cache_dtype": str(pool.dtype),
                "location_dtype": str(location.dtype),
                "locations": location.numel(),
            },
        }
        with self.writer.span("runtime.phase.attention", fields):
            return original(backend, q, k, v, layer, batch, save_kv_cache, **kwargs)

    def install(self):
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        bindings = {
            "sglang.srt.model_executor.runner.eager_runner.EagerRunner.load_batch": self.input_buffers,
            "sglang.srt.layers.quantization.unquant.UnquantizedLinearMethod.apply": self.linear,
            "sglang.srt.layers.layernorm.RMSNorm.forward_npu": self.normalization,
            "sglang.srt.layers.activation.SiluAndMul.forward_npu": self.activation,
            "sglang.srt.layers.quantization.unquant.UnquantizedEmbeddingMethod.embedding": self.embedding,
            "sglang.srt.layers.vocab_parallel_embedding.VocabParallelEmbedding.forward": self.embedding_forward,
            "sglang.srt.layers.vocab_parallel_embedding.get_masked_input_and_mask": self.embedding_mask,
            "sglang.srt.layers.rotary_embedding.base.RotaryEmbedding.get_cos_sin_with_position": self.rotary_table,
            "sglang.srt.layers.rotary_embedding.base.RotaryEmbedding.forward_npu": self.rotary,
            "sglang.srt.models.utils._reshape_for_qk_norm": self.reshape,
            "sgl_kernel_npu.norm.split_qkv_rmsnorm_rope.split_qkv_rmsnorm_rope": self.qkv,
            "sglang.srt.layers.logits_processor.LogitsProcessor._get_pruned_states": self.pruning,
            "sglang.srt.layers.logits_processor.LogitsProcessor._compute_lm_head": self.projection,
            "sglang.srt.layers.logits_processor.LogitsProcessor._copy_logits_to_buffer": self.logits_output,
            "torch.argmax": self.argmax,
            "sglang.srt.distributed.device_communicators.npu_communicator.NpuCommunicator.all_reduce": self.all_reduce,
            "sglang.srt.distributed.device_communicators.npu_communicator.NpuCommunicator.all_gather": self.all_gather,
            "sglang.srt.hardware_backend.npu.attention.ascend_backend.AscendAttnBackend.forward_extend": self.attention_prefill,
            "sglang.srt.hardware_backend.npu.attention.ascend_backend.AscendAttnBackend.forward_decode": self.attention_decode,
        }
        for target, observer in bindings.items():
            HookRegistry.register(target, observer, HookType.AROUND)
