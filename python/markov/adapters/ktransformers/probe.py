"""Observe complete serial forwards and the routing already copied by the native worker."""

from collections import Counter
import os
from pathlib import Path
import time

from markov.adapters.sglang.transport_probe import TransportProbe
from markov.instrumentation.events import EventWriter


IMPLEMENTATION = "ktransformers.ascend.deepseek_v4.eager"
TIMING_SCOPE = "closed_forward_response"


class KtProbe:
    def __init__(self, writer):
        self.writer = writer
        self.active = None
        self.iterations = {}
        self.prompts = {}
        self.pending_moe = {}
        self.placement_recorded = False

    def record_placement(self):
        affinities = Counter()
        for path in Path("/proc/self/task").glob("*/status"):
            try:
                fields = dict(line.split(":", 1) for line in path.read_text().splitlines())
            except FileNotFoundError:
                continue
            affinities[
                (fields["Name"].strip(), fields["Cpus_allowed_list"].strip(), fields["Mems_allowed_list"].strip())
            ] += 1

        policies, pages = Counter(), Counter()
        for line in Path("/proc/self/numa_maps").read_text().splitlines():
            fields = line.split()
            policies[fields[1]] += 1
            for field in fields[2:]:
                name, _, value = field.partition("=")
                if name.startswith("N") and name[1:].isdigit():
                    pages[name] += int(value)
        now = time.time_ns()
        self.writer.write(
            "runtime.kt.placement",
            now,
            now,
            {
                "cpu_affinity_groups": [
                    {"thread_name": name, "cpus": cpus, "memory_nodes": nodes, "threads": count}
                    for (name, cpus, nodes), count in sorted(affinities.items())
                ],
                "numa_mapping_policies": dict(policies),
                "resident_pages_by_node": dict(pages),
                "qualification": "Observed once after the first forward; thread roles are not inferred and page counts are not a peak-memory measurement.",
            },
        )
        self.placement_recorded = True

    def batch(self, original, scheduler, batch, *args, **kwargs):
        if self.active is not None:
            raise ValueError("KT observation requires non-overlapping serial batches")
        identities = [row.rid for row in batch.reqs]
        if len(identities) != 1 or not (batch.forward_mode.is_extend() or batch.forward_mode.is_decode()):
            raise ValueError("KT observation supports one ordinary Prefill/Decode request per batch")
        identity = identities[0]
        iteration = self.iterations.get(identity, 0)
        self.iterations[identity] = iteration + 1
        phase = "decode" if batch.forward_mode.is_decode() else "prefill"
        query = batch.extend_num_tokens if phase == "prefill" else 1
        if iteration == 0:
            self.prompts[identity] = query
        fields = {
            "implementation": IMPLEMENTATION,
            "timing_scope": TIMING_SCOPE,
            "request_ids": identities,
            "iteration": iteration,
            "phase": phase,
            "query_tokens": query,
            "context_tokens": self.prompts[identity] + iteration,
            "layers": [],
            "probe_cpu_ns": 0,
        }
        begin = time.time_ns()
        self.active = fields
        try:
            result = original(scheduler, batch, *args, **kwargs)
            import torch

            # This supported path has no side stream or overlapping batch.
            # Close the whole response once, without adding events to every layer.
            torch.npu.current_stream().synchronize()
            end = time.time_ns()
            fields["status"] = "returned"
            self.writer.write("runtime.kt.batch", begin, end, fields)
            if not self.placement_recorded:
                self.record_placement()
            return result
        finally:
            self.active = None
            self.pending_moe.clear()

    def submit(self, original, method, dispatcher, hidden_states, topk_output):
        if self.active is None:
            return original(method, dispatcher, hidden_states, topk_output)
        layer = method.kt_config.layer_idx
        fields = {
            "cpu_threads": method.kt_config.cpuinfer_threads,
            "numa_pools": method.kt_config.threadpool_count,
            "resident_experts": method.num_gpu_experts,
            "experts": method.wrapper.num_experts,
            "top_k": method.wrapper.num_experts_per_tok,
            "hidden_size": hidden_states.shape[-1],
        }
        result = original(method, dispatcher, hidden_states, topk_output)
        self.pending_moe[layer] = fields
        return result

    def combine(self, original, method, dispatcher, hidden_states):
        if self.active is None:
            return original(method, dispatcher, hidden_states)
        layer = method.kt_config.layer_idx
        fields = self.pending_moe.pop(layer)
        result = original(method, dispatcher, hidden_states)

        # The eager native submit has already synchronized this pinned ID copy.
        # Reading it avoids another NPU copy or a tensor snapshot.
        begin = time.time_ns()
        from kt_kernel.experts_base import KExpertsCPUBuffer

        query = self.active["query_tokens"]
        buffers = KExpertsCPUBuffer.get_buffer(hidden_states.view(-1, hidden_states.shape[-1]), fields["top_k"])
        slot = layer % KExpertsCPUBuffer.buffer_depth
        ids = buffers[1][slot][:query].reshape(-1)
        counts = Counter(ids.tolist())
        if ids.numel() != query * fields["top_k"] or any(index < 0 or index >= fields["experts"] for index in counts):
            raise ValueError("KT native routing IDs are incomplete or deferred; this path is not supported")
        fields["expert_counts"] = [counts.get(index, 0) for index in range(fields["experts"])]
        fields["probe_cpu_ns"] = time.time_ns() - begin
        self.active["probe_cpu_ns"] += fields["probe_cpu_ns"]
        self.active["layers"].append({"layer": layer, "moe": fields})
        return result

    def install(self):
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        TransportProbe(self.writer).install()
        for target, wrapper in (
            ("sglang.srt.managers.scheduler.Scheduler.run_batch", self.batch),
            ("sglang.srt.layers.moe.kt_ep_wrapper.KTEPWrapperMethod._ascend_pre_dispatch", self.submit),
            ("sglang.srt.layers.moe.kt_ep_wrapper.KTEPWrapperMethod._ascend_post_combine", self.combine),
        ):
            HookRegistry.register(target, wrapper, HookType.AROUND)


def install():
    output = os.environ.get("MARKOV_KT_FACTS") or os.environ.get("MARKOV_KT_MEASUREMENTS")
    if output:
        KtProbe(EventWriter(Path(output))).install()
