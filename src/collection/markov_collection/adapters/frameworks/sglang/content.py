"""Supply synthetic output content without replacing native sampling work."""

from contextlib import nullcontext

import torch
from markov_workload import WorkloadContent
from markov_workload.routing import RoutedContent


class NativeContent:
    def __init__(self, description, auxiliary=nullcontext) -> None:
        source = description["content"]
        self.content = WorkloadContent(source["requests"], source["prefixes"])
        self.aliases = {
            native: request["id"]
            for native, request in zip(description["source_requests"], source["requests"], strict=True)
        }
        self.execution_aliases = self.aliases
        if description["representative_warmup"]:
            warmup = {native: request["id"] for native, request in zip(description["warmup_requests"], source["requests"], strict=True)}
            self.execution_aliases = self.aliases | warmup
        self.auxiliary = auxiliary
        self.tokens = None
        self.routing = None
        self.route_assignments = None
        serving = description["serving_input"]
        if "ep" in serving["configuration"]:
            settings = serving["configuration"]
            if settings.get("prefix_cache", False):
                routes = serving["ep_routes"][settings["ep"]["id"]]
                self.route_assignments = routes["assignments"]

    def configure(self, workers) -> None:
        if self.route_assignments is not None:

            # Cache identity covers the whole model, including expert layers on other PP stages.
            layers = {layer["layer"] for worker in workers for layer in worker["expert_layers"]}
            self.routing = RoutedContent(self.content, self.route_assignments, layers)

    def request(self, original, req, *args, **kwargs):
        original(req, *args, **kwargs)
        if req.rid in self.execution_aliases:
            req.extra_key = self.execution_aliases[req.rid]

    def cache_key(self, original, key, token_ids, extra_key=None, is_bigram=False, limit=None):
        if extra_key is not None:
            token_ids = self.routing.key(extra_key, token_ids if limit is None else token_ids[:limit])
            extra_key = None
        original(key, token_ids, extra_key, is_bigram, limit)

    def run_batch(self, original, scheduler, batch, *args, **kwargs):
        previous = self.tokens
        aliases = self.execution_aliases
        self.tokens = [self.content.output_token(aliases[req.rid]) for req in batch.reqs] if batch.reqs and batch.reqs[0].rid in aliases else None
        try:
            return original(scheduler, batch, *args, **kwargs)
        finally:
            self.tokens = previous

    def sample(self, original, runner, *args, **kwargs):
        result = original(runner, *args, **kwargs)
        if self.tokens is not None:

            # Keep the sampler's tensor identity and work; injected values are workload setup, not model work.
            with self.auxiliary():
                result.copy_(torch.tensor(self.tokens, dtype=result.dtype, device=result.device))
        return result

    def install(self) -> None:
        from sglang.srt.plugins.hook_registry import HookRegistry, HookType

        HookRegistry.register("sglang.srt.managers.scheduler.Scheduler.run_batch", self.run_batch, HookType.AROUND)
        HookRegistry.register("sglang.srt.model_executor.model_runner.ModelRunner.sample", self.sample, HookType.AROUND)
        if self.route_assignments is not None:
            HookRegistry.register("sglang.srt.managers.schedule_batch.Req.__init__", self.request, HookType.AROUND)
            HookRegistry.register("sglang.srt.mem_cache.radix_cache.RadixKey.__init__", self.cache_key, HookType.AROUND)
