"""Describe native expert boundary geometry from conditional workload routes."""

from collections import Counter

from markov_workload.expert_routes import ExpertRoutes


class ExpertBatch:
    def __init__(self, routes: ExpertRoutes, size: int, rank: int, backend: str, mode: str) -> None:
        self.routes = routes
        self.size = size
        self.rank = rank
        if size == 1 and backend == "none":
            self.layout = "ascend_standard"
            self.compute_rows = "expert_input_tokens"
        elif size > 1 and (backend, mode) == ("deepep", "normal"):
            self.layout = "ascend_deepep_normal_alltoall"
            self.compute_rows = "expert_received_tokens"
        else:
            raise ValueError("unsupported native expert boundary layout")

        # Parameters address the local expert-layer sequence; workload routes retain actual model layer IDs.
        self.layer_parameters = {layer: f"expert/{index}/received" for index, layer in enumerate(routes.layers)}
        if any(layer["experts"] % size for layer in routes.layers.values()):
            raise ValueError("EP group must partition the native routed experts")

    def observation_conditions(self, requests):
        prompts = {request["id"]: request["prompt_tokens"] for request in requests}
        conditions = set()
        for local, (layer, geometry) in enumerate(sorted(self.routes.layers.items())):
            width = geometry["experts"] // self.size
            begin, end = self.rank * width, (self.rank + 1) * width
            for identity, layers in self.routes.assignments.items():
                for first, last, experts in layers[layer]:
                    receiving = any(begin <= expert < end for expert in experts)
                    if first < prompts[identity]:
                        conditions.add((local, "prefill", receiving))
                    if last > prompts[identity]:
                        conditions.add((local, "decode", receiving))

            # Padding may give an otherwise empty receiver work; it is a separate declared opportunity.
            for _, _, experts in self.routes.padding_routes.get(layer, ()):
                conditions.add((local, "padding", any(begin <= expert < end for expert in experts)))
        return sorted(conditions)

    def parameters(self, members, model_tokens: int) -> dict[str, int]:
        if self.size == 1:

            # Local dispatch keeps token rows; expert expansion happens inside the computation scope.
            return {}
        if model_tokens % self.size:
            raise ValueError("DeepEP input must be padded before splitting tokens across its group")
        result = {}
        for layer, geometry in self.routes.layers.items():
            counts = self.routes.batch_counts(members, layer, model_tokens)
            width = geometry["experts"] // self.size
            begin = self.rank * width

            # This backend duplicates a token for each selected expert, including multiple experts on one receiver.
            result[self.layer_parameters[layer]] = sum(counts[begin:begin + width])
        return result

    def combine_counts(self, members, model_tokens, layer):
        width = self.routes.layers[layer]["experts"] // self.size
        rows = model_tokens // self.size
        sent, received = [0] * self.size, [0] * self.size
        position = 0
        for count, experts in self.routes.batch_segments(members, layer, model_tokens):
            receivers = Counter(expert // width for expert in experts)
            end = position + count

            # Dispatch partitions padded token order; Combine reverses exactly those peer-wise transfers.
            for sender in range(position // rows, (end - 1) // rows + 1):
                overlap = min(end, (sender + 1) * rows) - max(position, sender * rows)
                if sender == self.rank:
                    for receiver, copies in receivers.items():
                        received[receiver] += overlap * copies
                sent[sender] += overlap * receivers[self.rank]
            position = end
        return {"send": sent, "receive": received}

    def required_peers(self, members, model_tokens):
        result = {}
        for layer, key in self.layer_parameters.items():
            counts = self.combine_counts(members, model_tokens, layer)
            result[key] = [peer for peer, (sent, received) in enumerate(zip(counts["send"], counts["receive"], strict=True))
                           if peer == self.rank or sent or received]
        return result

    def scope(self, enclosing, layer: int | None):
        parameters = {name: value for name, value in enclosing["parameters"].items() if not name.startswith("expert/")}
        arguments = {name: name for name in parameters if name not in enclosing["constants"]}
        if layer is not None:
            key = self.layer_parameters[layer]
            if key in enclosing["parameters"]:
                parameters["expert_received_tokens"] = enclosing["parameters"][key]
                arguments["expert_received_tokens"] = key
        return parameters, arguments
