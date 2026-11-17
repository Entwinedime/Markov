"""Import independent KT closed regions into the shared performance database."""

from dataclasses import asdict
import json

from markov.artifacts.performance import Measurement
from markov.artifacts.request import Deployment, read_json
from markov.adapters.ktransformers.probe import IMPLEMENTATION


def work_features(counts, query, resident, threads, pools):
    cpu = counts[resident:]
    npu = counts[:resident]
    cpu_pairs, active_cpu = sum(cpu), sum(count > 0 for count in cpu)
    return {
        "query_tokens": query,
        "cpu_token_expert_pairs": cpu_pairs,
        "npu_token_expert_pairs": sum(npu),
        "active_cpu_experts": active_cpu,
        "active_npu_experts": sum(count > 0 for count in npu),
        "cpu_threads": threads,
        "numa_pools": pools,
        "resident_experts": resident,
        "cpu_pairs_per_thread": cpu_pairs / threads,
        "active_cpu_experts_per_pool": active_cpu / pools,
    }


def metadata(path):
    result = read_json(path)
    if (
        result["kind"] != "independent_kt_measurements"
        or result["status"] != "completed"
        or result["profiling"]
        or not result["files"]
    ):
        raise ValueError(f"KT independent measurement is incomplete: {path}")
    return result


def context(captured, deployment):
    current = asdict(Deployment.parse(captured["deployment"]))
    if current != deployment:
        raise ValueError("KT measurements belong to another deployment")
    settings = captured["configuration"]
    # Resource counts are work variables. Quantization and execution policy are not.
    return {
        **current,
        "kt_method": settings["kt_method"],
        "quantization": settings["quantization"],
        "disable_cuda_graph": settings["disable_cuda_graph"],
        "disable_overlap_schedule": settings["disable_overlap_schedule"],
        "placement": settings.get("kt_expert_placement_strategy", "prefix"),
        "resource_envelope": captured["resource_envelope"],
    }


def recipes(captured):
    return {
        (f"moe.{phase}", IMPLEMENTATION, "closed_stream_response"): {
            "feature": [
                "query_tokens",
                "cpu_pairs_per_thread",
                "active_cpu_experts_per_pool",
                "npu_token_expert_pairs",
                "active_npu_experts",
            ],
            "nonnegative": True,
            "coverage_features": [
                "cpu_token_expert_pairs",
                "active_cpu_experts",
                "cpu_threads",
                "numa_pools",
                "resident_experts",
            ],
            "extrapolation_features": [
                "cpu_token_expert_pairs",
                "npu_token_expert_pairs",
                "active_cpu_experts",
                "active_npu_experts",
                "cpu_pairs_per_thread",
                "active_cpu_experts_per_pool",
            ],
            "limitations": [
                "Closed native MoE response includes D2H, CPU/NPU overlap, drain, merge and H2D; constituent services are not separately identifiable.",
                "CPU work per thread and active experts per pool approximate scaling; bandwidth saturation and placement can break this model.",
                "CPU/NUMA counts share coefficients through work variables; unmeasured combinations need independent verification and base routing remains conditional.",
            ],
        }
        for phase in captured["phases"]
    }


def measurements(path, captured):
    position = 0
    for name in captured["files"]:
        source = (path.parent / name).resolve()
        if not source.is_relative_to(path.parent.resolve()):
            raise ValueError("KT measurement file escapes its capture directory")
        with source.open() as stream:
            for line in stream:
                event = json.loads(line)
                if event["name"] != "runtime.kt.batch":
                    continue
                fields = event["args"]
                if fields["status"] != "returned" or fields["implementation"] != IMPLEMENTATION:
                    raise ValueError("KT independent forward failed or uses another implementation")
                warmup = all(
                    int(rid.removeprefix("request_")) < captured["formal_begin_index"] for rid in fields["request_ids"]
                )
                for layer in fields["layers"]:
                    position += 1
                    moe = layer["moe"]
                    features = work_features(
                        moe["expert_counts"],
                        fields["query_tokens"],
                        moe["resident_experts"],
                        moe["cpu_threads"],
                        moe["numa_pools"],
                    )
                    yield Measurement(
                        position,
                        "closed_moe",
                        f"moe.{fields['phase']}",
                        IMPLEMENTATION,
                        "closed_stream_response",
                        features,
                        moe["stream_ns"],
                        warmup,
                    )
