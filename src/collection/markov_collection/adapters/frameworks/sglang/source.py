"""Interpret recorded SGLang boundaries and runtime facts without running serving."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from markov_collection.adapters.runtimes.ascend.trace import AscendTrace
from markov_collection.artifacts import write_json


class SGLangSource:
    @staticmethod
    def interpret(path: Path, profile: bool) -> None:
        from markov_collection.adapters.frameworks.sglang.allocation import SGLangAllocation
        from markov_collection.adapters.frameworks.sglang.batches import BatchTensors
        from markov_collection.adapters.frameworks.sglang.expert_geometry import ModelPaddingCall
        from markov_collection.adapters.frameworks.sglang.experts import ExpertObservation
        from markov_collection.adapters.frameworks.sglang.parallel import CollectiveCall
        from markov_collection.adapters.frameworks.sglang.pd_templates import DisaggregatedTemplates
        from markov_collection.adapters.frameworks.sglang.prefix import PrefixObservation
        from markov_collection.adapters.frameworks.sglang.preparation import PrefixLocations
        from markov_collection.adapters.frameworks.sglang.state import PoolPorts
        from markov_collection.adapters.runtimes.ascend.concatenation import AscendConcatenation
        from markov_collection.adapters.runtimes.ascend.indexed_writes import AscendIndexedWrites
        from markov_collection.adapters.runtimes.ascend.layouts import AscendLayouts
        from markov_collection.adapters.runtimes.ascend.matrices import AscendMatrices
        from markov_collection.adapters.runtimes.pytorch.collectives import AllReduceGeometry
        from markov_collection.adapters.runtimes.pytorch.indexing import TensorAssignmentCall

        directory = path.parent
        source = json.loads(path.read_text())

        # Interpretation can be repeated from the captured facts without another native session.
        SGLangSource.declare_execution_domain(source, directory)
        SGLangSource.result_interfaces(source["regions"])
        ExpertObservation.declare(source)
        BatchTensors.declare_model_inputs(source["regions"], source["values"])
        PrefixLocations.declare(source)
        PrefixObservation.declare(source["regions"], source["values"])
        AscendConcatenation.declare(source["regions"], source["values"])
        AscendMatrices.declare(
            source["regions"], source["operations"], source["values"], source["environment"]["matrix_selection"]
        )
        CollectiveCall.bind_transports(source["regions"], source["operations"], source["values"])
        SGLangAllocation.declare(source["regions"], source["operations"], source["values"])
        geometries = TensorAssignmentCall.input_geometries(source["regions"])
        ModelPaddingCall.declare(source["regions"], source["values"], geometries)
        TensorAssignmentCall.declare(source["regions"], source["values"], geometries)
        AscendIndexedWrites.declare(source["regions"], source["values"], geometries)
        AscendLayouts.declare(source["regions"], source["operations"], source["values"])
        AllReduceGeometry.declare(source["operations"], source["values"])
        DisaggregatedTemplates.declare(source)
        PoolPorts.declare(source["regions"], source["values"])
        if source["environment"]["logical_pp"] > 1:
            from markov_collection.adapters.frameworks.sglang.pipeline import PipelineObservation

            PipelineObservation.declare_waits(source["regions"])
        write_json(path, source)
        if not profile:
            return

        grouped = source["environment"]["source_transport"] != "physical_group_of_one"
        name = f"physical-{path.name}" if grouped else "physical.json"

        # Physical mapping belongs to immutable trace identities; declaration fixes can reuse the completed export.
        if (directory / name).exists():
            return
        traces = directory / "native" / path.stem if grouped else directory / "native"
        trace = AscendTrace.exported_trace(traces)
        observer = AscendTrace(trace, directory / "runtime", device=source["environment"]["device_index"])
        physical = observer.physical_work(len(source["regions"]), len(source["operations"]))
        write_json(directory / name, physical)

    @staticmethod
    def declare_execution_domain(source: dict[str, Any], directory: Path) -> None:
        environment = source["environment"]
        implementation = environment["implementation"]
        session = json.loads((directory / "session.json").read_text())
        origin = Path(session["output"])
        if implementation["disaggregation_mode"] in {"prefill", "decode"}:
            origin = origin.parent

        # One native session supplies the implementation; logical coordinates preserve worker duties.
        source["execution_domain"] = {
            "session": str(origin),
            "runtime": {name: environment[name] for name in ("framework", "torch", "torch_npu", "cann", "device")},
            "parallel": {
                name: environment[name]
                for name in ("logical_tp", "logical_pp", "logical_ep", "tp_rank", "pp_rank", "ep_rank")
            },
            "implementation": {
                name: value
                for name, value in implementation.items()
                if name not in {"disaggregation_mode", "chunked_prefill_size"}
            },
        }

    @staticmethod
    def result_interfaces(regions: list[dict[str, Any]]) -> None:
        for region in regions:
            metadata = region["metadata"]

            # The target controller supplies members; the source supplies this caller's own CPU service.
            if region["role"] == "ResultProcessed":
                metadata["dynamic_calls"] = True
                metadata["parameters"] = {"result_members": len(metadata["effects"])}
                continue
            if region["role"] == "RequestOutput":
                metadata["entrypoint"] = "request_output"
                metadata["parameters"] = {"published_requests": len(metadata["requests"])}
                continue
            if region["role"] != "HostRead" or metadata["path"] != "tolist":
                continue

            # Only the read of this result's sampled tokens is a result-stage entrypoint.
            parent = region["parent"]
            while parent is not None:
                caller = regions[parent]
                if caller["role"] == "ResultProcessed":
                    if metadata["source"] == caller["metadata"]["ports"]["inputs"]["sampled_tokens"]:
                        metadata["entrypoint"] = "result_tokens"
                        metadata["ports"] = {"inputs": {"tensor": metadata["source"]}, "outputs": {}}
                    break
                parent = caller["parent"]
