"""Capture the single-NPU DeepSeek service through the common device lifecycle."""

import argparse
from collections import Counter
import json
import os
from pathlib import Path
import sys
import time

from markov.artifacts.capture import capture_origin
from markov.artifacts.request import positive_integer, read_json
from markov.artifacts.writing import write_json
from markov.instrumentation.http import SerialHttpClient, encode_inputs, inference_server
from markov.adapters.ktransformers.probe import TIMING_SCOPE


def auxiliary_mounts(configuration):
    template = Path(configuration["kt_weight_path"])
    if not template.is_absolute() or "{layer_idx}" not in template.name:
        raise ValueError("kt_weight_path must be an absolute GGUF path with {layer_idx} in its filename")
    template.parent.stat()
    return ((template.parent, "/experts", False),)


def validate_prediction(request):
    if request.execution.kernel_cache_initial_state not in (None, "empty"):
        raise ValueError("KT currently preserves base startup history; choose kernel_cache_initial_state=empty")
    if any(target.deployment.tensor_parallel != 1 for target in request.targets):
        raise ValueError(
            "KT targets currently keep the single-NPU deployment; only CPU and expert placement counts change"
        )


def server_arguments(deployment, configuration, *, calibration=False):
    if deployment.runtime != "ascend" or deployment.tensor_parallel != 1 or deployment.dtype != "bfloat16":
        raise ValueError("KT capture supports the single-NPU BF16 DeepSeek service with its declared quantization")
    owned = {"model_path", "dtype", "tp_size", "ep_size", "host", "port", "skip_server_warmup"}
    if owned.intersection(configuration):
        raise ValueError("Model, dtype and TP belong to deployment; capture owns the local endpoint and startup")
    for key in ("kt_cpuinfer", "kt_threadpool_count", "kt_num_gpu_experts"):
        positive_integer(configuration[key], key)
    if configuration["kt_cpuinfer"] < configuration["kt_threadpool_count"]:
        raise ValueError("Each KT thread pool needs at least one CPU inference thread")
    if configuration["kt_cpuinfer"] % configuration["kt_threadpool_count"]:
        raise ValueError("KT CPU threads must divide evenly between the native NUMA pools")
    if configuration.get("enable_hierarchical_cache", False):
        raise ValueError("HiCache is not part of the KT DeepSeek execution path")
    if not configuration.get("disable_cuda_graph", True) or not configuration.get("disable_overlap_schedule", True):
        raise ValueError("KT semantic capture currently requires explicit eager, non-overlap scheduling")
    if configuration.get("kt_expert_placement_strategy", "prefix") != "prefix":
        raise ValueError("KT prediction currently uses prefix expert placement")
    if (
        not configuration.get("disable_radix_cache", True)
        or configuration.get("max_running_requests", 1) != 1
        or configuration.get("kt_method", "LLAMAFILE") != "LLAMAFILE"
        or configuration.get("quantization", "compressed-tensors") != "compressed-tensors"
        or configuration.get("kt_max_deferred_experts_per_token", 0) != 0
    ):
        raise ValueError(
            "KT capture requires the existing serial LLAMAFILE/W8A8 path without radix reuse or deferred experts"
        )
    auxiliary_mounts(configuration)
    return {
        "device": "npu",
        "attention_backend": "ascend",
        "moe_a2a_backend": "none",
        "page_size": 128,
        "quantization": "compressed-tensors",
        "disable_shared_experts_fusion": True,
        "trust_remote_code": True,
        "disable_radix_cache": True,
        "disable_cuda_graph": True,
        "disable_overlap_schedule": True,
        "mem_fraction_static": 0.81,
        "context_length": 65536,
        "max_prefill_tokens": 65535,
        "chunked_prefill_size": 32768,
        "watchdog_timeout": 18000,
        "kt_method": "LLAMAFILE",
        "kt_max_deferred_experts_per_token": 0,
        **configuration,
        "kt_weight_path": "/experts/" + Path(configuration["kt_weight_path"]).name,
        "model_path": "/model",
        "dtype": deployment.dtype,
        "tp_size": 1,
        "ep_size": 1,
        "max_running_requests": 1,
        "host": "127.0.0.1",
        "port": 30000,
        "skip_server_warmup": True,
    }


def capture_environment(profiling, calibration=False, *, semantic=False):
    if profiling:
        raise ValueError("KT native profiler diagnostics are not yet supported; use capture.detail=semantic")
    return {
        "SGLANG_PLUGINS": "markov_kt_facts",
        "MARKOV_KT_FACTS": "/capture/probe" if semantic else "",
        "MARKOV_KT_MEASUREMENTS": "/capture/regions" if calibration else "",
        "SGLANG_ENABLE_HEALTH_ENDPOINT_GENERATION": "0",
        "SGLANG_SET_CPU_AFFINITY": "1",
        "PYTORCH_NPU_ALLOC_CONF": "expandable_segments:True",
        "TASK_QUEUE_ENABLE": "1",
        "KT_SIDE_STREAM": "0",
        "KT_PREFILL_STREAM": "0",
    }


class Capture(SerialHttpClient):
    def __init__(self, path):
        self.path = path
        self.plan = read_json(path)
        super().__init__(
            path.parent, self.plan["workload"]["formal_begin_index"], time.monotonic() + self.plan["budget"]["wall_sec"]
        )

    def run(self):
        server = self.plan["server"]
        model = read_json(Path(server["model_path"]) / "config.json")
        if model["model_type"] != "deepseek_v4":
            raise ValueError("This KT adapter interprets DeepSeek-V4; other execution rules are not implemented")
        if server["kt_num_gpu_experts"] >= model["n_routed_experts"]:
            raise ValueError("KT expert placement must leave at least one routed expert on CPU")
        threads, pools = server["kt_cpuinfer"], server["kt_threadpool_count"]
        expert_width = model["moe_intermediate_size"]
        if expert_width % pools or expert_width % 256 or pools > expert_width // 256:
            raise ValueError(
                "KT NUMA pools must divide the expert width and each receive a LLAMAFILE quantization block"
            )
        nodes = sorted(Path("/sys/devices/system/node").glob("node[0-9]*"), key=lambda node: int(node.name[4:]))
        resources = {
            "cpu_ids": sorted(os.sched_getaffinity(0)),
            "numa_nodes": len(nodes),
        }

        cores_by_node = {}
        for node in nodes:
            cores = set()
            for cpu in node.glob("cpu[0-9]*"):
                siblings = (cpu / "topology/thread_siblings_list").read_text().strip()
                cores.add(int(siblings.split(",")[0].split("-")[0]))
            cores_by_node[node.name.removeprefix("node")] = sorted(cores)
        resources["numa_core_cpu_ids"] = cores_by_node

        cpu_max = Path("/sys/fs/cgroup/cpu.max")
        if cpu_max.is_file():
            quota, period = cpu_max.read_text().split()
            resources["cpu_quota_cores"] = None if quota == "max" else int(quota) / int(period)
        else:
            quota = Path("/sys/fs/cgroup/cpu/cpu.cfs_quota_us")
            if quota.is_file():
                value = int(quota.read_text())
                resources["cpu_quota_cores"] = (
                    None if value < 0 else value / int(quota.with_name("cpu.cfs_period_us").read_text())
                )
        for path in (Path("/sys/fs/cgroup/memory.max"), Path("/sys/fs/cgroup/memory/memory.limit_in_bytes")):
            if path.is_file():
                value = path.read_text().strip()
                resources["memory_limit_bytes"] = None if value == "max" else int(value)
                break

        issue = None
        allowed = set(resources["cpu_ids"])
        for pool in range(pools):
            cores = cores_by_node.get(str(pool), [])

            # Native pools select the first local physical cores, one PU each.
            if len(cores) < threads // pools or not set(cores[: threads // pools]) <= allowed:
                issue = f"KT NUMA pool {pool} cannot bind {threads // pools} local physical cores in the container CPU set"
                break
        if issue is not None:
            artifact = (
                "profile_manifest.json"
                if self.plan.get("semantic", False)
                else "region_measurements.json"
                if self.plan["calibration"]
                else "normal_observation.json"
            )
            write_json(self.directory / artifact, {"status": "resource_infeasible", "resource_envelope": resources, "issue": issue})
            return

        for layer in range(model["num_hidden_layers"]):
            Path(server["kt_weight_path"].format(layer_idx=layer)).stat()
        if self.plan["workload"].get("drain_before_formal", False):
            raise ValueError("drain_before_formal is a HiCache operation, not a KT operation")
        requests = self.plan["workload"]["requests"]
        if any(not row["ignore_eos"] or row["return_logprob"] for row in requests):
            raise ValueError("KT prediction requires fixed output counts without logprob")
        tokenizer = None
        if any(row["prompt"] is not None for row in requests):
            from sglang.srt.utils.hf_transformers import get_tokenizer

            tokenizer = get_tokenizer(server["model_path"], trust_remote_code=True, local_files_only=True)
        encode_inputs(requests, server["model_path"], self.plan["budget"]["input_tokens"], True, tokenizer=tokenizer)
        if any(len(row["input_ids"]) > server["chunked_prefill_size"] for row in requests):
            raise ValueError("KT semantic prediction currently requires one Prefill chunk per request")
        for flag in ("MARKOV_KT_FACTS", "MARKOV_KT_MEASUREMENTS"):
            if self.plan["environment"].get(flag):
                Path(self.plan["environment"][flag]).mkdir(exist_ok=True)

        command = [sys.executable, "-m", "markov.adapters.ktransformers.capture", "server", "--input", str(self.path)]
        write_json(
            self.directory / "config.json",
            {
                "deployment": self.plan["deployment"],
                "model_configuration": model,
                "resource_envelope": resources,
                "configuration": self.plan["configuration"],
                "workload": self.plan["workload"],
                "server": {"command": command, "arguments": server, "environment": self.plan["environment"]},
            },
        )
        origin = capture_origin(self.directory) if self.plan["calibration"] else None
        begin = time.monotonic()
        with inference_server(command, self.directory / "server.log", self):
            startup_sec = time.monotonic() - begin
            for index, row in enumerate(requests):
                self.request(index, row)

        storage = sum(path.stat().st_size for path in self.directory.rglob("*") if path.is_file())
        facts = sorted((self.directory / "probe").glob("*.jsonl"))
        regions = sorted((self.directory / "regions").glob("*.jsonl"))
        phases = self.validate_observations(facts or regions) if facts or regions else []
        if self.plan.get("semantic", False):
            self.publish_manifest(facts, storage)
        elif self.plan["calibration"]:
            if not regions:
                raise ValueError("KT independent capture produced no measurements")
            write_json(
                self.directory / "region_measurements.json",
                {
                    "kind": "independent_kt_measurements",
                    "timing_scope": TIMING_SCOPE,
                    "status": "completed",
                    "profiling": False,
                    "capture_origin": origin,
                    "deployment": self.plan["deployment"],
                    "configuration": server,
                    "resource_envelope": resources,
                    "formal_begin_index": self.formal_begin_index,
                    "files": [str(path.relative_to(self.directory)) for path in regions],
                    "measurement_ports": ["runtime.kt.batch", "runtime.kt.placement"],
                    "phases": phases,
                    "temporary_storage_bytes": storage,
                    "startup_sec": startup_sec,
                },
            )
        else:
            write_json(
                self.directory / "normal_observation.json",
                {
                    "kind": "normal_http_observation",
                    "status": "completed",
                    "profiling": False,
                    "framework": "ktransformers",
                    "config_path": "config.json",
                    "workload_report": "workload_report.json",
                    "temporary_storage_bytes": storage,
                    "startup_sec": startup_sec,
                },
            )

    def validate_observations(self, files):
        expected = Counter({row["logical_request_id"]: 1 for row in self.rows})
        received, sent, batches = Counter(), Counter(), Counter()
        formal = {row["logical_request_id"] for row in self.rows if row["measure"]}
        phases, placements = set(), 0
        for path in files:
            with path.open() as stream:
                for line in stream:
                    event = json.loads(line)
                    fields = event["args"]
                    if fields.get("status", "returned") != "returned":
                        raise ValueError(f"KT observation failed: {path}: {event['name']}")
                    if event["name"] == "runtime.request.socket_received":
                        received.update(fields["request_ids"])
                    elif event["name"] == "runtime.response.scheduler_send":
                        sent.update(fields["request_ids"])
                    elif event["name"] == "runtime.kt.batch":
                        if fields.get("timing_scope") != TIMING_SCOPE:
                            raise ValueError("KT capture needs complete forward observations")
                        batches.update(fields["request_ids"])
                        if not fields["layers"]:
                            raise ValueError("KT base has an empty forward observation")
                        if formal.intersection(fields["request_ids"]):
                            phases.add(fields["phase"])
                    elif event["name"] == "runtime.kt.placement":
                        placements += 1
        if (
            received != expected
            or sent != expected
            or batches != Counter({row["logical_request_id"]: row["generated_tokens"] for row in self.rows})
        ):
            raise ValueError(
                f"KT request ports or forward counts are incomplete: received={received}, sent={sent}, batches={batches}"
            )
        if placements != 1:
            raise ValueError("KT capture requires one actual worker placement observation")
        return [phase for phase in ("prefill", "decode") if phase in phases]

    def publish_manifest(self, files, storage):
        write_json(
            self.directory / "profile_manifest.json",
            {
                "framework": "ktransformers",
                "status": "completed",
                "dry_run": False,
                "profiling_ready": False,
                "run_dir": str(self.directory),
                "config_path": "config.json",
                "scope": "Eager KT semantic regions and native routing; no detailed device-causality qualification",
                "temporary_storage_bytes": storage,
                "trace": {"torch_trace_files": [], "ld_preload_trace_files": []},
                "sidecar": {
                    "python_probe_files": [
                        {"path": str(path.relative_to(self.directory)), "format": "jsonl"} for path in files
                    ]
                },
                "bench": {"workload_report_files": [{"path": "workload_report.json"}]},
            },
        )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("collect", "server"))
    parser.add_argument("--input", type=Path, required=True)
    args = parser.parse_args()
    if args.action == "collect":
        Capture(args.input).run()
        return

    from sglang.launch_server import run_server
    from sglang.srt.plugins import load_plugins
    from sglang.srt.server_args import ServerArgs
    from sglang.srt.utils import kill_process_tree

    load_plugins()
    try:
        run_server(ServerArgs(**read_json(args.input)["server"]))
    finally:
        kill_process_tree(os.getpid(), include_parent=False)


if __name__ == "__main__":
    main()
