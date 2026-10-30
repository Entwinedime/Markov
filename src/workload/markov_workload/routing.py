"""Conditional cache identities without changing the model's input tokens."""

from array import array
from collections import defaultdict
from itertools import pairwise

from markov_workload import WorkloadContent


class RoutedContent:
    def __init__(self, content: WorkloadContent, assignments: list[dict]) -> None:
        changes = {identity: defaultdict(dict) for identity in content.requests}
        for assignment in assignments:
            begin, _ = assignment["tokens"]
            for layer in range(*assignment["layers"]):
                changes[assignment["request"]][begin][layer] = tuple(assignment["experts"])

        signatures = {}
        self.segments = {}
        for identity in sorted(content.requests):
            request = content.requests[identity]
            length = request["prompt_tokens"] + request["output_tokens"] - 1
            events = changes[identity]
            events[length] = {}
            positions = sorted(events)
            active = {}
            segments = []
            for begin, end in pairwise(positions):
                active.update(events[begin])

                # Equal conditions have the same identity even when input intervals are split differently.
                signature = tuple(sorted(active.items()))
                symbol = signatures.setdefault(signature, len(signatures))
                segments.append((end, symbol * content.vocabulary))
            self.segments[identity] = segments

    def key(self, identity: str, tokens) -> array:
        segments = self.segments[identity]
        if len(tokens) > segments[-1][0]:
            raise ValueError(f"cache key exceeds supplied route coverage for {identity}")

        # Negative symbols separate conditional keys from ordinary native warmup tokens.
        # A radix path retains earlier differences, so later equal routes cannot rejoin it.
        result = array("q")
        begin = 0
        for end, offset in segments:
            result.extend(-offset - token - 1 for token in tokens[begin : min(end, len(tokens))])
            if end >= len(tokens):
                break
            begin = end
        return result
