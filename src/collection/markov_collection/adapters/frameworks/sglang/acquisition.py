"""SGLang/Ascend discovery recipe and native acquisition entrypoint."""

from __future__ import annotations

import argparse
import json
from collections.abc import Iterator, Sequence
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
                    pools = configuration["pd"].values() if "pd" in configuration else (configuration,)
                    for pool in pools:
                        if "ep" not in pool:
                            if experts:
                                raise ValueError("MoE instances require an EP topology and routes, including EP1")
                            continue
                        if not experts:
                            raise ValueError("EP topology requires a model with routed experts")
                        topology = pool["ep"]
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
            "attention_backend": configuration["attention_backend"],
            "page_size": configuration["page_size"],
        }
        if "pd" in configuration:
            domain["pd"] = tuple(
                (
                    role,
                    configuration["pd"][role]["tp"],
                    configuration["pd"][role].get("pp", 1),
                    configuration["pd"][role].get("ep", {}).get("size", 1),
                )
                for role in ("prefill", "decode")
            )
        else:
            domain.update(
                tp=configuration["tp"], pp=configuration.get("pp", 1), ep=configuration.get("ep", {}).get("size", 1)
            )
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
    def source_groups(domain: dict[str, Any], plan: dict[str, Any] | None = None) -> Iterator[Sequence[int]]:
        if domain.get("pd"):
            yield range(SGLangAcquisition.required_devices(domain)) if plan is None else tuple(plan["representatives"])
        elif domain["pp"] > 1 or domain["ep"] > 1:
            yield (
                NativeServingSession.source_roles(domain["tp"], domain["pp"], 0, domain["ep"])
                if plan is None
                else tuple(plan["representatives"])
            )
        else:
            ranks = range(domain["tp"]) if plan is None else plan["representatives"]
            for rank in ranks:
                yield range(rank, rank + 1)

    @staticmethod
    def role_plan(domain: dict[str, Any], bootstrap: Path) -> dict[str, Any]:
        if domain.get("pd") or domain["pp"] > 1 or domain["ep"] > 1:
            return json.loads((bootstrap / "role-plan.json").read_text())
        if not domain.get("pd") and domain["pp"] == 1 and domain["ep"] == 1 and domain["tp"] > 1:
            source = json.loads((bootstrap / "rank-0.json").read_text())
            return source["environment"]["worker_roles"]
        return {
            "representatives": [rank for group in SGLangAcquisition.source_groups(domain) for rank in group],
            "instances": [],
        }

    @staticmethod
    def observation_directories(domain: dict[str, Any], capture: Path) -> tuple[Path, ...]:
        return (capture / "prefill", capture / "decode") if domain.get("pd") else (capture,)

    @staticmethod
    def required_devices(domain: dict[str, Any]) -> int:
        if domain.get("pd"):
            from markov_collection.adapters.frameworks.sglang.pd_session import NativeDisaggregatedSession

            return sum(len(instance["ranks"]) for instance in NativeDisaggregatedSession.instances(domain))
        return len(NativeServingSession.source_roles(domain["tp"], domain["pp"], 0, domain["ep"]))

    @staticmethod
    def source_name(domain: dict[str, Any], rank: int) -> str:
        prefix = SGLangAcquisition.instance(domain, rank)["role"] + "/" if domain.get("pd") else ""
        return prefix + f"rank-{rank}.json"

    @staticmethod
    def physical_name(domain: dict[str, Any], rank: int) -> str:
        if domain.get("pd"):
            return SGLangAcquisition.instance(domain, rank)["role"] + f"/physical-rank-{rank}.json"
        return f"physical-rank-{rank}.json" if domain["pp"] > 1 or domain["ep"] > 1 else "physical.json"

    @staticmethod
    def instance(domain: dict[str, Any], rank: int) -> dict[str, Any]:
        from markov_collection.adapters.frameworks.sglang.pd_session import NativeDisaggregatedSession

        return next(instance for instance in NativeDisaggregatedSession.instances(domain) if rank in instance["ranks"])

    @staticmethod
    def row_limits(configurations: list[dict[str, Any]], max_prompt_tokens: int) -> dict[str, int]:
        members = 0
        rows = max_prompt_tokens
        for configuration in configurations:
            prefill = configuration["pd"]["prefill"] if "pd" in configuration else configuration
            budget = prefill["max_prefill_tokens"]
            page_size = configuration["page_size"]

            # Native Prefill can batch whole requests in both serving modes, independently of our controller.
            prefix = (budget - 1) // page_size * page_size
            rows = max(rows, budget, prefix + max_prompt_tokens)

            pools = configuration["pd"].values() if "pd" in configuration else (configuration,)
            for pool in pools:
                size = pool.get("ep", {}).get("size", 1)

                # Native MLP synchronization pads Decode members as well as Prefill tokens.
                members = max(members, (pool["max_running_requests"] + size - 1) // size * size)
                rows = (rows + size - 1) // size * size
        return {"M": max(rows, members), "B": members}

    @staticmethod
    def request(
        specification: dict[str, Any],
        domain: dict[str, Any],
        rank: int,
        output: str,
        selector: str,
        model_configuration: dict[str, Any],
    ) -> dict[str, Any]:
        requests = NativeServingSession.structure_requests(domain["page_size"])
        if domain.get("pd"):
            from markov_collection.adapters.frameworks.sglang.pd_session import NativeDisaggregatedSession

            requests = NativeDisaggregatedSession.structure_requests(domain["page_size"])
        model = specification["model"]
        configurations = [
            configuration
            for configuration in specification["configurations"]
            if SGLangAcquisition.domain(model, configuration) == domain
        ]
        representative = (
            {role: SGLangAcquisition.configuration(domain["page_size"], pp) for role, _, pp, _ in domain["pd"]}
            if domain.get("pd")
            else SGLangAcquisition.configuration(domain["page_size"], domain["pp"])
        )
        if domain.get("pd"):

            # In-flight transfers retain slots; let the fixed representative set reach multi-member admission.
            for pool in representative.values():
                pool["max_running_requests"] = max(pool["max_running_requests"], len(requests))
        configurations.append(
            {"page_size": domain["page_size"], "pd": representative} if domain.get("pd") else representative
        )
        maximum_prompt = max(
            max(prompt for prompt, _ in requests),
            max(
                request["prompt_tokens"] for workload in specification["workloads"] for request in workload["requests"]
            ),
        )
        limits = SGLangAcquisition.row_limits(configurations, maximum_prompt)
        result = {
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
        if domain.get("pd"):
            result["submissions"] = NativeDisaggregatedSession.structure_submissions(requests)
        if model_configuration.get("num_experts", 0):
            result["serving_input"] = SGLangAcquisition.source_input(
                result, model_configuration, specification["node_durations_ns"]
            )
        return result

    @staticmethod
    def source_input(request: dict[str, Any], model: dict[str, Any], durations: dict[str, int]) -> dict[str, Any]:
        requests = [
            {"id": f"representative-{index}", "arrival_ns": 0, "prompt_tokens": prompt, "output_tokens": output}
            for index, (prompt, output) in enumerate(request["requests"])
        ]
        roles = request["pd"] if "pd" in request else (("model", request["tp"], request["pp"], request["ep"]),)
        pools = {}
        routes = {}
        for role, tp, pp, ep in roles:
            settings = request["configuration"][role] if "pd" in request else request["configuration"]
            pool = {name: settings[name] for name in ("max_running_requests", "max_total_tokens", "max_prefill_tokens")}
            topology = {"id": f"representative-{role}", "size": ep}
            pool["ep"] = topology
            if "pd" in request:
                pool["tp"] = tp
                if role == "decode":
                    del pool["max_prefill_tokens"]
            if pp > 1:
                pool.update(pp=pp, microbatch_requests=settings["pp_max_micro_batch_size"])
            pools[role] = pool
            routes[topology["id"]] = ExpertRouteConditions(model, ep).representative(requests)

        configuration = {"page_size": request["page_size"]}
        if "pd" in request:
            configuration["pd"] = pools
        else:
            configuration.update(pools["model"])
        return {
            "configuration": configuration,
            "requests": requests,
            "ep_routes": routes,
            "node_durations_ns": durations,
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
            observe_all_workers=request.get("observe_all_workers", False),
            serving_input=request.get("serving_input"),
        ).run()
        summary = json.loads((Path(request["output"]) / "session-summary.json").read_text())
        for rank in summary["observed_roles"]:
            (Path(request["output"]) / f"rank-{rank}.json").stat()

    @staticmethod
    def interpret(request: dict[str, Any]) -> None:
        if request.get("pd"):
            from markov_collection.adapters.frameworks.sglang.pd_session import NativeDisaggregatedSession

            NativeDisaggregatedSession.interpret(request)
            return

        directory = Path(request["output"])
        summary = json.loads((directory / "session-summary.json").read_text())
        for rank in summary["observed_roles"]:
            SGLangAcquisition.interpret_role(directory, rank, summary["logical_pp"], summary["profile"])

    @staticmethod
    def interpret_role(directory: Path, rank: int, pp: int, profile: bool) -> None:
        from markov_collection.adapters.frameworks.sglang.allocation import SGLangAllocation
        from markov_collection.adapters.frameworks.sglang.batches import BatchTensors
        from markov_collection.adapters.frameworks.sglang.expert_geometry import ModelPaddingCall
        from markov_collection.adapters.frameworks.sglang.experts import ExpertObservation
        from markov_collection.adapters.frameworks.sglang.hooks import SGLangObservation
        from markov_collection.adapters.frameworks.sglang.parallel import CollectiveCall
        from markov_collection.adapters.frameworks.sglang.pd_templates import DisaggregatedTemplates
        from markov_collection.adapters.frameworks.sglang.state import PoolPorts
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
        ExpertObservation.declare(source)
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
        PoolPorts.declare(source["regions"], source["values"])
        if pp > 1:
            from markov_collection.adapters.frameworks.sglang.pipeline import PipelineObservation

            PipelineObservation.declare_waits(source["regions"])
        write_json(path, source)
        if not profile:
            return

        grouped = source["environment"]["source_transport"] != "physical_group_of_one"
        name = f"physical-rank-{rank}.json" if grouped else "physical.json"

        # Physical mapping belongs to immutable trace identities; declaration fixes can reuse the completed export.
        if (directory / name).exists():
            return
        traces = directory / "native" / f"rank-{rank}" if grouped else directory / "native"
        trace = AscendTrace.exported_trace(traces)
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
