"""Lower native resource and shape declarations for projection and source checking."""

from markov_adapters.sglang.batches import BatchTensors
from markov_adapters.sglang.resources import ResourceInputs


class SGLangSourceScopes:
    relay_ports = {"run_batch": "output", "output_prepare": "input"}
    qwen_layer = "sglang.srt.models.qwen3.Qwen3DecoderLayer"
    rotary_layers = {qwen_layer, "sglang.srt.models.llama.LlamaDecoderLayer"}
    model_calls = {"launch", "run_batch", "model"}

    def __init__(self, environment):
        self.environment = environment
        self.first_layer = str(environment["parallel"]["layer_range"][0])

    def operands(self, scope):
        direction = self.relay_ports.get(scope["role"])
        if direction and any(access["field"] == "output_token" and access["access"] == "write"
                             for access in scope.get("state_accesses", ())):

            # FutureMap writes these values through Python indexed assignment, which strips leading unit axes.
            return "next_token_ids", direction
        if (scope["implementation"] == "sglang.srt.mem_cache.common.alloc_for_decode"
                and scope["role"] == "segment" and scope["work"]["continuation"]["exit"] == "call.return"):
            return "cache_locations", "output"
        if scope["role"] != "layer" or not self.environment["device"].startswith("Ascend"):
            return None

        implementation = scope["implementation"]
        phase = scope["work"]["phase"]
        if implementation == self.qwen_layer and phase in {"prefill", "mixed"}:

            # Flattening each packed Q/K view for normalization copies when it crosses token rows.
            return "argument.positions", "input"
        if (implementation in self.rotary_layers
                and scope["site"].rsplit(".", 1)[-1] == self.first_layer and phase == "decode"):

            # The first local decoder makes the strided RoPE rows contiguous only when there are multiple positions.
            return "argument.positions", "input"
        return None

    def lower(self, scopes):
        backend = self.environment["implementation"]["attention_backend"]
        result = []
        for scope in ResourceInputs.scopes(scopes, self.environment):
            if scope["role"] in self.model_calls:
                layout = BatchTensors.attention_layout(backend, scope["parameters"], scope["work"]["members"])
                if layout:
                    scope = {**scope, "parameters": scope["parameters"] | layout}
                    if scopes[scope["parent"]]["role"] in self.model_calls:
                        scope["arguments"] = scope["arguments"] | {name: name for name in layout}
                    if scope["role"] == "model":
                        guards = [["eq", name, value] for name, value in layout.items()]
                        scope["guards"] = [*scope.get("guards", ()), *guards]

            operand = self.operands(scope)
            if operand is not None:
                tensor, = (port["tensor"] for port in scope["ports"] if (port["name"], port["direction"]) == operand)
                size, = tensor["shape"]
                extent, = tensor["shape_rule"]
                guard = ["eq", extent, 1] if size == 1 else ["lt", 1, extent]
                scope = {**scope, "guards": [*scope.get("guards", ()), guard]}
            result.append(scope)
        return result
