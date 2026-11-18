"""Collect structural timeline facts without unused hardware performance counters."""

from inspect import signature

import torch_npu


class AscendProfiling:
    @staticmethod
    def configuration(original, configuration, *args, **kwargs):
        arguments = signature(original).bind(configuration, *args, **kwargs)

        # Keep the native trace level and HCCL/API data; no structural consumer reads AI Core counters.
        arguments.arguments["aic_metrics"] = torch_npu.profiler.AiCMetrics.AiCoreNone
        return original(*arguments.args, **arguments.kwargs)

    @staticmethod
    def handler(original, directory, *args, **kwargs):

        # Native recording must close before CPU-only SDK export and graph production begin.
        kwargs["analyse_flag"] = False
        return original(directory, *args, **kwargs)

    @classmethod
    def hooks(cls):
        return {
            "torch_npu.profiler._ExperimentalConfig.__init__": cls.configuration,
            "torch_npu.profiler.tensorboard_trace_handler": cls.handler,
        }
