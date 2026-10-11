"""Observe dense computation geometry without reading tensor contents."""

import os
import sys
from contextvars import ContextVar
from types import ModuleType
from typing import Any, Callable
from weakref import WeakKeyDictionary

from trace_sim_probe.patching import install_wrapper
from trace_sim_probe.probes.thread_timing import invoke_runtime_call


TARGET_MODULES = (
    "sglang.srt.layers.quantization.unquant",
    "sglang.srt.models.qwen3",
    "sglang.srt.layers.activation",
    "sglang.srt.layers.layernorm",
    "sglang.srt.layers.rotary_embedding.base",
    "sglang.srt.models.utils",
    "sglang.srt.layers.communicator",
    "sglang.srt.distributed.device_communicators.npu_communicator",
    "sglang.srt.layers.linear",
    "sglang.srt.layers.logits_processor",
    "sglang.srt.layers.vocab_parallel_embedding",
    "sglang.srt.model_executor.model_runner",
    "sglang.srt.layers.sampler",
    "sglang.srt.model_executor.runner.eager_runner",
)
_MARKER = "__trace_sim_dense_backend__"
_DENSE_ROLES: WeakKeyDictionary[Any, dict[str, Any]] = WeakKeyDictionary()
_LAYER_OWNER: ContextVar[dict[str, Any] | None] = ContextVar("dense_layer_owner", default=None)
_QK_INPUTS: ContextVar[dict[int, dict[str, Any]] | None] = ContextVar("dense_qk_inputs", default=None)
_DEVICE_GROUPS: WeakKeyDictionary[Any, dict[str, Any]] = WeakKeyDictionary()
_LOGITS_LAST_TOKEN: ContextVar[bool] = ContextVar("logits_last_token", default=False)


def _qwen3_layer_init(original: Callable[..., Any]) -> Callable[..., Any]:
    def observed(layer: Any, *args: Any, **kwargs: Any) -> None:
        original(layer, *args, **kwargs)

        # Bind actual module objects through the model's declared members,
        # not a guessed weight name or a dimension equal to a token count.
        # apply_qk_norm reshapes NPU inputs to [tokens*heads, head_dim].
        # Use declared local head counts, never infer this ratio from sizes.
        for role, operation, rows_per_token in (
            ("qkv_projection", layer.self_attn.qkv_proj, 1),
            ("attention_output", layer.self_attn.o_proj, 1),
            ("mlp_gate_up", layer.mlp.gate_up_proj, 1),
            ("mlp_down", layer.mlp.down_proj, 1),
            ("mlp_activation", layer.mlp.act_fn, 1),
            ("input_norm", layer.input_layernorm, 1),
            ("post_attention_norm", layer.post_attention_layernorm, 1),
            ("query_norm", layer.self_attn.q_norm, layer.self_attn.q_size // layer.self_attn.head_dim),
            ("key_norm", layer.self_attn.k_norm, layer.self_attn.kv_size // layer.self_attn.head_dim),
            ("qkv_norm_rope", layer.self_attn, 1),
            ("communication", layer.layer_communicator, 1),
        ):
            _DENSE_ROLES[operation] = {
                "model_layer_implementation": f"{type(layer).__module__}.{type(layer).__qualname__}",
                "model_layer_index": layer.self_attn.attn.layer_id,
                "model_start_layer_index": layer.self_attn.start_layer,
                "dense_role": role,
                "rows_per_token": rows_per_token,
            }

    return observed


def _qwen3_model_init(original: Callable[..., Any]) -> Callable[..., Any]:
    def observed(model: Any, *args: Any, **kwargs: Any) -> None:
        original(model, *args, **kwargs)
        if model.end_layer <= model.start_layer:
            return

        first = model.layers[model.start_layer]
        _DENSE_ROLES[model] = dict(_DENSE_ROLES[first.input_layernorm], dense_role="input_buffers")
        if model.pp_group.is_first_rank:
            input_owner = _DENSE_ROLES[first.input_layernorm]
            input_owner["model_input_embedding_present"] = True
            input_owner["model_input_embedding_tp_size"] = model.embed_tokens.tp_size
            _DENSE_ROLES[model.embed_tokens] = dict(input_owner, dense_role="input_embedding")
        if not model.pp_group.is_last_rank:
            return

        # Qwen3Model inherits Qwen2Model.forward: final RMSNorm consumes all
        # rows returned by the last decoder layer, before logits pruning.
        # The layer index is a token-layout anchor, not a claim that this
        # model-level operation belongs to the decoder's own call envelope.
        last = model.layers[model.end_layer - 1]
        owner = _DENSE_ROLES[last.input_layernorm]
        owner["model_output_norm_present"] = True
        _DENSE_ROLES[model.norm] = {
            **owner,
            "model_implementation": f"{type(model).__module__}.{type(model).__qualname__}",
            "dense_role": "output_norm",
        }

    return observed


def _qwen3_causal_init(original: Callable[..., Any]) -> Callable[..., Any]:
    def observed(model: Any, *args: Any, **kwargs: Any) -> None:
        original(model, *args, **kwargs)
        if model.pp_group.is_last_rank:
            output_owner = _DENSE_ROLES[model.model.norm]
            output_owner["model_logits_present"] = True
            _DENSE_ROLES[model.logits_processor] = dict(output_owner, dense_role="logits_projection")

    return observed


def _logits_inputs(original: Callable[..., Any]) -> Callable[..., Any]:
    def observed(processor: Any, hidden: Any, head: Any, metadata: Any, embedding_bias: Any = None) -> Any:
        ordinary = metadata.forward_mode.is_decode() or (
            metadata.forward_mode.is_extend() and not metadata.extend_return_logprob
        )
        token = _LOGITS_LAST_TOKEN.set(ordinary)
        owner = _DENSE_ROLES.get(processor)
        context = None if owner is None else dict(owner, callsite="_get_logits", last_token_projection=ordinary)
        owner_token = _LAYER_OWNER.set(context)
        try:
            return original(processor, hidden, head, metadata, embedding_bias)
        finally:
            _LAYER_OWNER.reset(owner_token)
            _LOGITS_LAST_TOKEN.reset(token)

    return observed


def _lm_head_call(original: Callable[..., Any], module: ModuleType) -> Callable[..., Any]:
    def observed(processor: Any, hidden: Any, head: Any, embedding_bias: Any = None) -> Any:
        # This branch is torch.matmul, not UnquantizedLinearMethod.apply.
        # Conversion, LoRA and alternate backends need their own programs.
        if (
            hidden.device.type != "npu"
            or processor.use_fp32_lm_head
            or (hasattr(head, "set_lora") and hasattr(head, "apply_lora"))
            or not hasattr(head, "weight")
            or hidden.dtype != head.weight.dtype
            or module.get_global_server_args().rl_on_policy_target is not None
        ):
            return original(processor, hidden, head, embedding_bias)

        fields = {
            "implementation": "sglang.LogitsProcessor._compute_lm_head",
            "input_shape": list(hidden.shape),
            "input_stride": list(hidden.stride()),
            "input_dtype": str(hidden.dtype),
            "weight_shape": list(head.weight.shape),
            "weight_stride": list(head.weight.stride()),
            "weight_dtype": str(head.weight.dtype),
            "bias_shape": None,
            "bias_dtype": None,
            "last_token_projection": _LOGITS_LAST_TOKEN.get(),
        }
        fields.update(_DENSE_ROLES.get(processor, {}))
        return _invoke(original, (processor, hidden, head, embedding_bias), fields, "runtime.phase.logits_projection")

    return observed


def _logits_output(original: Callable[..., Any]) -> Callable[..., Any]:
    def observed(processor: Any, logits: Any, metadata: Any) -> Any:
        if logits.device.type != "npu":
            return original(processor, logits, metadata)

        fields = {
            **_DENSE_ROLES.get(processor, {}),
            "dense_role": "logits_output",
            "implementation": "sglang.LogitsProcessor._copy_logits_to_buffer",
            "last_token_projection": _LOGITS_LAST_TOKEN.get(),
            "input_shape": list(logits.shape),
            "input_stride": list(logits.stride()),
            "input_dtype": str(logits.dtype),
            "vocab_size": processor.vocab_size,
            "output_buffer_present": metadata.next_token_logits_buffer is not None,
        }
        return _invoke(original, (processor, logits, metadata), fields, "runtime.phase.logits_output")

    return observed


def _sampling_owner(original: Callable[..., Any]) -> Callable[..., Any]:
    def observed(runner: Any, logits_output: Any, forward_batch: Any) -> Any:
        processor = getattr(runner.model, "logits_processor", None)
        owner = _DENSE_ROLES.get(processor) if processor is not None else None
        token = _LAYER_OWNER.set(owner)
        try:
            return original(runner, logits_output, forward_batch)
        finally:
            _LAYER_OWNER.reset(token)

    return observed


def _eager_inputs(original: Callable[..., Any], module: ModuleType) -> Callable[..., Any]:
    def observed(runner: Any, forward_batch: Any, pp_proxy_tensors: Any = None, **kwargs: Any) -> Any:
        model = getattr(runner.model_runner.model, "model", None)
        owner = _DENSE_ROLES.get(model) if model is not None else None
        if owner is None or forward_batch.input_ids.device.type != "npu":
            return original(runner, forward_batch, pp_proxy_tensors, **kwargs)

        slots = []
        for slot in runner._eager_registry._slots.values():
            if not slot.enabled or slot.buffer is None:
                continue

            # Describe declared buffer roles, not cache contents. Custom source
            # and slicing callbacks are marked, never invoked by the probe.
            source = getattr(forward_batch, slot.name, None) if slot.source_fn is None else None
            descriptor = {
                "name": slot.name,
                "axis": slot.axis,
                "copy_from_batch": slot.copy_from_fb,
                "custom_source": slot.source_fn is not None,
                "custom_slice": slot.slice_fn is not None,
                "post_fill": slot.post_fill is not None,
                "buffer_shape": list(slot.buffer.shape),
                "buffer_stride": list(slot.buffer.stride()),
                "buffer_dtype": str(slot.buffer.dtype),
                "buffer_device": slot.buffer.device.type,
                "source_tensor": isinstance(source, module.torch.Tensor),
            }
            if descriptor["source_tensor"]:
                descriptor.update(
                    source_shape=list(source.shape),
                    source_stride=list(source.stride()),
                    source_dtype=str(source.dtype),
                    source_device=source.device.type,
                )
            slots.append(descriptor)

        fields = {
            **owner,
            "implementation": "sglang.EagerRunner.load_batch",
            "batch_size": forward_batch.batch_size,
            "input_tokens": forward_batch.input_ids.shape[0],
            "copy_enabled": not module.envs.SGLANG_EAGER_INPUT_NO_COPY.get(),
            "pipeline_inputs_present": pp_proxy_tensors is not None,
            "slots": slots,
        }

        def invoke() -> Any:
            return original(runner, forward_batch, pp_proxy_tensors, **kwargs)

        return _invoke(invoke, (), fields, "runtime.phase.input_buffers", output_names=())

    return observed


def _logits_pruning(original: Callable[..., Any]) -> Callable[..., Any]:
    def observed(
        processor: Any,
        hidden_states: Any,
        hidden_states_before_norm: Any,
        aux_hidden_states: Any,
        logits_metadata: Any,
    ) -> Any:
        args = (processor, hidden_states, hidden_states_before_norm, aux_hidden_states, logits_metadata)
        owner = _DENSE_ROLES.get(processor)
        if owner is None or hidden_states.device.type != "npu":
            return original(*args)

        mode = logits_metadata.forward_mode
        passthrough = mode.is_decode_or_idle() or mode.is_target_verify() or mode.is_draft_extend_v2()
        last_token = mode.is_extend() and not logits_metadata.extend_return_logprob
        fields = {
            **owner,
            "dense_role": "logits_pruning",
            "implementation": "sglang.LogitsProcessor._get_pruned_states",
            "input_shape": list(hidden_states.shape),
            "input_stride": list(hidden_states.stride()),
            "input_dtype": str(hidden_states.dtype),
            "passthrough": passthrough,
            "last_token_selection": last_token and not passthrough,
            "before_norm_present": hidden_states_before_norm is not None,
            "auxiliary_count": 0 if aux_hidden_states is None else len(aux_hidden_states),
        }
        if last_token and not passthrough:
            lengths = logits_metadata.extend_seq_lens
            fields.update(
                length_shape=list(lengths.shape),
                length_dtype=str(lengths.dtype),
                length_device_type=lengths.device.type,
                padded_static_len=logits_metadata.padded_static_len,
            )

        # The return tuple also contains optional tensors and host lists.
        # Only its first tensor describes the selected hidden-state rows;
        # do not read index values or treat the whole envelope as service.
        return _invoke(original, args, fields, "runtime.phase.logits_pruning", output_names=("output",))

    return observed


def _greedy_sampling(original: Callable[..., Any]) -> Callable[..., Any]:
    def observed(sampler: Any, logits_output: Any, sampling_info: Any, *args: Any, **kwargs: Any) -> Any:
        logits = logits_output.next_token_logits
        owner = _LAYER_OWNER.get()
        if owner is None or logits.device.type != "npu" or not sampling_info.is_all_greedy:
            return original(sampler, logits_output, sampling_info, *args, **kwargs)

        # Only the inner argmax is a modeled region. Custom preprocessing,
        # logprobs and optional TP synchronization remain separate work.
        fields = {
            **owner,
            "dense_role": "greedy_sampling",
            "implementation": "sglang.Sampler.greedy_argmax",
            "input_shape": list(logits.shape),
            "input_stride": list(logits.stride()),
            "input_dtype": str(logits.dtype),
        }

        def invoke() -> Any:
            return original(sampler, logits_output, sampling_info, *args, **kwargs)

        return _invoke(invoke, (), fields, "runtime.phase.greedy_sampling")

    return observed


def _invoke(
    original: Callable[..., Any],
    args: tuple[Any, ...],
    fields: dict[str, Any],
    event_name: str,
    output_names: tuple[str, ...] = ("output", "residual_output"),
    output_getter: Callable[[Any], tuple[Any, ...]] | None = None,
) -> Any:
    def describe(result: Any) -> None:
        if output_getter is not None:
            outputs = output_getter(result)
        elif isinstance(result, tuple):
            outputs = result
        else:
            outputs = (result,)

        for name, output in zip(output_names, outputs):
            fields[name + "_shape"] = list(output.shape)
            fields[name + "_stride"] = list(output.stride())
            fields[name + "_dtype"] = str(output.dtype)

    return invoke_runtime_call(original, args, {}, fields, event_name, describe)


def _embedding_forward(original: Callable[..., Any]) -> Callable[..., Any]:
    def observed(layer: Any, input_: Any) -> Any:
        owner = _DENSE_ROLES.get(layer)
        if input_.device.type != "npu" or owner is None:
            return original(layer, input_)

        fields = {
            **owner,
            "implementation": "sglang.VocabParallelEmbedding.forward",
            "input_shape": list(input_.shape),
            "tp_size": layer.tp_size,
            "quant_implementation": f"{type(layer.quant_method).__module__}.{type(layer.quant_method).__qualname__}",
        }
        token = _LAYER_OWNER.set(dict(owner, callsite=original.__name__))
        try:
            return _invoke(original, (layer, input_), fields, "runtime.phase.embedding_forward")
        finally:
            _LAYER_OWNER.reset(token)

    return observed


def _embedding_mask_call(original: Callable[..., Any]) -> Callable[..., Any]:
    def observed(
        input_: Any,
        org_vocab_start_index: int,
        org_vocab_end_index: int,
        num_org_vocab_padding: int,
        added_vocab_start_index: int,
        added_vocab_end_index: int,
    ) -> Any:
        args = (
            input_,
            org_vocab_start_index,
            org_vocab_end_index,
            num_org_vocab_padding,
            added_vocab_start_index,
            added_vocab_end_index,
        )
        if input_.device.type != "npu":
            return original(*args)

        fields = {
            "implementation": "sglang.get_masked_input_and_mask.npu_eager",
            "input_shape": list(input_.shape),
            "input_stride": list(input_.stride()),
            "input_dtype": str(input_.dtype),
        }
        owner = _LAYER_OWNER.get()
        if owner is not None:
            fields.update(owner, dense_role="input_embedding_mask")
        return _invoke(original, args, fields, "runtime.phase.embedding_mask", ("output", "mask"))

    return observed


def _embedding_call(original: Callable[..., Any]) -> Callable[..., Any]:
    def observed(method: Any, layer: Any, indices: Any) -> Any:
        if indices.device.type != "npu":
            return original(method, layer, indices)

        fields = {"implementation": "torch.nn.functional.embedding"}
        for name, tensor in (("indices", indices), ("weight", layer.weight)):
            fields[name + "_shape"] = list(tensor.shape)
            fields[name + "_stride"] = list(tensor.stride())
            fields[name + "_dtype"] = str(tensor.dtype)
            fields[name + "_device_type"] = tensor.device.type
        fields.update(_DENSE_ROLES.get(layer, {}))
        return _invoke(original, (method, layer, indices), fields, "runtime.phase.embedding")

    return observed


def _linear_call(original: Callable[..., Any]) -> Callable[..., Any]:
    def observed(method: Any, layer: Any, x: Any, bias: Any = None) -> Any:
        # The NPU unquantized implementation calls F.linear directly. CPU AMX
        # and ROCm AITER have different dispatch and are outside this descriptor.
        if x.device.type != "npu":
            return original(method, layer, x, bias)

        weight = layer.weight
        fields = {
            "implementation": "torch.nn.functional.linear",
            "layer_implementation": f"{type(layer).__module__}.{type(layer).__qualname__}",
            "input_shape": list(x.shape),
            "input_stride": list(x.stride()),
            "input_dtype": str(x.dtype),
            "weight_shape": list(weight.shape),
            "weight_stride": list(weight.stride()),
            "weight_dtype": str(weight.dtype),
            "bias_shape": None if bias is None else list(bias.shape),
            "bias_dtype": None if bias is None else str(bias.dtype),
        }
        fields.update(_DENSE_ROLES.get(layer, {}))
        return _invoke(original, (method, layer, x, bias), fields, "runtime.phase.linear")

    return observed


def _gated_activation_call(original: Callable[..., Any]) -> Callable[..., Any]:
    def observed(layer: Any, x: Any) -> Any:
        # Only the actual fused NPU entry is wrapped. The native SiLU/multiply
        # route selected for on-policy execution is a different tensor program.
        fields = {
            "implementation": "torch_npu.npu_swiglu",
            "input_shape": list(x.shape),
            "input_stride": list(x.stride()),
            "input_dtype": str(x.dtype),
        }
        fields.update(_DENSE_ROLES.get(layer, {}))
        return _invoke(original, (layer, x), fields, "runtime.phase.gated_activation")

    return observed


def _normalization_call(original: Callable[..., Any]) -> Callable[..., Any]:
    def observed(layer: Any, x: Any, residual: Any = None, post_residual_addition: Any = None) -> Any:
        # Batch-invariant mode replaces this Python callable with an explicit
        # add followed by RMSNorm. Observe the installed function, not an env
        # default or the layer method's unchanged name.
        residual_backend = sys.modules["torch_npu"].npu_add_rms_norm
        split_residual = residual is not None and (
            getattr(residual_backend, "__module__", "")
            == "sglang.srt.hardware_backend.npu.batch_invariant_ops.npu_batch_invariant_ops"
            and getattr(residual_backend, "__name__", "") == "npu_add_rms_norm_batch_invariant"
        )
        fields = {
            "implementation": "torch_npu.npu_add_rms_norm" if residual is not None else "torch_npu.npu_rms_norm",
            "split_residual_norm": split_residual,
            "input_shape": list(x.shape),
            "input_stride": list(x.stride()),
            "input_dtype": str(x.dtype),
            "weight_shape": list(layer.weight.shape),
            "weight_stride": list(layer.weight.stride()),
            "weight_dtype": str(layer.weight.dtype),
            "epsilon": layer.variance_epsilon,
            "residual_shape": None if residual is None else list(residual.shape),
            "residual_stride": None if residual is None else list(residual.stride()),
            "residual_dtype": None if residual is None else str(residual.dtype),
            "post_residual_addition": post_residual_addition is not None,
        }
        fields.update(_DENSE_ROLES.get(layer, {}))
        return _invoke(original, (layer, x, residual, post_residual_addition), fields, "runtime.phase.normalization")

    return observed


def _layer_owner(original: Callable[..., Any]) -> Callable[..., Any]:
    def observed(layer: Any, *args: Any, **kwargs: Any) -> Any:
        # Outer preparation may include multiple compute/communication calls.
        # Supply identity only; never charge its whole envelope again.
        owner = _DENSE_ROLES.get(layer)
        context = None if owner is None else dict(owner, callsite=original.__name__)
        token = _LAYER_OWNER.set(context)
        try:
            return original(layer, *args, **kwargs)
        finally:
            _LAYER_OWNER.reset(token)

    return observed


def _qkv_norm_rope_call(original: Callable[..., Any]) -> Callable[..., Any]:
    def observed(
        x: Any,
        sin: Any,
        cos: Any,
        q_hidden_size: int,
        kv_hidden_size: int,
        head_dim: int,
        eps: float | None = None,
        q_weight: Any = None,
        k_weight: Any = None,
        q_bias: Any = None,
        k_bias: Any = None,
        is_neox_style: bool = True,
    ) -> Any:
        fields = {
            "implementation": "sgl_kernel_npu.split_qkv_rmsnorm_rope",
            "query_width": q_hidden_size,
            "kv_width": kv_hidden_size,
            "head_dim": head_dim,
            "epsilon": eps,
            "is_neox_style": is_neox_style,
            "triton_task_queue_enabled": os.getenv("TRITON_ENABLE_TASKQUEUE", "true").lower() in {"true", "1"},
        }
        for name, tensor in (
            ("input", x),
            ("sin", sin),
            ("cos", cos),
            ("query_weight", q_weight),
            ("key_weight", k_weight),
            ("query_bias", q_bias),
            ("key_bias", k_bias),
        ):
            fields[name + "_shape"] = None if tensor is None else list(tensor.shape)
            fields[name + "_stride"] = None if tensor is None else list(tensor.stride())
            fields[name + "_dtype"] = None if tensor is None else str(tensor.dtype)

        owner = _LAYER_OWNER.get()
        if owner is not None:
            fields.update(owner)
        args = (
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
        return _invoke(original, args, fields, "runtime.phase.qkv_norm_rope", ("query", "key", "value"))

    return observed


def _rotary_table_call(original: Callable[..., Any]) -> Callable[..., Any]:
    def observed(layer: Any, positions: Any) -> Any:
        fields = {
            "implementation": "sglang.RotaryEmbedding.get_cos_sin_with_position",
        }
        for name, tensor in (("positions", positions), ("table", layer.cos_sin_cache)):
            fields[name + "_shape"] = list(tensor.shape)
            fields[name + "_stride"] = list(tensor.stride())
            fields[name + "_dtype"] = str(tensor.dtype)
            fields[name + "_device_type"] = tensor.device.type

        owner = _LAYER_OWNER.get()
        if owner is not None:
            fields.update(owner)
            fields["dense_role"] = "rotary_table"
        # The framework returns None and stores outputs on the shared module.
        # Inspect metadata only after the timed call; return its original result.
        return _invoke(
            original,
            (layer, positions),
            fields,
            "runtime.phase.rotary_table",
            ("cos", "sin"),
            lambda result: (layer.position_cos, layer.position_sin),
        )

    return observed


def _rotary_call(original: Callable[..., Any], module: ModuleType) -> Callable[..., Any]:
    def observed(
        layer: Any,
        positions: Any,
        query: Any,
        key: Any,
        offsets: Any = None,
        fused_set_kv_buffer_arg: Any = None,
    ) -> Any:
        fields = {
            "implementation": "sglang.RotaryEmbedding.forward_npu",
            "head_dim": layer.head_size,
            "rotary_dim": layer.rotary_dim,
            "is_neox_style": layer.is_neox_style,
            "fused_rope_available": getattr(module, "fused_rope_qk_mqa", None) is not None,
            "cached_position_table": hasattr(layer, "sin_cos_cache"),
            "native_rotary_uncompiled": layer._apply_rotary_emb_wrapped is module.apply_rotary_emb,
            "offsets_present": offsets is not None,
            "fused_kv_write": fused_set_kv_buffer_arg is not None,
        }
        for name, tensor in (
            ("positions", positions),
            ("query_input", query),
            ("key_input", key),
            ("table", layer.cos_sin_cache),
        ):
            fields[name + "_shape"] = list(tensor.shape)
            fields[name + "_stride"] = list(tensor.stride())
            fields[name + "_dtype"] = str(tensor.dtype)
            fields[name + "_device_type"] = tensor.device.type

        owner = _LAYER_OWNER.get()
        if owner is not None:
            fields.update(owner)
            fields["dense_role"] = "rotary"
        return _invoke(
            original,
            (layer, positions, query, key, offsets, fused_set_kv_buffer_arg),
            fields,
            "runtime.phase.rotary",
            ("query", "key"),
        )

    return observed


def _qk_inputs(original: Callable[..., Any]) -> Callable[..., Any]:
    def observed(
        q: Any,
        k: Any,
        q_norm: Any,
        k_norm: Any,
        head_dim: int,
        alt_stream: Any = None,
        allow_inplace: bool = True,
    ) -> Any:
        owners = {}
        for tensor, norm in ((q, q_norm), (k, k_norm)):
            role = _DENSE_ROLES.get(norm)
            if role is not None:
                owners[id(tensor)] = role
        token = _QK_INPUTS.set(owners)
        try:
            return original(q, k, q_norm, k_norm, head_dim, alt_stream, allow_inplace)
        finally:
            _QK_INPUTS.reset(token)

    return observed


def _qk_reshape(original: Callable[..., Any]) -> Callable[..., Any]:
    def observed(x: Any, head_dim: int) -> Any:
        if x.device.type != "npu":
            return original(x, head_dim)

        fields = {
            "implementation": "sglang.reshape_for_qk_norm",
            "input_shape": list(x.shape),
            "input_stride": list(x.stride()),
            "input_dtype": str(x.dtype),
            "head_dim": head_dim,
        }
        owners = _QK_INPUTS.get()
        owner = None if owners is None else owners.get(id(x))
        if owner is not None:
            fields.update(owner)
            fields["dense_role"] = owner["dense_role"].replace("_norm", "_reshape")
            # This operation receives token rows, not the flattened RMS rows.
            fields["rows_per_token"] = 1

        def outputs(result: Any) -> tuple[Any, ...]:
            fields["storage_shared"] = result.data_ptr() == x.data_ptr()
            return (result,)

        return _invoke(original, (x, head_dim), fields, "runtime.phase.qk_reshape", output_getter=outputs)

    return observed


def _device_collective(original: Callable[..., Any], module: ModuleType, kind: str) -> Callable[..., Any]:
    def observed(communicator: Any, x: Any, dim: int = -1) -> Any:
        group = communicator.group
        if group not in _DEVICE_GROUPS:
            # Public roster lookup reads local process-group metadata only;
            # it does not start another collective or synchronize devices.
            _DEVICE_GROUPS[group] = {
                "group": module.dist.distributed_c10d._get_process_group_name(group),
                "group_members": module.dist.get_process_group_ranks(group),
                "global_rank": module.dist.get_rank(),
                "observed_all_reduce_index": 0,
                "observed_all_gather_index": 0,
            }
        group_record = _DEVICE_GROUPS[group]
        fields = {
            **group_record,
            "implementation": "sglang.NpuCommunicator." + kind,
            "operation": "sum" if kind == "all_reduce" else "gather",
            "input_shape": list(x.shape),
            "input_stride": list(x.stride()),
            "input_dtype": str(x.dtype),
            "input_device_type": x.device.type,
            "element_bytes": x.element_size(),
            "world_size": communicator.world_size,
            "group_rank": group.rank(),
        }
        # Counts this observed entry, including failures, not all native HCCL
        # work. Quantized or unobserved collectives are not implied covered.
        group_record["observed_" + kind + "_index"] += 1
        args = (communicator, x)
        if kind == "all_gather":
            fields["gather_dim"] = dim if dim >= 0 else dim + x.dim()
            args = (*args, dim)
        owner = _LAYER_OWNER.get()
        if owner is not None:
            fields.update(owner)
        return _invoke(original, args, fields, "runtime.phase.device_collective")

    return observed


def install(module: ModuleType) -> None:
    """Patch the loaded implementation without importing or initializing it."""
    if module.__name__ == TARGET_MODULES[0]:
        install_wrapper(getattr(module, "UnquantizedLinearMethod", None), "apply", _linear_call, marker=_MARKER)
        install_wrapper(
            getattr(module, "UnquantizedEmbeddingMethod", None), "embedding", _embedding_call, marker=_MARKER
        )
    elif module.__name__ == TARGET_MODULES[1]:
        install_wrapper(getattr(module, "Qwen3DecoderLayer", None), "__init__", _qwen3_layer_init, marker=_MARKER)
        install_wrapper(getattr(module, "Qwen3Model", None), "__init__", _qwen3_model_init, marker=_MARKER)
        install_wrapper(getattr(module, "Qwen3ForCausalLM", None), "__init__", _qwen3_causal_init, marker=_MARKER)
        install_wrapper(getattr(module, "Qwen3Attention", None), "forward_prepare_npu", _layer_owner, marker=_MARKER)
        install_wrapper(getattr(module, "Qwen3Attention", None), "forward_prepare_native", _layer_owner, marker=_MARKER)
        install_wrapper(module, "split_qkv_rmsnorm_rope", _qkv_norm_rope_call, marker=_MARKER)
        install_wrapper(module, "apply_qk_norm", _qk_inputs, marker=_MARKER)
    elif module.__name__ == TARGET_MODULES[2]:
        install_wrapper(getattr(module, "SiluAndMul", None), "forward_npu", _gated_activation_call, marker=_MARKER)
    elif module.__name__ == TARGET_MODULES[3]:
        install_wrapper(getattr(module, "RMSNorm", None), "forward_npu", _normalization_call, marker=_MARKER)
    elif module.__name__ == TARGET_MODULES[4]:
        install_wrapper(
            getattr(module, "RotaryEmbedding", None), "get_cos_sin_with_position", _rotary_table_call, marker=_MARKER
        )
        install_wrapper(
            getattr(module, "RotaryEmbedding", None),
            "forward_npu",
            lambda original: _rotary_call(original, module),
            marker=_MARKER,
        )
    elif module.__name__ == TARGET_MODULES[5]:
        install_wrapper(module, "_reshape_for_qk_norm", _qk_reshape, marker=_MARKER)
    elif module.__name__ == TARGET_MODULES[6]:
        communicator = getattr(module, "LayerCommunicator", None)
        install_wrapper(communicator, "prepare_attn", _layer_owner, marker=_MARKER)
        install_wrapper(communicator, "prepare_mlp", _layer_owner, marker=_MARKER)
        install_wrapper(communicator, "postprocess_layer", _layer_owner, marker=_MARKER)
    elif module.__name__ == TARGET_MODULES[7]:
        for kind in ("all_reduce", "all_gather"):
            install_wrapper(
                getattr(module, "NpuCommunicator", None),
                kind,
                lambda original, kind=kind: _device_collective(original, module, kind),
                marker=_MARKER,
            )
    elif module.__name__ == TARGET_MODULES[8]:
        # RowParallelLinear reduces after quant_method.apply returns. Its
        # collective is not inside the linear device-service envelope.
        install_wrapper(getattr(module, "RowParallelLinear", None), "forward", _layer_owner, marker=_MARKER)
    elif module.__name__ == TARGET_MODULES[9]:
        install_wrapper(
            getattr(module, "LogitsProcessor", None), "_copy_logits_to_buffer", _logits_output, marker=_MARKER
        )
        processor = getattr(module, "LogitsProcessor", None)
        install_wrapper(processor, "_get_pruned_states", _logits_pruning, marker=_MARKER)
        install_wrapper(processor, "_get_logits", _logits_inputs, marker=_MARKER)
        install_wrapper(processor, "_compute_lm_head", lambda original: _lm_head_call(original, module), marker=_MARKER)
    elif module.__name__ == TARGET_MODULES[10]:
        install_wrapper(getattr(module, "VocabParallelEmbedding", None), "forward", _embedding_forward, marker=_MARKER)
        install_wrapper(module, "get_masked_input_and_mask", _embedding_mask_call, marker=_MARKER)
    elif module.__name__ == TARGET_MODULES[11]:
        install_wrapper(getattr(module, "ModelRunner", None), "sample", _sampling_owner, marker=_MARKER)
    elif module.__name__ == TARGET_MODULES[12]:
        install_wrapper(getattr(module, "Sampler", None), "forward", _greedy_sampling, marker=_MARKER)
    elif module.__name__ == TARGET_MODULES[13]:
        install_wrapper(
            getattr(module, "EagerRunner", None),
            "load_batch",
            lambda original: _eager_inputs(original, module),
            marker=_MARKER,
        )
