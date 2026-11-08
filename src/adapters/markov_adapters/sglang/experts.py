"""Describe native expert boundary geometry from conditional workload routes."""

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

    def scope(self, enclosing, layer: int | None):
        parameters = {name: value for name, value in enclosing["parameters"].items() if not name.startswith("expert/")}
        arguments = {name: name for name in parameters if name not in enclosing["constants"]}
        if layer is not None:
            key = self.layer_parameters[layer]
            if key in enclosing["parameters"]:
                parameters["expert_received_tokens"] = enclosing["parameters"][key]
                arguments["expert_received_tokens"] = key
        return parameters, arguments
