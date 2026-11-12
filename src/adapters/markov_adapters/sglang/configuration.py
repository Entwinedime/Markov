"""Shared serving inputs for native observation and target task assembly."""

from __future__ import annotations

import re
from dataclasses import dataclass


class NativeConfiguration:
    @staticmethod
    def upstream_revision(value: str | None) -> str:
        if not isinstance(value, str) or re.fullmatch("[0-9a-f]{40}", value) is None:
            raise ValueError("SGLang image requires its actual upstream Git revision metadata")
        return value

    def __init__(self, source: dict) -> None:
        allowed = {
            "id", "tp", "pp", "ep", "attention_backend", "page_size", "max_running_requests", "max_total_tokens",
            "max_prefill_tokens", "prefill_chunk_tokens", "mixed_batches", "prefix_cache", "microbatch_requests",
        }
        if source.keys() - allowed:
            raise ValueError("native configuration contains unsupported fields")
        self.values = {"pp": 1, "prefill_chunk_tokens": 0, "mixed_batches": False, "prefix_cache": False, **source}
        positive = ["tp", "pp", "page_size", "max_running_requests", "max_total_tokens", "max_prefill_tokens"]
        if "microbatch_requests" in self.values:
            positive.append("microbatch_requests")
        for name in positive:
            value = self.values[name]
            if type(value) is not int or not 0 < value < 1 << 63:
                raise ValueError(f"native {name} must be a positive signed 64-bit integer")
        chunk = self.values["prefill_chunk_tokens"]
        if type(chunk) is not int or not 0 <= chunk < 1 << 63:
            raise ValueError("native prefill_chunk_tokens must be a nonnegative signed 64-bit integer")
        for name in ("mixed_batches", "prefix_cache"):
            if type(self.values[name]) is not bool:
                raise TypeError(f"native {name} must be a boolean")
        if self.values["mixed_batches"] and not chunk:
            raise ValueError("mixed batches require chunked prefill")
        if not isinstance(self.values["attention_backend"], str) or not self.values["attention_backend"]:
            raise ValueError("native attention_backend must be a nonempty string")

        self.tp, self.pp = self.values["tp"], self.values["pp"]
        self.ep = 1
        if "ep" in self.values:
            expert = self.values["ep"]
            if expert.keys() != {"id", "size"} or not isinstance(expert["id"], str) or not expert["id"]:
                raise ValueError("an EP topology needs a nonempty id and size")
            self.ep = expert["size"]
            if type(self.ep) is not int or not 0 < self.ep < 1 << 63:
                raise ValueError("native EP size must be a positive signed 64-bit integer")

    @staticmethod
    def batch_capacity(configuration: dict) -> int:
        slots = configuration["max_running_requests"]

        # Scheduler initializes the default per PP stage; admission also checks the shared request pool.
        microbatch = configuration.get("microbatch_requests", max(slots // configuration.get("pp", 1), 1))
        return min(slots, microbatch)

    def routes(self, workload: dict):
        if "ep" not in self.values:
            return None
        return workload["ep_routes"][self.values["ep"]["id"]]

    def structure(self) -> dict:

        # Capacities and chunk length are target parameters; changing the native mechanism needs another source.
        return {
            "tp": self.tp, "pp": self.pp, "ep": self.ep,
            "attention_backend": self.values["attention_backend"], "page_size": self.values["page_size"],
            "prefix_cache": self.values["prefix_cache"],
            "chunked_prefill": self.values["prefill_chunk_tokens"] > 0,
            "mixed_batches": self.values["mixed_batches"],
        }

    def settings(self) -> dict:
        return {name: self.values[name] for name in (
            "max_running_requests", "max_total_tokens", "max_prefill_tokens", "page_size", "prefill_chunk_tokens",
            "microbatch_requests",
        ) if name in self.values}

    def control(self) -> dict:
        return {
            "mixed_batches": self.values["mixed_batches"], "disable_radix_cache": not self.values["prefix_cache"],
            "moe_a2a_backend": "deepep" if self.ep > 1 else "none", "deepep_mode": "normal" if self.ep > 1 else "auto",
        }


@dataclass
class InstanceConfiguration:
    identity: str
    mode: str
    configuration: NativeConfiguration


class ServingInput:
    """Normalize model, workload and instances; each entrypoint owns its execution options."""

    def __init__(self, source: dict) -> None:
        self.model = source["model"]
        if self.model.keys() != {"path", "dtype"}:
            raise ValueError("serving model needs path and dtype")
        self.instances = []
        identities = set()
        for description in source["instances"]:
            if description.keys() != {"id", "mode", "configuration"}:
                raise ValueError("a serving instance needs id, mode and configuration")
            identity = description["id"]
            if not isinstance(identity, str) or not identity or identity in identities:
                raise ValueError("serving instance ids must be unique nonempty strings")
            identities.add(identity)
            configuration = NativeConfiguration(description["configuration"])
            self.instances.append(InstanceConfiguration(identity, description["mode"], configuration))

        modes = [instance.mode for instance in self.instances]
        if modes != ["null"] and sorted(modes) != ["decode", "prefill"]:
            raise ValueError("serving requires one ordinary instance or one prefill/decode pair")
        if len({instance.configuration.values["page_size"] for instance in self.instances}) != 1:
            raise ValueError("native PD transfer requires equal page sizes")
        self.workload = self.workload_input(source["workload"])
        for instance in self.instances:
            instance.configuration.routes(self.workload)

    @staticmethod
    def workload_input(source: dict) -> dict:
        workload = dict(source)
        if workload.keys() - {"id", "requests", "prefixes", "ep_routes"}:
            raise ValueError("serving workload contains unsupported fields")
        workload.setdefault("prefixes", [])
        requests = workload["requests"]
        if not isinstance(requests, list) or not requests:
            raise ValueError("serving needs a nonempty request array")
        identities = set()
        for request in requests:
            if request.keys() - {"id", "arrival_ns", "prompt_tokens", "output_tokens", "prefix_id"}:
                raise ValueError("serving request contains unsupported fields")
            identity = request["id"]
            if not isinstance(identity, str) or not identity or identity in identities:
                raise ValueError("request IDs must be unique nonempty strings")
            identities.add(identity)
            for name in ("arrival_ns", "prompt_tokens", "output_tokens"):
                minimum = 0 if name == "arrival_ns" else 1
                if type(request[name]) is not int or not minimum <= request[name] < 1 << 63:
                    raise ValueError(f"invalid signed 64-bit request {name}: {identity}")
            if request["prompt_tokens"] + request["output_tokens"] - 1 >= 1 << 63:
                raise ValueError(f"request token length overflows: {identity}")
        return workload


class CaptureInput(ServingInput):
    def __init__(self, source: dict) -> None:
        required = {"model", "instances", "workload", "observe_all_workers"}
        if not required <= source.keys() or source.keys() - required - {"submission_groups"}:
            raise ValueError("a native job needs one model, explicit instances, workload and observation role policy")
        if type(source["observe_all_workers"]) is not bool:
            raise TypeError("observation role policy must be a boolean")
        super().__init__(source)
        self.observe_all_workers = source["observe_all_workers"]
        identities = [request["id"] for request in self.workload["requests"]]
        self.submission_groups = source.get("submission_groups", [identities])
        if not isinstance(self.submission_groups, list) or not self.submission_groups or any(
            not isinstance(group, list) or not group or any(not isinstance(identity, str) for identity in group)
            for group in self.submission_groups
        ):
            raise ValueError("capture submission groups must be nonempty request-ID arrays")
        members = [identity for group in self.submission_groups for identity in group]
        if len(members) != len(identities) or set(members) != set(identities):
            raise ValueError("capture submission groups must partition the workload requests")
