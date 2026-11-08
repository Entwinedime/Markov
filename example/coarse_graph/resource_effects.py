"""Compare observed resource effects; never rerun allocation or cache policy to invent answers."""

from collections import Counter
from itertools import chain


class FieldEffects:
    """Compare row ownership without substituting request slots or replaying allocator policy."""

    def __init__(self):
        self.free = {}
        self.bindings = {}
        self.names = {}
        self.operations = []
        self.capacities = {}

    def declare(self, field, capacity, free):
        slots = set(free)
        if (type(capacity) is not int or capacity <= 0 or field in self.free
                or len(slots) != len(free) or not slots <= set(range(capacity))):
            raise ValueError("invalid observed slot pool declaration")
        self.capacities[field] = capacity
        self.free[field] = slots

    def apply(self, record):
        field, operation, slot = record["field"], record["operation"], record["slot"]
        free = self.free[field]
        if operation == "allocate" and slot is None:
            if free:
                raise ValueError("slot allocation failed with available rows")
            self.operations.append(record)
            return
        if not isinstance(slot, int) or not 0 <= slot < self.capacities[field]:
            raise ValueError("slot operation lies outside its physical pool")
        key = field, slot
        if operation == "allocate":
            free.remove(slot)
        elif slot in free:
            raise ValueError("slot operation accesses a free row")

        # Physical aliases persist across release: reuse must not disappear under renaming.
        identity = self.names.setdefault(key, sum(name[0] == field for name in self.names))
        value = {**record, "slot": identity}
        if operation == "bind":
            request = record["request"]
            if key in self.bindings and self.bindings[key] != request:
                raise ValueError("slot binding overwrites another live request")
            if any(owner == request and address[0] == field and address != key for address, owner in self.bindings.items()):
                raise ValueError("request owns multiple rows in one slot pool")
            self.bindings[key] = request
        elif operation == "release":
            self.bindings.pop(key, None)
            free.add(slot)
        elif operation != "allocate":
            raise ValueError("unknown observed slot operation")
        self.operations.append(value)


class ResourceEffects:
    def __init__(self, page_size):
        self.page_size = page_size
        self.pages, self.slots, self.nodes = {}, {}, {}
        self.operations = []
        self.fields = FieldEffects()

    @staticmethod
    def name(names, identity):
        return names.setdefault(identity, len(names))

    def page(self, page):

        # Renaming hides absolute addresses, not physical reuse across allocation lifetimes.
        return self.name(self.pages, page)

    def slot(self, slot):
        return self.name(self.slots, slot)

    def ranges(self, indices):
        ranges = []
        for index in indices:
            page, offset = divmod(index, self.page_size)
            identity = self.page(page)
            if ranges and ranges[-1][0] == identity and ranges[-1][2] == offset:
                ranges[-1][2] += 1
            else:
                ranges.append([identity, offset, offset + 1])
        return ranges

    def apply(self, operation):
        kind = operation["kind"]
        if kind == "slots":
            operation["slots"] = [self.slot(slot) for slot in operation["slots"]]
        elif kind == "allocate":
            operation["ranges"] = self.ranges(operation.pop("indices"))
        elif kind == "release_request":
            slot = operation["slot"]
            operation["slot"] = self.slot(slot)
        elif kind == "release_pages":
            pages = operation["pages"]
            if not pages:
                return

            # Release order within one native free call is immaterial; grouping and alias identity are not.
            operation["pages"] = sorted(self.page(page) for page in pages)
        elif kind == "prefix":
            if "node" in operation:
                operation["node"] = self.name(self.nodes, operation["node"])
            if "indices" in operation:
                operation["ranges"] = self.ranges(operation.pop("indices"))
            elif "pages" in operation:
                operation["ranges"] = [[self.page(page), 0, self.page_size] for page in operation.pop("pages")]
        self.operations.append(operation)

    def native(self, history, scopes, cache_disabled):
        for field, pool in history["initial"].get("state_slots", {}).items():
            self.fields.declare(field, pool["capacity"], pool["free"])
        for record in history.get("state_events", []):
            self.fields.apply(record)
        for field, free in history.get("state_free", {}).items():
            if self.fields.free[field] != set(free) or len(free) != len(set(free)):
                raise ValueError("native slot operations disagree with final free rows")

        for record in history["events"]:
            observed = record["operation"]
            kind = observed["kind"]
            if kind == "prefix" and cache_disabled:

                # Disabled native cache methods are no-ops; the control view makes no core prefix call for them.
                continue
            operation = {"kind": kind}
            if kind == "slots":
                operation.update(requests=observed["requests"], slots=record["result"])
            elif kind == "allocate":
                members = scopes[record["scope"]]["work"]["members"]
                requests = [[member["id"], member["new_tokens"]] for member in members]
                if record["indices"] is None or sum(count for _, count in requests) != len(record["indices"]):
                    raise ValueError("native allocation lacks its complete request demand or result")
                operation.update(requests=requests, indices=record["indices"])
            elif kind == "release_request":
                operation.update(request=observed["request"], slot=observed["slot"])
            elif kind == "release_pages":
                if observed["grouped"]:
                    continue
                operation["pages"] = sorted({index // self.page_size for index in observed["indices"]})
            elif kind == "prefix":
                action = observed["operation"]
                operation["operation"] = action
                if action in {"match", "insert"}:
                    tokens = observed["tokens"]
                    length = len(tokens) // self.page_size * self.page_size
                    operation.update(tokens=tokens[:length], node=record["result"]["node"])
                    if action == "match":
                        operation["indices"] = record["result"]["indices"]
                    else:
                        operation.update(indices=observed["indices"][:length], existing_tokens=record["result"]["existing_tokens"])
                elif action == "lock":
                    operation.update(node=observed["node"], acquire=observed["acquire"], delta=record["result"])
                elif action == "evict":
                    operation.update(tokens=observed["tokens"], evicted_tokens=record["result"])
                else:
                    raise ValueError("unknown observed prefix operation")
            else:
                raise ValueError("unknown observed resource operation")
            self.apply(operation)

    def predicted(self, row, requests):
        observed, result = row["operation"], row["result"]
        kind = observed["kind"]

        # Queries have no observed mutation counterpart; request-prefix rebinding is checked at consumed page tables.
        if kind in {"declare_resources", "capacity", "free_slots", "bind_prefix"}:
            return
        if kind == "state_slots":
            field, operation = observed["field"], observed["operation"]
            if operation == "declare":
                self.fields.declare(field, observed["capacity"], list(range(observed["capacity"])))
            elif operation == "available":
                if result != len(self.fields.free[field]):
                    raise ValueError("state slot availability differs from observed ownership")
            else:
                value = {"field": field, "operation": operation, "slot": result if operation == "allocate" else observed["slot"]}
                if operation == "bind":
                    value["request"] = requests[observed["request"]]
                if operation == "release" and row["returned_fields"] != [[field, observed["slot"]]]:
                    raise ValueError("state slot release differs from its invalidation record")
                self.fields.apply(value)
            return
        operation = {"kind": kind}
        if kind == "slots":
            operation.update(requests=[requests[member["request"]] for member in observed["members"]],
                             slots=[member["slot"] for member in result])
        elif kind == "allocate":
            operation.update(requests=[[requests[member["request"]], member["tokens"]] for member in observed["members"]],
                             indices=chain.from_iterable(member["indices"] for member in result))
        elif kind == "release_request":
            slot, = row["returned_slots"]
            operation.update(request=requests[observed["request"]], slot=slot)
        elif kind == "release_pages":
            if observed["pages"] != row["returned_pages"]:
                raise ValueError("resource release differs from its invalidation record")
            operation["pages"] = observed["pages"]
        elif kind == "prefix":
            action = observed["operation"]
            operation["operation"] = action
            if action in {"match", "insert"}:
                pages = result["pages"] if action == "match" else observed["pages"]
                operation.update(tokens=observed["tokens"], node=result["node"], pages=pages)
                if action == "insert":
                    operation["existing_tokens"] = result["existing_tokens"]
            elif action == "lock":
                operation.update(node=observed["node"], acquire=observed["acquire"], delta=result)
            elif action == "evict":
                operation.update(tokens=observed["tokens"], evicted_tokens=sum(map(len, result)) * self.page_size)
            else:
                raise ValueError("unknown predicted prefix operation")
        else:
            raise ValueError("unknown predicted resource operation")
        self.apply(operation)


class ResourceEffectComparison:
    def __init__(self):
        self.native, self.predicted = {}, {}
        self.requests = []
        self.counts = {}

    def observed(self, history, graph, group):
        source = ResourceEffects(history["initial"]["page_size"])
        source.native(history, graph["scopes"], graph["environment"]["implementation"]["cache"]["disabled"])
        self.native[group] = source
        self.predicted[group] = ResourceEffects(source.page_size)

    @staticmethod
    def summary(operation):
        return {name: {"count": len(value), "first": value[:4]} if isinstance(value, list) else value
                for name, value in operation.items()}

    def compare(self):
        matched, totals = 0, Counter()
        for group, source in self.native.items():
            target = self.predicted[group]
            for operation in source.operations:
                totals[operation["kind"]] += 1
            if source.operations == target.operations:
                matched += 1
            else:
                different = next((index for index, pair in enumerate(zip(source.operations, target.operations)) if pair[0] != pair[1]),
                                 min(len(source.operations), len(target.operations)))
                yield "resource_effects", {"group": group}, {
                    "first_difference": different, "native_operations": len(source.operations), "predicted_operations": len(target.operations),
                    "native": self.summary(source.operations[different]) if different < len(source.operations) else None,
                    "predicted": self.summary(target.operations[different]) if different < len(target.operations) else None,
                }
        state_matched = 0
        state_unobserved = []
        for group, source in self.native.items():
            target = self.predicted[group]
            if target.fields.capacities and not source.fields.capacities:
                state_unobserved.append(group)
            elif source.fields.capacities == target.fields.capacities and source.fields.operations == target.fields.operations:
                state_matched += 1
            else:
                yield "state_slot_effects", {"group": group}, {
                    "native_capacity": source.fields.capacities, "predicted_capacity": target.fields.capacities,
                    "native_operations": len(source.fields.operations), "predicted_operations": len(target.fields.operations),
                }
        self.counts = {"groups": len(self.native), "groups_matched": matched, "operations": dict(totals),
                       "state_slots": {"groups_matched": state_matched, "unobserved_native": state_unobserved,
                                       "predicted_operations": sum(len(target.fields.operations) for target in self.predicted.values())},
                       "scope": "Per-worker observed allocation/cache/free operation inputs and results under opaque identity renaming. Excludes queries, explicit request-prefix rebinding between snapshots, and intra-call effect timing."}
