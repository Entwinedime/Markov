"""Construct finite request-lifecycle probes without inventing expert choices."""

import json
from math import lcm

from markov_workload import WorkloadContent
from markov_workload.expert_routes import ExpertRoutes


class LifecycleProbes:
    def __init__(self, instances, topologies):
        self.page_size = instances[0].configuration.values["page_size"]
        self.chunks = {instance.configuration.values["prefill_chunk_tokens"] for instance in instances}
        self.prefix_cache = any(instance.configuration.values["prefix_cache"] for instance in instances)
        self.topologies = topologies
        self.prompt_period = lcm(self.page_size, *(chunk for chunk in self.chunks if chunk))

    def normalize(self, workload):
        content = WorkloadContent(workload["requests"], workload["prefixes"])
        requests = content.requests
        limits = {}
        for identity, request in requests.items():
            prompt = request["prompt_tokens"]
            prefix = content.lengths[request.get("prefix_id")]
            minimum = max(prefix + (prompt > prefix), self.page_size + 1 if prompt > self.page_size else 1)
            minimum = max([minimum, *(chunk + 1 for chunk in self.chunks if 0 < chunk < prompt)])
            output = request["output_tokens"]
            decode_minimum = min(output, 3)

            # The prompt consumes its last token before Decode; crossing its page needs one more input token.
            first_new_page = (-prompt) % self.page_size + 2
            if output >= first_new_page:
                decode_minimum = max(decode_minimum, first_new_page)
            limits[identity] = {"prompt_tokens": minimum, "output_tokens": decode_minimum}

        # Keep interval boundaries visible, including gaps or overlaps that native validation must still reject.
        for topology in self.topologies:
            for entry in workload["ep_routes"][topology]["assignments"]:
                request = requests[entry["request"]]
                prompt = request["prompt_tokens"]
                total = prompt + request["output_tokens"] - 1
                for position in ExpertRoutes.interval(entry["tokens"], total, "tokens"):
                    if position == total:
                        continue
                    field = "output_tokens" if position >= prompt else "prompt_tokens"
                    minimum = position - prompt + 2 if position >= prompt else position + 1
                    limits[entry["request"]][field] = max(limits[entry["request"]][field], minimum)

        cohorts = {"prompt_tokens": {}, "output_tokens": {}}
        for identity, request in requests.items():
            for field, lengths in cohorts.items():
                length = request[field]
                lengths[length] = max(lengths.get(length, 0), limits[identity][field])

        # Preserve page/chunk remainders and equal-length cohorts; Decode also keeps separated output-length cohorts.
        prompts = self.shorten(cohorts["prompt_tokens"], self.prompt_period, 1)
        outputs = self.shorten(cohorts["output_tokens"], self.page_size, 2)
        normalized = {
            key: {**request, "prompt_tokens": prompts[request["prompt_tokens"]],
                  "output_tokens": outputs[request["output_tokens"]]}
            for key, request in requests.items()
        }
        return self.resize(workload, normalized)

    def resize(self, workload, requests):
        original = {request["id"]: request for request in workload["requests"]}
        result = {**workload, "requests": list(requests.values())}
        result.pop("ep_routes", None)

        # Keep original expert choices for retained tokens; shorter Prefill moves Decode's origin.
        routes = {}
        for topology in self.topologies:
            source = workload["ep_routes"][topology]
            assignments = []
            for entry in source["assignments"]:
                identity = entry["request"]
                if identity not in requests:
                    continue
                before, after = original[identity], requests[identity]
                tokens = []
                for position in entry["tokens"]:
                    if position < before["prompt_tokens"]:
                        tokens.append(min(position, after["prompt_tokens"]))
                    else:
                        tokens.append(after["prompt_tokens"] + min(position - before["prompt_tokens"], after["output_tokens"] - 1))
                if tokens[0] < tokens[1]:
                    assignments.append({**entry, "tokens": tokens})
            routes[topology] = {**source, "assignments": assignments}
        if routes:
            result["ep_routes"] = routes
        return result

    @staticmethod
    def shorten(cohorts, period, separation):
        result = {}
        previous, original = 0, 0
        for length, minimum in sorted(cohorts.items()):
            minimum = max(minimum, previous + min(length - original, separation))
            previous = minimum + (length - minimum) % period
            result[length] = previous
            original = length
        return result

    @staticmethod
    def shared_prefix_requests(content, page_size):
        owners = {}
        for request in content.requests.values():
            prefix = request.get("prefix_id")
            while prefix is not None:
                if content.lengths[prefix] >= page_size:
                    owners.setdefault(prefix, []).append(request["id"])
                prefix = content.prefixes[prefix].get("parent")
        shared = {identity for identities in owners.values() if len(identities) > 1 for identity in identities}

        # A producer can cache its entire prompt; a consumer must leave its last token for model execution.
        return [request for identity, request in content.requests.items() if identity in shared and request["prompt_tokens"] > page_size]

    def extend(self, workload, groups, *, model_phases):
        requests = {request["id"]: request for request in workload["requests"]}
        decodable = [request for request in requests.values() if request["output_tokens"] > 1]
        if "decode" in model_phases:
            source = min(decodable, key=lambda request: (request["output_tokens"] == 2,
                         request["prompt_tokens"] + request["output_tokens"], request["id"]))

            # Short prompts leave room for joint admission before both requests enter Decode.
            self.append(workload, groups, source, "decode_pair", copies=2, prompt_tokens=1)
        if self.prefix_cache:
            content = WorkloadContent(workload["requests"], workload["prefixes"])
            candidates = self.shared_prefix_requests(content, self.page_size)
            if candidates:
                source = min(candidates, key=lambda request: (
                    request["output_tokens"] < 3, request["prompt_tokens"] + request["output_tokens"], request["id"],
                ))

                # Consecutive singleton groups let native completion publish the prefix before its next use.
                if groups[-1] != [source["id"]]:
                    self.append(workload, groups, source, "cache_seed")
                self.append(workload, groups, source, "cache_reuse")
                requests = {request["id"]: request for request in workload["requests"]}
        minimum_output = 2 if "decode" in model_phases else 3
        candidates = [request for request in requests.values() if request["output_tokens"] >= minimum_output]
        if not candidates and model_phases:
            candidates = list(requests.values())
        identities = {request["id"] for request in candidates}
        if not candidates or any(len(group) == 1 and group[0] in identities for group in groups):
            return

        source = min(candidates, key=lambda request: (request["prompt_tokens"] + request["output_tokens"], request["id"]))

        # The same singleton supplies degenerate model geometry and, when present, Decode continuation.
        self.append(workload, groups, source, "isolated")

    def append(self, workload, groups, source, purpose, *, copies=1, prompt_tokens=None):
        request = dict(source)
        if prompt_tokens is not None:
            request["prompt_tokens"] = prompt_tokens
            request.pop("prefix_id", None)
        seed = self.resize(workload, {source["id"]: request})

        identities = []
        for index in range(copies):
            identity = json.dumps(["probe", purpose, index, source["id"]], ensure_ascii=False, separators=(",", ":"))
            workload["requests"].append({**request, "id": identity, "arrival_ns": 0})
            identities.append(identity)
        groups.append(identities)
        for topology, route in seed.get("ep_routes", {}).items():
            workload["ep_routes"][topology]["assignments"].extend(
                {**entry, "request": identity} for identity in identities for entry in route["assignments"]
            )
