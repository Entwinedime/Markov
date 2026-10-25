"""SGLang/Ascend discovery recipe and native acquisition entrypoint."""

from __future__ import annotations

import argparse
import json
from collections.abc import Iterator
from pathlib import Path
from typing import Any

from markov_collection.adapters.frameworks.sglang.routes import ExpertRouteConditions
from markov_collection.adapters.frameworks.sglang.session import NativeServingSession
from markov_collection.adapters.runtimes.ascend.trace import AscendTrace
from markov_collection.artifacts.json_file import write_json


class SGLangAcquisition:
    service = "sglang-profile"
    sdk_include = "/usr/local/Ascend/ascend-toolkit/latest/include"
    selector_binary = "markov_ascend_matmul_selection"
    observer_library = "libmarkov_ascend_observer.so"

    @staticmethod
    def check_workloads(model: dict[str, Any], specification: dict[str, Any]) -> dict[str, str]:
        experts = model.get("num_experts", 0)
        failures = {}
        for configuration in specification["configurations"]:
            for workload in specification["workloads"]:
                try:
                    if "ep" not in configuration:
                        if experts:
                            raise ValueError("MoE configurations require an EP topology and routes, including EP1")
                        continue
                    if not experts:
                        raise ValueError("EP topology requires a model with routed experts")
                    topology = configuration["ep"]
                    source = workload.get("ep_routes", {}).get(topology["id"])
                    ExpertRouteConditions(model, topology["size"]).check(source, workload["requests"])
                except (KeyError, TypeError, ValueError) as error:
                    failures[f"{configuration['id']}/{workload['id']}"] = f"route input: {error}"
        return failures

    @staticmethod
    def observer_commands(directory: str) -> tuple[list[str], list[str]]:
        return (
            [
                "cmake",
                "-S",
                "/workspace/trace-sim/src/collection/adapters/runtimes/ascend",
                "-B",
                directory,
                "-G",
                "Ninja",
                "-DCMAKE_BUILD_TYPE=Release",
                f"-DMARKOV_ASCEND_INCLUDE_DIR={SGLangAcquisition.sdk_include}",
            ],
            ["cmake", "--build", directory, "-j2"],
        )

    @staticmethod
    def domain(model: dict[str, Any], configuration: dict[str, Any]) -> dict[str, Any]:
        domain = {
            "dtype": model["dtype"],
            "tp": configuration["tp"],
            "pp": configuration.get("pp", 1),
            "ep": configuration["ep"]["size"] if "ep" in configuration else 1,
            "attention_backend": configuration["attention_backend"],
            "page_size": configuration["page_size"],
        }
        if "pd" in configuration:
            domain["pd"] = True
        return domain

    @staticmethod
    def configuration(page_size: int, pp: int = 1) -> dict[str, int]:
        configuration = {
            "page_size": page_size,
            "max_running_requests": 2,
            "max_total_tokens": 32 * page_size,
            "max_prefill_tokens": 32 * page_size,
        }
        if pp > 1:
            configuration.update(max_running_requests=4, pp_max_micro_batch_size=2, pp_async_batch_depth=0)
        return configuration

    @staticmethod
    def source_groups(domain: dict[str, Any]) -> Iterator[range]:
        if domain.get("pd"):
            yield range(2)
        elif domain["pp"] > 1 or domain["ep"] > 1:
            yield NativeServingSession.source_roles(domain["tp"], domain["pp"], 0, domain["ep"])
        else:
            for rank in range(domain["tp"]):
                yield range(rank, rank + 1)

    @staticmethod
    def required_devices(domain: dict[str, Any]) -> int:
        return (
            2
            if domain.get("pd")
            else len(NativeServingSession.source_roles(domain["tp"], domain["pp"], 0, domain["ep"]))
        )

    @staticmethod
    def source_name(domain: dict[str, Any], rank: int) -> str:
        prefix = ("prefill", "decode")[rank] + "/" if domain.get("pd") else ""
        return prefix + f"rank-{rank}.json"

    @staticmethod
    def physical_name(domain: dict[str, Any], rank: int) -> str:
        if domain.get("pd"):
            return ("prefill", "decode")[rank] + "/physical.json"
        return f"physical-rank-{rank}.json" if domain["pp"] > 1 or domain["ep"] > 1 else "physical.json"

    @staticmethod
    def row_limits(configurations: list[dict[str, Any]], max_prompt_tokens: int) -> dict[str, int]:
        members = 0
        rows = max_prompt_tokens
        for configuration in configurations:
            prefill = configuration["pd"]["prefill"] if "pd" in configuration else configuration
            members = max(members, prefill["max_running_requests"])
            if "pd" in configuration:
                members = max(members, configuration["pd"]["decode"]["max_running_requests"])
                continue
            budget = prefill["max_prefill_tokens"]
            page_size = configuration["page_size"]

            # A final whole request can start while the page-aligned prefix still has positive budget.
            prefix = (budget - 1) // page_size * page_size
            rows = max(rows, budget, prefix + max_prompt_tokens)
        return {"M": max(rows, members), "B": members}

    @staticmethod
    def request(
        specification: dict[str, Any], domain: dict[str, Any], rank: int, output: str, selector: str
    ) -> dict[str, Any]:
        requests = NativeServingSession.warmup_requests(domain["page_size"])
        if domain.get("pd"):
            from markov_collection.adapters.frameworks.sglang.pd_session import NativeDisaggregatedSession

            requests = NativeDisaggregatedSession.structure_requests(domain["page_size"])
        model = specification["model"]
        configurations = [
            configuration
            for configuration in specification["configurations"]
            if SGLangAcquisition.domain(model, configuration) == domain
        ]
        representative = SGLangAcquisition.configuration(domain["page_size"], domain["pp"])
        configurations.append(
            {"page_size": domain["page_size"], "pd": {"prefill": representative, "decode": representative}}
            if domain.get("pd")
            else representative
        )
        maximum_prompt = max(
            request["prompt_tokens"] for workload in specification["workloads"] for request in workload["requests"]
        )
        limits = SGLangAcquisition.row_limits(configurations, maximum_prompt)
        if domain["ep"] > 1:
            size = domain["ep"]
            limits["M"] = (limits["M"] + size - 1) // size * size
        if domain.get("pd"):
            # Target P runs one prompt; native representative acquisition can still batch its own requests.
            limits["M"] = max(
                limits["M"], representative["max_running_requests"] * max(prompt for prompt, _ in requests)
            )
        return {
            "model": model["path"],
            **domain,
            "rank": rank,
            "output": output,
            "requests": requests,
            "configuration": representative,
            "matrix_selection": {
                "binary": selector,
                "limits": limits,
            },
        }

    @staticmethod
    def command(stage: str, request: str) -> list[str]:
        return [
            "python3",
            "-m",
            "markov_collection.adapters.frameworks.sglang.acquisition",
            stage,
            "--request",
            request,
        ]

    @staticmethod
    def capture(request: dict[str, Any]) -> None:
        if request.get("pd"):
            from markov_collection.adapters.frameworks.sglang.pd_session import NativeDisaggregatedSession

            request["native_library"] = str(
                Path(request["matrix_selection"]["binary"]).resolve().with_name(SGLangAcquisition.observer_library)
            )
            NativeDisaggregatedSession(request).run()
            return

        NativeServingSession(
            request["model"],
            tuple(tuple(pair) for pair in request["requests"]),
            Path(request["output"]),
            True,
            request["tp"],
            request["rank"],
            configuration=request["configuration"],
            matrix_selection=request["matrix_selection"],
            native_library=str(
                Path(request["matrix_selection"]["binary"]).resolve().with_name(SGLangAcquisition.observer_library)
            ),
            pp_size=request["pp"],
            ep_size=request["ep"],
            dtype=request["dtype"],
            attention_backend=request["attention_backend"],
        ).run()
        ranks = NativeServingSession.source_roles(request["tp"], request["pp"], request["rank"], request["ep"])
        for rank in ranks:
            (Path(request["output"]) / f"rank-{rank}.json").stat()

    @staticmethod
    def interpret(request: dict[str, Any]) -> None:
        if request.get("pd"):
            from markov_collection.adapters.frameworks.sglang.pd_session import NativeDisaggregatedSession

            NativeDisaggregatedSession.interpret(request)
            return

        directory = Path(request["output"])
        summary = json.loads((directory / "session-summary.json").read_text())
        ranks = NativeServingSession.source_roles(
            summary["logical_tp"], summary["logical_pp"], request["rank"], summary["logical_ep"]
        )
        for rank in ranks:
            SGLangAcquisition.interpret_role(
                directory, rank, summary["logical_pp"], summary["profile"], summary["logical_ep"]
            )

    @staticmethod
    def interpret_role(directory: Path, rank: int, pp: int, profile: bool, ep: int = 1) -> None:
        from markov_collection.adapters.frameworks.sglang.allocation import SGLangAllocation
        from markov_collection.adapters.frameworks.sglang.batches import BatchTensors
        from markov_collection.adapters.frameworks.sglang.expert_geometry import ModelPaddingCall
        from markov_collection.adapters.frameworks.sglang.hooks import SGLangObservation
        from markov_collection.adapters.frameworks.sglang.parallel import CollectiveCall
        from markov_collection.adapters.frameworks.sglang.pd_templates import DisaggregatedTemplates
        from markov_collection.adapters.runtimes.ascend.concatenation import AscendConcatenation
        from markov_collection.adapters.runtimes.ascend.indexed_writes import AscendIndexedWrites
        from markov_collection.adapters.runtimes.ascend.layouts import AscendLayouts
        from markov_collection.adapters.runtimes.ascend.matrices import AscendMatrices
        from markov_collection.adapters.runtimes.pytorch.collectives import AllReduceGeometry
        from markov_collection.adapters.runtimes.pytorch.indexing import TensorAssignmentCall

        path = directory / f"rank-{rank}.json"
        source = json.loads(path.read_text())

        # Interpretation can be repeated from the captured facts without another native session.
        SGLangObservation.result_interfaces(source["regions"])
        BatchTensors.declare_model_inputs(source["regions"], source["values"])
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
        if pp > 1:
            from markov_collection.adapters.frameworks.sglang.pipeline import PipelineObservation

            PipelineObservation.declare_waits(source["regions"])
        write_json(path, source)
        if not profile:
            return

        name = SGLangAcquisition.physical_name({"pp": pp, "ep": ep}, rank)

        # Physical mapping belongs to immutable trace identities; declaration fixes can reuse the completed export.
        if (directory / name).exists():
            return
        traces = directory / "native" / f"rank-{rank}" if pp > 1 or ep > 1 else directory / "native"
        (trace,) = traces.rglob("trace_view.json")
        observer = AscendTrace(trace, directory / "runtime", device=source["environment"]["device_index"])
        physical = observer.physical_work(len(source["regions"]), len(source["operations"]))
        write_json(directory / name, physical)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stage", choices=("capture", "interpret"))
    parser.add_argument("--request", required=True, type=Path)
    arguments = parser.parse_args()
    request = json.loads(arguments.request.read_text())
    if arguments.stage == "capture":
        SGLangAcquisition.capture(request)
    else:
        SGLangAcquisition.interpret(request)
