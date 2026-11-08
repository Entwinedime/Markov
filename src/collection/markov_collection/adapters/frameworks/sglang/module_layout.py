"""Read native layer communication choices for shape rules and representative roles."""

from functools import partial
from inspect import signature


class LayerCommunication:
    def __init__(self, native) -> None:
        self.native = native

    @classmethod
    def read(cls, module):
        from sglang.srt.layers.communicator import LayerCommunicator

        native = getattr(module, "layer_communicator", None)
        return cls(native) if isinstance(native, LayerCommunicator) else None

    def rows(self) -> dict:
        native = self.native
        context = native._context
        modes = native.layer_scatter_modes

        # Native group widths define token ownership; equal observed dimensions alone do not prove it.
        return {
            direction: ["floor_div", ["mul", "model_tokens", context.process_group_sizes[mode]], context.attn_tp_size]
            for direction, mode in (("input", modes.layer_input_mode), ("output", modes.layer_output_mode))
        }

    def document(self) -> dict:
        native = self.native
        context = native._context
        functions = {}
        for name in (
            "_communicate_simple_fn", "_communicate_with_all_reduce_and_layer_norm_fn",
            "_communicate_summable_tensor_pair_fn", "qkv_latent_func",
        ):
            function = getattr(native, name)
            modes = {}
            if isinstance(function, partial):

                # Native communication factories bind scatter modes that change residual ownership.
                bound = signature(function.func).bind_partial(*function.args, **function.keywords)
                modes = {parameter: mode.name for parameter, mode in bound.arguments.items()}
                function = function.func
            functions[name] = None if function is None else {
                "implementation": f"{function.__module__}.{function.__qualname__}", "bound_modes": modes,
            }

        # Compare initialized policies, not rank placement or the communicator's mutable cache.
        return {
            "implementation": f"{type(native).__module__}.{type(native).__qualname__}",
            "modes": {name: mode.name for name, mode in vars(native.layer_scatter_modes).items()},
            "group_sizes": {mode.name: size for mode, size in context.process_group_sizes.items()},
            "sizes": {name: getattr(context, name) for name in ("attn_tp_size", "attn_dp_size", "attn_cp_size", "tp_size")},
            "flags": {name: getattr(native, name) for name in (
                "allow_reduce_scatter", "is_last_layer", "force_layernorm_before_dp_gather",
            )},
            "functions": functions,
        }
