"""Observe Ascend attention inputs and the call boundaries needed for DAG replacement."""

from functools import lru_cache, partial
from itertools import count
import os
import sys
from types import ModuleType
from typing import Any, Callable
from weakref import WeakKeyDictionary

from trace_sim_probe.patching import install_wrapper
from trace_sim_probe.schema import HICACHE_CONSUMER_DAG_PATCH
from trace_sim_probe.probes.thread_timing import invoke_runtime_call


TARGET_MODULES = ("sglang.srt.hardware_backend.npu.attention.ascend_backend",)
_MARKER = "__trace_sim_attention_backend__"


def _fia_torch_ops_dispatch() -> str | None:
    """Read the installed callable, not a mode flag that may already be stale."""

    torch_module = sys.modules.get("torch")
    if torch_module is None:
        return None

    operation = torch_module.ops.npu.npu_fused_infer_attention_score
    if (
        getattr(operation, "__module__", None)
        == "sglang.srt.hardware_backend.npu.batch_invariant_ops.npu_batch_invariant_ops"
        and getattr(operation, "__name__", None) == "npu_fused_infer_attention_score_batch_invariant"
    ):
        return "batch_invariant"

    if type(operation).__module__ == "torch._ops" and type(operation).__name__ == "OpOverloadPacket":
        return "native"

    return None


def _attention_call(original: Callable[..., Any], phase: str) -> Callable[..., Any]:
    recorded: WeakKeyDictionary[Any, dict[tuple[Any, ...], int]] = WeakKeyDictionary()
    identities = count()
    consumers = os.environ.get("TRACE_SIM_PYTHON_PROBE_CONSUMERS", "").split(",")
    every_call = (
        HICACHE_CONSUMER_DAG_PATCH in consumers
        or os.environ.get("TRACE_SIM_PYTHON_PROBE_DIAGNOSTICS", "off") == "timing"
    )

    def observed(
        backend: Any, q: Any, k: Any, v: Any, layer: Any, forward_batch: Any, *args: Any, **kwargs: Any
    ) -> Any:
        # These are Python scalars and tensor metadata already used by the
        # backend. No .item(), .cpu(), .tolist(), cache snapshots or syncs.
        optional_inputs: dict[str, Any] = {}
        parameters = (
            ("save_kv_cache", True),
            ("q_rope", None),
            ("k_rope", None),
            ("topk_indices", None),
            ("sinks", None),
            ("slopes", None),
        )
        for index, (name, default) in enumerate(parameters):
            if name in kwargs:
                optional_inputs[name] = kwargs[name]
            elif index < len(args):
                optional_inputs[name] = args[index]
            else:
                optional_inputs[name] = default

        query_shape = tuple(q.shape)
        forward_mode = str(forward_batch.forward_mode)
        extend_query_lengths = forward_batch.extend_seq_lens_cpu
        extend_shape = None if extend_query_lengths is None else tuple(extend_query_lengths)
        prefix_lengths = forward_batch.extend_prefix_lens_cpu
        prefix_shape = None if prefix_lengths is None else tuple(prefix_lengths)
        sequence_lengths_on_cpu = forward_batch.seq_lens.device.type == "cpu"
        fia_torch_ops_dispatch = _fia_torch_ops_dispatch()
        communicator = sys.modules.get("sglang.srt.layers.communicator")
        context = None if communicator is None else communicator.get_attn_tp_context()
        padding = {
            "mlp_sync": forward_batch.global_num_tokens_cpu is not None,
            "allow_input_scattered": None if context is None else context.allow_input_scattered,
            "tp_size": None if communicator is None else communicator.get_parallel().tp_size,
            "can_run_tbo": forward_batch.can_run_tbo,
        }
        pool = backend.token_to_kv_pool
        pool_dtype = getattr(pool, "dtype", None)
        graph_decode = phase == "decode" and backend.graph_mode and not backend.enable_torch_compile
        cache_loc = (
            forward_batch.encoder_out_cache_loc
            if layer.is_cross_attention and not graph_decode
            else forward_batch.out_cache_loc
        )
        cache_write = {
            "pool_implementation": f"{type(pool).__module__}.{type(pool).__qualname__}",
            "use_fia": getattr(pool, "use_fia", None),
            "cache_dtype": None if pool_dtype is None else str(pool_dtype),
            "key_dtype": None if k is None else str(k.dtype),
            "value_dtype": None if v is None else str(v.dtype),
            "key_shape": None if k is None else tuple(k.shape),
            "key_stride": None if k is None else tuple(k.stride()),
            "value_shape": None if v is None else tuple(v.shape),
            "value_stride": None if v is None else tuple(v.stride()),
            "location_dtype": None if cache_loc is None else str(cache_loc.dtype),
            "key_elements": None if k is None else k.numel(),
            "value_elements": None if v is None else v.numel(),
            "locations": None if cache_loc is None else cache_loc.numel(),
            "layer_counter_registered": getattr(pool, "layer_transfer_counter", None) is not None,
            "layer_index": layer.layer_id - pool.start_layer if hasattr(pool, "start_layer") else None,
        }
        key = (
            layer.layer_id,
            query_shape,
            str(q.dtype),
            forward_mode,
            forward_batch.batch_size,
            backend.graph_mode,
            backend.use_fia,
            backend.use_mla,
            forward_batch.encoder_lens is not None,
            optional_inputs["save_kv_cache"],
            optional_inputs["topk_indices"] is not None,
            optional_inputs["sinks"] is not None,
            optional_inputs["slopes"] is not None,
            k is not None,
            v is not None,
            forward_batch.attn_cp_metadata is not None,
            optional_inputs["q_rope"] is not None,
            optional_inputs["k_rope"] is not None,
            forward_batch.num_token_non_padded_cpu,
            extend_shape,
            prefix_shape,
            tuple(cache_write.values()),
            sequence_lengths_on_cpu,
            tuple(padding.values()),
            fia_torch_ops_dispatch,
        )
        seen = recorded.setdefault(backend, {})
        known = key in seen
        if known and not every_call:
            return original(backend, q, k, v, layer, forward_batch, *args, **kwargs)

        descriptor_id = seen[key] if known else next(identities)
        fields = (
            {}
            if known
            else {
                "phase": phase,
                "backend_implementation": f"{type(backend).__module__}.{type(backend).__qualname__}",
                "layer_id": layer.layer_id,
                "page_size_tokens": backend.page_size,
                "batch_size": forward_batch.batch_size,
                "query_shape": list(query_shape),
                "query_dtype": str(q.dtype),
                "query_heads": layer.tp_q_head_num,
                "key_heads": layer.tp_k_head_num,
                "value_heads": layer.tp_v_head_num,
                "query_key_head_dim": layer.qk_head_dim,
                "value_head_dim": layer.v_head_dim,
                "logit_cap": layer.logit_cap,
                "cross_attention": layer.is_cross_attention,
                "attention_type": str(layer.attn_type),
                "causal_attention": not layer.is_cross_attention and layer.attn_type.value != "encoder_only",
                "sliding_window_size": layer.sliding_window_size,
                "use_fia": backend.use_fia,
                "use_mla": backend.use_mla,
                "use_alibi": getattr(backend, "use_alibi", None),
                "use_native_sdpa": getattr(backend, "use_native_sdpa", False),
                "graph_mode": backend.graph_mode,
                "enable_torch_compile": backend.enable_torch_compile,
                "dllm_model": backend.is_dllm_model,
                "encoder_present": forward_batch.encoder_lens is not None,
                "forward_mode": forward_mode,
                "mode_is_target_verify": forward_batch.forward_mode.is_target_verify(),
                "mode_is_draft_extend": forward_batch.forward_mode.is_draft_extend_v2(),
                "mode_is_context_parallel_extend": forward_batch.forward_mode.is_context_parallel_extend(),
                "non_padded_tokens": forward_batch.num_token_non_padded_cpu,
                "extend_query_lengths": extend_query_lengths,
                "extend_prefix_lengths": prefix_lengths,
                "sequence_lengths_on_cpu": sequence_lengths_on_cpu,
                "fia_torch_ops_dispatch": fia_torch_ops_dispatch,
                "cache_write": cache_write,
                "padding": padding,
                "save_kv_cache": optional_inputs["save_kv_cache"],
                "key_present": k is not None,
                "value_present": v is not None,
                "topk_present": optional_inputs["topk_indices"] is not None,
                "sinks_present": optional_inputs["sinks"] is not None,
                "slopes_present": optional_inputs["slopes"] is not None,
                "query_rope_present": optional_inputs["q_rope"] is not None,
                "key_rope_present": optional_inputs["k_rope"] is not None,
                "cp_metadata_present": forward_batch.attn_cp_metadata is not None,
                "attention_cp_size": backend.attn_cp_size,
                "hybrid_swa": backend.is_hybrid_swa,
                "swa_layer": backend._is_swa_layer(layer),
            }
        )
        fields.update(
            {
                "phase": phase,
                "descriptor_id": descriptor_id,
                "descriptor_present": not known,
                "sampling": "every_call" if every_call else "first_successful_call_per_layer_shape_path",
            }
        )

        def remember(result: Any) -> None:
            seen[key] = descriptor_id

        return invoke_runtime_call(
            original, (backend, q, k, v, layer, forward_batch, *args), kwargs, fields, "runtime.phase.attention", remember
        )

    return observed


def _metadata_call(original: Callable[..., Any], route: str, device_name: Callable[[Any], str]) -> Callable[..., Any]:
    """Separate per-forward preparation from per-layer attention and allocation."""

    def observed(backend: Any, forward_batch: Any, *args: Any, **kwargs: Any) -> Any:
        # Describe storage, not its contents. In particular, reading seq_lens
        # back would introduce the synchronization this probe must not add.
        table = backend.req_to_token_pool.req_to_token
        # Graph replay supplies a namespace of buffer views, not ForwardBatch;
        # that path neither exposes nor consumes extend_seq_lens.
        extend_seq_lens = forward_batch.extend_seq_lens if route == "eager" else None
        in_capture = False
        if route == "graph":
            if "in_capture" in kwargs:
                in_capture = kwargs["in_capture"]
            elif args:
                in_capture = args[0]

        fields = {
            "backend_implementation": f"{type(backend).__module__}.{type(backend).__qualname__}",
            "route": route,
            "device_name": device_name(table.device),
            "in_capture": in_capture,
            "max_context_tokens": backend.max_context_len,
            # A framework-maintained Python scalar, not a tensor readback.
            # With one request this is the actual current/chunk context.
            "context_tokens": forward_batch.seq_lens_sum
            if forward_batch.batch_size == 1 and isinstance(forward_batch.seq_lens_sum, int)
            else None,
            "forward_mode": str(forward_batch.forward_mode),
            "mode_is_target_verify": forward_batch.forward_mode.is_target_verify(),
            "mode_is_draft_extend": forward_batch.forward_mode.is_draft_extend_v2(),
            "batch_size": forward_batch.batch_size,
            "page_size_tokens": backend.page_size,
            "sequence_shape": list(forward_batch.seq_lens.shape),
            "sequence_dtype": str(forward_batch.seq_lens.dtype),
            "sequence_device_type": forward_batch.seq_lens.device.type,
            "request_indices_shape": list(forward_batch.req_pool_indices.shape),
            "request_indices_dtype": str(forward_batch.req_pool_indices.dtype),
            "request_indices_device_type": forward_batch.req_pool_indices.device.type,
            "request_table_shape": list(table.shape),
            "request_table_stride": list(table.stride()),
            "request_table_dtype": str(table.dtype),
            "request_table_contiguous": table.is_contiguous(),
            "extend_present": extend_seq_lens is not None,
            "speculative_present": forward_batch.spec_info is not None,
            "hybrid_swa": backend.is_hybrid_swa,
            "sliding_window_kv_pool": backend.use_sliding_window_kv_pool,
            "use_mla": backend.use_mla,
            "sampling": "every_call",
        }
        if extend_seq_lens is not None:
            fields["extend_dtype"] = str(extend_seq_lens.dtype)
            fields["extend_device_type"] = extend_seq_lens.device.type

        def describe(result: Any) -> None:
            output = backend.forward_metadata.block_tables
            fields["block_table_shape"] = list(output.shape)
            fields["block_table_stride"] = list(output.stride())
            fields["block_table_dtype"] = str(output.dtype)
            if route == "graph":
                source_sequence = forward_batch.seq_lens
                destination_sequence = backend.forward_metadata.seq_lens
                fields["sequence_copy_identity"] = (
                    source_sequence.device == destination_sequence.device
                    and source_sequence.dtype == destination_sequence.dtype
                    and source_sequence.shape == destination_sequence.shape
                    and source_sequence.stride() == destination_sequence.stride()
                    and source_sequence.data_ptr() == destination_sequence.data_ptr()
                )

        return invoke_runtime_call(
            original, (backend, forward_batch, *args), kwargs, fields, "runtime.phase.attention_metadata", describe
        )

    return observed


def install(module: ModuleType) -> None:
    """Wrap loaded backend methods; do not import SGLang or initialize devices."""

    backend = getattr(module, "AscendAttnBackend", None)
    device_name = lru_cache(maxsize=None)(module.torch.npu.get_device_name)
    for method, route in (("init_forward_metadata", "eager"), ("init_forward_metadata_out_graph", "graph")):
        install_wrapper(backend, method, partial(_metadata_call, route=route, device_name=device_name), marker=_MARKER)
    for method, phase in (("forward_extend", "prefill"), ("forward_decode", "decode")):
        install_wrapper(backend, method, partial(_attention_call, phase=phase), marker=_MARKER)
