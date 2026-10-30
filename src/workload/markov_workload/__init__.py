"""Deterministic synthetic content shared by collection and native control."""

from collections import defaultdict, deque
from collections.abc import Sequence


class WorkloadContent:
    def __init__(self, requests: Sequence[dict], prefixes: Sequence[dict] = ()) -> None:
        self.requests = {request["id"]: request for request in requests}
        if len(self.requests) != len(requests) or any(
            not isinstance(identity, str) or not identity for identity in self.requests
        ):
            raise ValueError("content request IDs must be nonempty strings and unique")
        self.prefixes = {}
        children = defaultdict(list)
        for prefix in prefixes:
            if prefix.keys() - {"id", "parent", "tokens"}:
                raise ValueError("prefix fields are id, parent and tokens")
            identity = prefix["id"]
            if not isinstance(identity, str) or not identity or identity in self.prefixes:
                raise ValueError("prefix IDs must be nonempty strings and unique")
            if type(prefix["tokens"]) is not int or prefix["tokens"] <= 0:
                raise ValueError("prefix tokens must be a positive integer")
            parent = prefix.get("parent")
            if parent is not None and (not isinstance(parent, str) or not parent):
                raise ValueError("prefix parent must be a nonempty ID")
            self.prefixes[identity] = prefix
            children[parent].append(identity)

        for parent in children:
            if parent is not None and parent not in self.prefixes:
                raise ValueError(f"unknown prefix parent: {parent}")

        # Assign distinct tokens only where paths diverge, rather than hashing IDs into a vocabulary.
        self.prefix_tokens = {}
        self.private_tokens = {}
        branches = defaultdict(list)
        for identity, prefix in self.prefixes.items():
            branches[prefix.get("parent")].append((0, identity))
        for identity, request in self.requests.items():
            parent = request.get("prefix_id")
            if parent is not None and (not isinstance(parent, str) or parent not in self.prefixes):
                raise ValueError(f"unknown request prefix: {parent}")
            branches[parent].append((1, identity))
        for siblings in branches.values():
            for token, (kind, identity) in enumerate(sorted(siblings)):
                mapping = self.prefix_tokens if kind == 0 else self.private_tokens
                mapping[identity] = token
        self.vocabulary = max((len(siblings) for siblings in branches.values()), default=0)

        self.lengths = {None: 0}
        pending = deque(children[None])
        while pending:
            identity = pending.popleft()
            prefix = self.prefixes[identity]
            self.lengths[identity] = self.lengths[prefix.get("parent")] + prefix["tokens"]
            pending.extend(children[identity])
        if len(self.lengths) != len(self.prefixes) + 1:
            raise ValueError("prefix parent relationships contain a cycle")
        for request in requests:
            if self.lengths[request.get("prefix_id")] > request["prompt_tokens"]:
                raise ValueError(f"shared prefix exceeds request prompt: {request['id']}")

    def check_vocabulary(self, size: int) -> None:
        if self.vocabulary > size:
            raise ValueError(f"content needs {self.vocabulary} distinct sibling tokens, model vocabulary has {size}")

    def prompt(self, identity: str) -> list[int]:
        request = self.requests[identity]
        path = []
        parent = request.get("prefix_id")
        while parent is not None:
            prefix = self.prefixes[parent]
            path.append((self.prefix_tokens[parent], prefix["tokens"]))
            parent = prefix.get("parent")

        tokens = [token for token, length in reversed(path) for _ in range(length)]
        tokens.extend([self.output_token(identity)] * (request["prompt_tokens"] - len(tokens)))
        return tokens

    def output_token(self, identity: str) -> int:

        # A private continuation also separates requests whose entire prompt is shared.
        return self.private_tokens[identity]
