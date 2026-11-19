"""Lower native resource and shape declarations for projection and source checking."""

from markov_adapters.sglang.resources import ResourceInputs


class SGLangSourceScopes:
    relay_ports = {"run_batch": "output", "output_prepare": "input"}

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
        if (scope["implementation"] == "sglang.srt.models.llama.LlamaDecoderLayer"
                and scope["role"] == "layer" and scope["site"].rsplit(".", 1)[-1] == self.first_layer
                and scope["work"]["phase"] == "decode"):

            # The first local decoder makes the strided RoPE rows contiguous only when there are multiple positions.
            return "argument.positions", "input"
        return None

    def lower(self, scopes):
        result = []
        for scope in ResourceInputs.scopes(scopes, self.environment):
            operand = self.operands(scope)
            if operand is not None:
                tensor, = (port["tensor"] for port in scope["ports"] if (port["name"], port["direction"]) == operand)
                size, = tensor["shape"]
                extent, = tensor["shape_rule"]
                guard = ["eq", extent, 1] if size == 1 else ["lt", 1, extent]
                scope = {**scope, "guards": [*scope.get("guards", ()), guard]}
            result.append(scope)
        return result
