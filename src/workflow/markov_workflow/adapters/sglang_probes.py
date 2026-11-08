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
        result = {**workload, "requests": list(normalized.values())}
        result.pop("ep_routes", None)

        # Only remove homogeneous tails; shift the original route intervals without inventing expert assignments.
        routes = {}
        for topology in self.topologies:
            source = workload["ep_routes"][topology]
            assignments = []
            for entry in source["assignments"]:
                before, after = requests[entry["request"]], normalized[entry["request"]]
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

    def extend(self, workload, groups):
        requests = {request["id"]: request for request in workload["requests"]}
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
        if any(len(group) == 1 and requests[group[0]]["output_tokens"] >= 3 for group in groups):
            return
        candidates = [request for request in requests.values() if request["output_tokens"] >= 3]
        if not candidates:
            return

        source = min(candidates, key=lambda request: (request["prompt_tokens"] + request["output_tokens"], request["id"]))

        # A lone request exercises continuation and completion without another request finishing on every iteration.
        self.append(workload, groups, source, "isolated")

    @staticmethod
    def append(workload, groups, source, purpose):
        identity = json.dumps(["probe", purpose, source["id"]], ensure_ascii=False, separators=(",", ":"))
        workload["requests"].append({**source, "id": identity, "arrival_ns": 0})
        groups.append([identity])
        for route in workload.get("ep_routes", {}).values():
            route["assignments"].extend([{**entry, "request": identity} for entry in route["assignments"] if entry["request"] == source["id"]])
