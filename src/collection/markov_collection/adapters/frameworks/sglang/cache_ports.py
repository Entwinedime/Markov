"""Expose native paged KV layers as disjoint persistent regions."""

from __future__ import annotations

from typing import Any

from markov_adapters.sglang.kv_layout import MhaKvLayout


class AttentionCachePorts:
    def __init__(self, tensors: Any, module: Any, arguments: dict[str, Any]) -> None:
        from sglang.srt.model_executor.forward_context import get_attn_backend

        self.tensors = tensors
        self.layer = module.layer_id
        pool = get_attn_backend().token_to_kv_pool
        local = self.layer - pool.start_layer
        self.layout = MhaKvLayout.from_pool(pool)

        # The native layout gives each layer a disjoint slice even though all layers share an allocation.
        with tensors.metadata():
            self.buffers = {"cache.key": pool.k_buffer[local], "cache.value": pool.v_buffer[local]}
        self.token_bytes = self.layout.key_token_bytes + self.layout.value_token_bytes
        self.region = f"kv_layer/{self.layer}"
        self.writes = arguments.get("save_kv_cache", True) and arguments.get("k") is not None

    def ports(self, call: Any, direction: str) -> None:
        if direction == "output" and not self.writes:
            return
        members = call.record["work"]["members"]
        call.record.setdefault("state_accesses", []).append({
            "space": "kv", "pool": "local", "layer": self.layer, "access": "read" if direction == "input" else "write",
            "interval": "device",
            "rule": {
                "begin": 0 if direction == "input" else "context_tokens",
                "end": "context_tokens" if direction == "input" else ["add", "context_tokens", "new_tokens"],
            },
            "slice": {"token_bytes": self.token_bytes, "begin": 0, "end": self.token_bytes},
            "regions": [
                {
                    "request": member["id"],
                    "begin": 0 if direction == "input" else member["context_tokens"],
                    "end": member["context_tokens"] if direction == "input" else member["context_tokens"] + member["new_tokens"],
                }
                for member in members
            ],
        })

        # Layout ports do not imply whole-buffer hazards; readiness is selected by logical request/token regions.
        for name, tensor in self.buffers.items():
            if direction == "output":
                self.tensors.written(tensor, self.region)
            shape = [["add", "kv_pages", 1], "page_size", *tensor.shape[2:]]
            call.port(name, direction, self.tensors.observe(tensor, shape, region=self.region), use="state")
