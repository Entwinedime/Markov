"""Pair scoped native host calls using the evidence required by each work interval."""

from collections import defaultdict
import json


class NativeHostCalls:
    batch_roles = {"run_batch", "launch", "output_prepare", "output_skip"}
    local_batch_axes = {"batch.members", "batch.query_tokens"}

    def __init__(self, scopes, excluded=()):
        self.scopes = scopes
        self.calls = {0: None}
        self.children = defaultdict(list)
        self.batch_of = {0: None}
        self.batches = defaultdict(list)
        self.dimensions = {0: {}}
        for scope in scopes[1:]:
            identity, parent = scope["id"], scope["parent"]
            if parent not in self.calls or identity in excluded or "service" in scope:
                continue
            self.calls[identity] = self.identity(scope)
            self.children[parent].append(identity)
            self.batch_of[identity] = self.batch_of[parent]
            dimensions = self.dimensions[parent]
            self.dimensions[identity] = dimensions | scope["dimensions"] if "dimensions" in scope else dimensions
            if scope["role"] == "run_batch":
                self.batch_of[identity] = identity
                self.batches[self.batch_key(scope)].append(identity)

    @staticmethod
    def batch_key(scope):
        work = scope["work"]
        members = tuple((row["id"], row["new_tokens"], row["phase"]) for row in work["members"])
        return work["phase"], members

    @classmethod
    def identity(cls, scope):
        role, work = scope["role"], scope["work"]
        detail = None
        if role in cls.batch_roles:
            detail = [work["phase"], [{key: value for key, value in member.items() if key != "slot"} for member in work["members"]]]
        elif role == "segment":
            detail = [work["continuation"], scope["bindings"]["condition"]]
        elif role in {"invocation", "query", "host_read", "batch_transform", "cache_match", "cache_insert", "kv_reservation"}:
            detail = work.get("context")
        elif role == "metadata_collective":
            detail = [scope["parameters"], scope["bindings"]["condition"], work["context"].get("poll_requests")]
        elif role in {"transfer_send", "transfer_receive"}:
            detail = [work["context"]["message"], scope["bindings"]["dtype"]]
        elif role in {"transfer_send_wait", "transfer_receive_wait", "signal_wait"}:
            context = work["context"]
            producer = context["producer"]
            detail = [context["device_wait"], context.get("wait_stream"), producer.get("record_stream"), producer["message"]]
        elif role == "stream_wait":
            detail = work["context"]["streams"]
        elif role == "signal_record":
            detail = work["context"]["record_stream"]
        elif role == "gather":
            detail = scope["bindings"].get("collective")
        return role, scope["implementation"], scope["site"], json.dumps(detail, sort_keys=True)

    def index(self, paths, typed):
        routes = {0: 0}
        indexed = defaultdict(list)
        for identity, call in self.calls.items():
            if identity == 0:
                continue
            scope = self.scopes[identity]
            parent = scope["parent"]

            # Exact matches retain the enclosing batch context as part of the call route.
            component = call if scope["role"] in self.batch_roles else call[:3]
            route = routes[identity] = paths.setdefault((routes[parent], component), len(paths) + 1)
            arguments = json.dumps(scope.get("parameters"), sort_keys=True) if call[:3] in typed else self.calls[parent]
            indexed[route, call, arguments, self.child_keys(identity)].append(identity)
        return indexed

    def child_keys(self, identity):
        return tuple(self.calls[child] for child in self.children[identity])

    def exact_pairs(self, observed):
        paths = {}
        typed = {(scope["role"], scope["implementation"], scope["site"])
                 for scope in observed.scopes if "parameters" in scope}
        measured = observed.index(paths, typed)
        pairs = {}
        unmatched = 0
        for call, previous in self.index(paths, typed).items():
            timed = measured.get(call, ())

            # Polling can change multiplicity. Each paired call receives a separate sample.
            pairs.update(zip(previous, timed, strict=False))
            unmatched += max(0, len(previous) - len(timed))
        return pairs, unmatched

    def local_pairs(self, observed, exact):
        used_source = {self.batch_of[scope] for scope in exact}
        used_target = {observed.batch_of[scope] for scope in exact.values()}
        pairs = {}
        for key, source in self.batches.items():
            targets = observed.batches.get(key, ())
            left = (scope for scope in source if scope not in used_source)
            right = (scope for scope in targets if scope not in used_target)
            for original, timed in zip(left, right, strict=False):
                self.pair_subtree(observed, original, timed, pairs)
        return pairs

    def pair_subtree(self, observed, original, timed, pairs):
        work = [(original, timed)]
        for source, target in work:
            source_call, target_call = self.calls[source], observed.calls[target]
            if (source_call[:3] != target_call[:3] or (source != original and source_call != target_call)
                    or self.child_keys(source) != observed.child_keys(target)):
                continue
            measured = observed.scopes[target]
            if "parameters" in measured and measured["parameters"] != self.scopes[source].get("parameters"):
                continue
            pairs[source] = target
            work.extend(zip(self.children[source], observed.children[target], strict=True))

    def supports_local_work(self, scope, features, observed, aligned_scopes):
        sources = scope["bindings"]["source_scopes"]
        if len(sources) != 1 or scope["kv_accesses"] or scope["slot_accesses"]:
            return False
        source, = sources
        if source not in aligned_scopes:
            return False
        batch = self.scopes[self.batch_of[source]]
        axes = features["axes"]
        if features["phase"] != batch["work"]["phase"] or not self.local_batch_axes <= axes.keys():
            return False

        # Native dimensions supplement the query geometry without importing parameters from the detailed run.
        measured = observed.dimensions[aligned_scopes[source]]
        for name, value in axes.items():
            if name in self.local_batch_axes:
                continue
            if not name.startswith("parameter.") or measured.get(name.removeprefix("parameter.")) != value:
                return False
        return True
