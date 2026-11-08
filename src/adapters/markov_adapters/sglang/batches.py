"""Read native batch identity and token ranges without tensor-operation observation."""

from __future__ import annotations

from typing import Any


class BatchTensors:
    @staticmethod
    def boundaries(batch: Any) -> dict[str, tuple[Any, list[str]]]:

        # Capture and control publish the same batch ABI; native scratch tensors stay inside the call.
        return {
            "request_slots": (batch.req_pool_indices, ["members"]),
            "sequence_lengths": (batch.seq_lens, ["members"]),
            "cache_locations": (batch.out_cache_loc, ["input_tokens"]),
        }

    @staticmethod
    def values(batch: Any) -> dict[str, Any]:
        return {name: tensor for name, (tensor, _) in BatchTensors.boundaries(batch).items()}

    @staticmethod
    def phase(batch: Any) -> str:
        if batch.forward_mode.is_mixed():
            return "mixed"
        return "prefill" if batch.forward_mode.is_extend() or batch.forward_mode.is_prebuilt() else "decode"

    @staticmethod
    def members(batch: Any) -> list[dict[str, Any]]:
        sequences = batch.seq_lens_cpu.tolist()
        if batch.forward_mode.is_extend() or batch.forward_mode.is_prebuilt():
            lengths = batch.extend_lens
        else:
            lengths = [1] * len(batch.reqs)
        decoding = {request.rid for request in batch.decoding_reqs or ()}
        return [
            {
                "id": request.rid, "new_tokens": int(tokens), "context_tokens": int(sequence - tokens),
                "slot": int(request.req_pool_idx),
                "phase": "decode" if batch.forward_mode.is_decode() or request.rid in decoding else "prefill",
            }
            for request, sequence, tokens in zip(batch.reqs, sequences, lengths, strict=True)
        ]

    @staticmethod
    def parameters(batch: Any, tokens: int) -> dict[str, int]:
        return {
            **BatchTensors.capacity(batch.token_to_kv_pool_allocator, batch.req_to_token_pool),
            "members": len(batch.reqs), "input_tokens": int(tokens),
            "model_tokens": int(tokens), "model_members": len(batch.reqs),
        }

    @staticmethod
    def capacity(pool, slots):
        mapping = slots.req_to_token
        return {
            "kv_pages": pool.size // pool.page_size, "page_size": pool.page_size,
            "slot_capacity": mapping.shape[0] - 1, "sequence_capacity": mapping.shape[1],
        }

    @staticmethod
    def model_parameters(batch: Any, tokens: int) -> dict[str, int]:
        model_tokens, model_members = tokens, len(batch.reqs)
        if batch.global_num_tokens is not None:
            from sglang.srt.runtime_context import get_parallel
            from sglang.srt.utils.common import ceil_align

            # Native eager MLP-sync pads token rows and, for Decode, the model's batch metadata as well.
            parallel = get_parallel()
            if len(batch.global_num_tokens) != 1 or parallel.attn_cp_size != 1:
                raise ValueError("model padding requires a declared DP/CP layout")
            model_tokens = ceil_align(batch.global_num_tokens[0], parallel.attn_tp_size)
            if batch.forward_mode.is_decode():
                model_members = model_tokens
        return {"model_tokens": model_tokens, "model_members": model_members}
