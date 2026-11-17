"""Import independent complete KT forwards into the shared performance database."""

from dataclasses import asdict
from decimal import Decimal
import json

from markov.artifacts.performance import Measurement
from markov.artifacts.request import Deployment, read_json
from markov.adapters.ktransformers.probe import IMPLEMENTATION, TIMING_SCOPE


def work_features(layers, query, context_tokens, resident, threads, pools):
    cpu_pairs = npu_pairs = active_cpu = active_npu = 0
    for layer in layers:
        counts = layer["moe"]["expert_counts"]
        cpu, npu = counts[resident:], counts[:resident]
        cpu_pairs += sum(cpu)
        npu_pairs += sum(npu)
        active_cpu += sum(count > 0 for count in cpu)
        active_npu += sum(count > 0 for count in npu)
    return {
        "query_tokens": query,
        "context_tokens": context_tokens,
        "attention_pairs": query * (2 * context_tokens - query + 1) / 2,
        "layers": len(layers),
        "cpu_token_expert_pairs": cpu_pairs,
        "npu_token_expert_pairs": npu_pairs,
        "active_cpu_experts": active_cpu,
        "active_npu_experts": active_npu,
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
        or result.get("timing_scope") != TIMING_SCOPE
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
        "timing_scope": captured.get("timing_scope"),
        "resource_envelope": captured["resource_envelope"],
    }


def recipes(captured):
    return {
        (f"forward.{phase}", IMPLEMENTATION, TIMING_SCOPE): {
            "feature": [
                "query_tokens",
                "attention_pairs",
                "cpu_pairs_per_thread",
                "active_cpu_experts_per_pool",
                "npu_token_expert_pairs",
                "active_npu_experts",
            ],
            "nonnegative": True,
            "coverage_features": [
                "layers",
                "context_tokens",
                "cpu_token_expert_pairs",
                "active_cpu_experts",
                "cpu_threads",
                "numa_pools",
                "resident_experts",
            ],
            "extrapolation_features": [
                "context_tokens",
                "attention_pairs",
                "cpu_token_expert_pairs",
                "npu_token_expert_pairs",
                "active_cpu_experts",
                "active_npu_experts",
                "cpu_pairs_per_thread",
                "active_cpu_experts_per_pool",
            ],
            "limitations": [
                "Complete serial forward includes dense/attention/MoE work, transfers and internal CPU/NPU overlap; no source non-MoE remainder is added.",
                "The end-of-forward stream synchronization closes the observation; its perturbation and caller work outside the forward remain limitations.",
                "Work per thread, active experts per pool and causal context size approximate scaling; the context proxy does not reconstruct compressed/sliding attention kernels.",
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
                if fields["status"] != "returned" or fields["implementation"] != IMPLEMENTATION or fields.get("timing_scope") != TIMING_SCOPE:
                    raise ValueError("KT independent forward failed or uses another implementation")
                warmup = all(
                    int(rid.removeprefix("request_")) < captured["formal_begin_index"] for rid in fields["request_ids"]
                )
                settings = captured["configuration"]
                query = fields["query_tokens"]
                if not fields["layers"] or fields["context_tokens"] < query:
                    raise ValueError("KT independent forward has incomplete routing or context")
                for layer in fields["layers"]:
                    moe = layer["moe"]
                    if (
                        len(moe["expert_counts"]) != moe["experts"]
                        or sum(moe["expert_counts"]) != query * moe["top_k"]
                        or (moe["cpu_threads"], moe["numa_pools"], moe["resident_experts"])
                        != (settings["kt_cpuinfer"], settings["kt_threadpool_count"], settings["kt_num_gpu_experts"])
                    ):
                        raise ValueError("KT independent routing or resources disagree with the capture")
                features = work_features(
                    fields["layers"], query, fields["context_tokens"], settings["kt_num_gpu_experts"],
                    settings["kt_cpuinfer"], settings["kt_threadpool_count"]
                )
                position += 1
                yield Measurement(
                    position,
                    "closed_forward",
                    f"forward.{fields['phase']}",
                    IMPLEMENTATION,
                    TIMING_SCOPE,
                    features,
                    int(Decimal(str(event["dur"])) * 1000),
                    warmup,
                )
