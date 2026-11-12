"""Expose expert input, computation and output using the declared conditional route layout."""

from functools import partial, update_wrapper

from markov_collection.adapters.frameworks.sglang.module_layout import ModuleStructure


class ExpertScopes:
    def __init__(self, observation, modules) -> None:
        import os

        from sglang.srt.batch_overlap.two_batch_overlap import MaybeTboDeepEPDispatcher
        from sglang.srt.layers.moe.token_dispatcher.standard import StandardDispatcher
        from sglang.srt.layers.moe.token_dispatcher.deepep import DeepEPDispatcher

        self.observation = observation
        self.sites = []
        self.implementations = {}
        geometry = observation.routing.geometry
        if geometry.size > 1 and os.environ.get("DEEP_USE_ALLTOALL_MODE") != "1":
            raise ValueError("expert geometry requires the Ascend DeepEP normal AllToAll layout")
        dispatcher = StandardDispatcher if geometry.size == 1 else MaybeTboDeepEPDispatcher
        observation.implementation["expert_layout"] = geometry.layout
        for path, module in modules.items():
            if not isinstance(module.dispatcher, dispatcher):
                raise ValueError("native expert dispatcher differs from its declared boundary layout")
            if geometry.size > 1:

                # Native FusedMoE owns a wrapper even without overlap; communication hooks observe its single inner dispatcher.
                inner, = module.dispatcher._inners
                if not isinstance(inner, DeepEPDispatcher):
                    raise ValueError("native expert exchange differs from the declared DeepEP layout")
            self.sites.append(path)

            # Equal initialized modules may share local bodies; their concrete layer/weight bindings remain on each call.
            self.implementations[path] = {
                "layout": geometry.layout, "module": ModuleStructure.read(module, {module.layer_id: 0}, {}),
            }
            original = module.forward
            module.forward = update_wrapper(partial(self.forward, path, module, original), original)
            original = module.run_moe_core
            module.run_moe_core = update_wrapper(partial(self.compute, original), original)

    def forward(self, path, module, original, hidden_states, topk_output):
        owner = self.observation
        if not owner.active or not owner.selected:
            return original(hidden_states, topk_output)
        enclosing = owner.capture.scopes[owner.capture.current.get()]
        parameters, relations = owner.routing.geometry.scope(enclosing, module.layer_id)
        parameters["expert_input_tokens"] = parameters["model_tokens"] // module.moe_ep_size
        relations["expert_input_tokens"] = ["floor_div", "model_tokens", module.moe_ep_size]
        with owner.capture.call(
            "experts", path, f"{type(module).__module__}.{type(module).__qualname__}",
            parameters=parameters, arguments=relations, constants=enclosing["constants"], device=True,
            bindings={
                "weights": [name for name, _ in module.named_parameters(recurse=False)],
                "local_implementation": self.implementations[path],
            },
        ) as call:
            self.port(call, "hidden_states", "input", hidden_states, "expert_input_tokens")
            self.port(call, "topk_ids", "input", topk_output.topk_ids, "expert_input_tokens")
            self.port(call, "topk_weights", "input", topk_output.topk_weights, "expert_input_tokens")
            result = original(hidden_states, topk_output)
            self.port(call, "hidden_states", "output", result, "expert_input_tokens")
            return result

    def compute(self, original, dispatch_output):
        owner = self.observation
        if not owner.active or not owner.selected:
            return original(dispatch_output)
        rows = owner.routing.geometry.compute_rows
        with owner.call("expert_compute", original, device=True) as call:
            self.port(call, "hidden_states", "input", dispatch_output.hidden_states, rows)
            if dispatch_output.hidden_states_scale is not None:
                self.port(call, "scales", "input", dispatch_output.hidden_states_scale, rows)
            result = original(dispatch_output)
            self.port(call, "hidden_states", "output", result.hidden_states, rows)
            return result

    def port(self, call, name, direction, tensor, rows) -> None:
        owner = self.observation
        with owner.tensors.metadata():
            if tensor.ndim == 0 or tensor.shape[0] != call.record["parameters"][rows]:
                raise ValueError(f"native expert tensor disagrees with conditional route geometry: {name}")
            shape = [rows, *tensor.shape[1:]]
            call.port(name, direction, owner.tensors.observe(tensor, shape), use="device")
