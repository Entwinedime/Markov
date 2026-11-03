"""Interpret recorded SGLang boundaries and runtime facts without running serving."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from markov_collection.adapters.runtimes.ascend.trace import AscendTrace
from markov_collection.artifacts import write_json


class SGLangSource:
    @staticmethod
    def interpret(path: Path, output: Path, profile: bool, *, communication: bool = False) -> None:
        from markov_collection.adapters.frameworks.sglang.allocation import SGLangAllocation
        from markov_collection.adapters.frameworks.sglang.batches import BatchTensors
        from markov_collection.adapters.frameworks.sglang.expert_geometry import ModelPaddingCall
        from markov_collection.adapters.frameworks.sglang.experts import ExpertObservation
        from markov_collection.adapters.frameworks.sglang.parallel import CollectiveCall
        from markov_collection.adapters.frameworks.sglang.pd_templates import DisaggregatedTemplates
        from markov_collection.adapters.frameworks.sglang.preparation import PrefixLocations
        from markov_collection.adapters.runtimes.ascend.concatenation import AscendConcatenation
        from markov_collection.adapters.runtimes.ascend.indexed_writes import AscendIndexedWrites
        from markov_collection.adapters.runtimes.ascend.layouts import AscendLayouts
        from markov_collection.adapters.runtimes.ascend.matrices import AscendMatrices
        from markov_collection.adapters.runtimes.pytorch.collectives import AllReduceGeometry
        from markov_collection.adapters.runtimes.pytorch.indexing import InputGeometries, TensorAssignmentCall

        directory = path.parent
        source = json.loads(path.read_text())
        grouped = source["environment"]["source_transport"] != "physical_group_of_one"
        physical_path = output.parent / (f"physical-{path.name}" if grouped else "physical.json")
        if profile:

            # A failed reinterpretation must not leave an older physical result marked as complete.
            physical_path.unlink(missing_ok=True)

        # Interpretation can be repeated from the captured facts without another native session.
        SGLangSource.declare_execution_domain(source, directory)
        SGLangSource.control_interfaces(source)
        SGLangSource.model_interfaces(source)
        ExpertObservation.declare(source)
        BatchTensors.declare_model_inputs(source["regions"], source["values"])
        PrefixLocations.declare(source)
        AscendMatrices.declare(
            source["regions"], source["operations"], source["values"], source["environment"]["matrix_selection"]
        )
        CollectiveCall.bind_transports(source["regions"], source["operations"], source["values"])
        if source["environment"]["logical_pp"] > 1:
            from markov_collection.adapters.frameworks.sglang.pipeline import PipelineObservation

            PipelineObservation.declare_waits(source["regions"])

        DisaggregatedTemplates.declare(source)
        SGLangSource.pool_interfaces(source)

        # Refine framework-local pool geometry before runtime rules consume the completed declarations.
        SGLangAllocation.declare(source["regions"], source["operations"], source["values"])
        geometries = InputGeometries(source["regions"], source["values"])
        AscendConcatenation.declare(source["regions"], source["operations"], source["values"], geometries)
        ModelPaddingCall.declare(source["regions"], source["values"], geometries)
        TensorAssignmentCall.declare(source["regions"], source["values"], geometries)
        AscendIndexedWrites.declare(source["regions"], source["values"], geometries)
        AscendLayouts.declare(source["regions"], source["operations"], source["values"], geometries)
        AllReduceGeometry.declare(source["operations"], source["values"])
        write_json(output, source)
        if not profile:
            return

        traces = directory / "native" / path.stem if grouped else directory / "native"
        observer = AscendTrace.from_capture(
            traces, output.parent / "native" / path.stem, directory / "runtime",
            device=source["environment"]["device_index"], communication=communication,
        )
        physical = observer.physical_work(len(source["regions"]), len(source["operations"]))
        write_json(physical_path, physical)

    @classmethod
    def control_interfaces(cls, source: dict[str, Any]) -> None:
        cls.schedule_interfaces(source)
        cls.result_interfaces(source["regions"])
        for region in source["regions"]:
            metadata = region["metadata"]
            if region["role"] in {"KvAdvertise", "KvReady", "KvSubmit", "KvComplete", "KvPoll"}:

                # Explicit CPU services use their recorded thread even when the profiler misses background scopes.
                metadata.update(
                    interface=region["role"], inputs=[], outputs=[], writes=[], arguments=[], keywords={"mapping": []},
                    native_work={
                        "kind": "host", "resource": "thread", "identity": region["thread"], "name": region["role"],
                    },
                )

            if metadata.get("entrypoint") != "cache_match_prefix":
                continue

            # MatchResult's returned tensor distinguishes a cache hit from the native empty-match service.
            indices = metadata["result"][0]["value"]
            metadata["ports"] = {"inputs": {}, "outputs": {"prefix_indices": indices}}
            if source["values"][indices]["shape"] == [0]:
                metadata["entrypoint"] = "cache_empty"
                metadata.pop("dynamic_calls", None)

    @staticmethod
    def pool_interfaces(source: dict[str, Any]) -> None:
        for region in source["regions"]:
            metadata = region["metadata"]

            # Preparation crosses the real cache/allocator seam; it cannot execute as one late call.
            if metadata.get("entrypoint") == "batch_preparation":
                metadata["resource_calls"] = ["cache_evict", "kv_allocate"]
            elif region["role"] == "KvReserve":

                # PD admission and model preparation expose the same allocator implementation boundary.
                metadata.update(
                    entrypoint="KvReserve", entrypoint_phase=None, path="KvReserve",
                    resource_calls=["kv_allocate"], required_inputs=[],
                )

            # Host compositions delegate pool transitions to their callable child interfaces.
            if "entrypoint" not in metadata or "ports" not in metadata or metadata.get("dynamic_calls"):
                continue
            inputs = metadata["ports"]["inputs"]
            if effects := metadata.get("state_effects"):

                # Pool APIs are selected by their operands and state, independently of the enclosing model phase.
                metadata["entrypoint_phase"] = None

                # State may be initialized by its owner; API operands must come from the caller.
                metadata["required_inputs"] = sorted(
                    set(metadata.get("required_inputs", [])) | (inputs.keys() - effects.keys())
                )

            for name, identity in inputs.items():
                if name not in ("free_pages", "release_pages"):
                    continue

                # Incoming pool geometry belongs to this call, not to a previous producer's parameters.
                parameter = name + "_before"
                metadata.setdefault("parameters", {})[parameter] = source["values"][identity]["shape"][0]
                metadata["shape_contracts"] = [
                    item for item in metadata.get("shape_contracts", []) if item["value"] != identity
                ] + [{"value": identity, "shape": [parameter]}]

    @staticmethod
    def schedule_interfaces(source: dict[str, Any]) -> None:
        for region in source["regions"]:
            metadata = region["metadata"]
            if region["role"] == "Schedule":
                batch = source["batches"][metadata["batch"]]
                metadata.update(
                    entrypoint="Schedule",
                    entrypoint_phase=None,
                    dynamic_calls=True,
                    parameters={"scheduled_members": len(batch["members"])},
                )
            elif metadata.get("path") == "prepare_mlp_sync_batch_raw":

                # A None decision still executes the native coordination body.
                metadata.update(
                    entrypoint="batch_coordination", entrypoint_phase=None, ports={"inputs": {}, "outputs": {}}
                )

    @staticmethod
    def model_interfaces(source: dict[str, Any]) -> None:
        for region in source["regions"]:
            metadata = region["metadata"]
            if region["role"] != "module" or metadata["path"] != "" or "ports" not in metadata:
                continue

            ports = metadata["ports"]["inputs"]

            # Named module state and KV are persistent tensors; native reads can expose fresh views of them.
            state = [*source["environment"]["model_state"], "cache.k", "cache.v"]
            metadata.update(
                entrypoint="model",
                call_family="model",
                required_inputs=sorted(ports),
                resource_inputs=list(dict.fromkeys(ports[name] for name in state)),
            )

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
        pipeline_results: list[dict[str, Any] | None] = []
        for region in regions:
            metadata = region["metadata"]
            parent = region["parent"]
            pipeline = pipeline_results[parent] if parent is not None else None
            if metadata.get("entrypoint") == "pipeline_result_tokens":
                pipeline = metadata
            pipeline_results.append(pipeline)

            # Relay rows come from the actual indexed write, never a reconstructed slot tensor.
            if pipeline is not None and metadata.get("interface") == "tensor_assignment":
                tensor, indices, _ = metadata["arguments"]
                if tensor["value"] == pipeline["ports"]["inputs"]["token_relay"]:
                    pipeline["ports"]["inputs"]["request_slots"] = indices["value"]
                    pipeline["required_inputs"] = ["request_slots", "sampled_tokens"]
                    contracts = pipeline.setdefault("shape_contracts", [])
                    contract = {"value": indices["value"], "shape": ["B"]}
                    if contract not in contracts:
                        contracts.append(contract)

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
