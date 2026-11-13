"""Inspect native and instantiated graph contents through shared action and resource semantics."""

import json
from collections import Counter

from actions import ActionIdentity, ActionScopes


class GraphContents(ActionScopes):
    def __init__(self, graph):
        super().__init__(graph)
        self.pools = {}

    @classmethod
    def value_differences(cls, native, predicted, path=""):
        if isinstance(native, dict) and isinstance(predicted, dict):
            for key in sorted(native.keys() | predicted.keys()):
                if key in native and key in predicted:
                    yield from cls.value_differences(native[key], predicted[key], f"{path}/{key}")
                else:
                    yield {"field": f"{path}/{key}", "native": native.get(key), "predicted": predicted.get(key)}
        elif native != predicted:
            yield {"field": path, "native": native, "predicted": predicted}

    @classmethod
    def differences(cls, native, predicted, details=False):
        for path in sorted(native.keys() | predicted.keys()):
            if path not in native or path not in predicted:
                yield "scope_tree", {"scope": path, "native": path in native, "predicted": path in predicted}
                continue
            for field in native[path]:
                if native[path][field] != predicted[path][field]:
                    changes = list(cls.value_differences(native[path][field], predicted[path][field]))
                    yield field, {"scope": path, "changes": changes if details else changes[:3]}

    def control_key(self, root, identity):
        scope = self.graph["scopes"][root]
        return json.dumps({"control": {
            **{name: identity[name] for name in ("group", "action", "parameters")},
            **{name: scope[name] for name in ("site", "implementation")},
            "owner": (identity["work"]["continuation"] or {}).get("owner"),
        }}, sort_keys=True)

    def declared_control(self, root):
        scope = self.graph["scopes"][root]
        if not scope["bindings"].get("segment", {}).get("host_only", False):
            return False
        ports = {(port["name"], port["direction"]): port for port in scope["ports"]}
        host_ports = len(scope["ports"]) == 2 and ports.keys() == {("call.enter", "input"), ("call.return", "output")}
        for port in scope["ports"]:
            resource = self.graph["resources"][self.graph["events"][port["event"]]["resource"]]
            host_ports &= (resource["kind"] == "thread" and resource["role"] == "control"
                           and port["use"] == "control" and port["relation"] == "control"
                           and not any(port.get(name) for name in ("tensor", "wait", "signal")) and "rendezvous" not in port)
        pieces = self.work[root]
        resource = self.graph["resources"][pieces[0]["resource"]] if len(pieces) == 1 else {}

        # A native declaration licenses shared CPU contents, never hidden children, device work or resource effects.
        if (scope["role"] != "segment" or self.children[root] or not host_ports or len(pieces) != 1
                or pieces[0]["kind"] != "host" or resource.get("kind") != "thread" or resource.get("role") != "control"
                or scope.get("communication") or scope["kv_accesses"] or scope["slot_accesses"]):
            raise ValueError("native host-only declaration contains undeclared execution structure")
        return True

    def structure_key(self, root, identity, controls):
        key = self.control_key(root, identity)
        return key if key in controls else ActionIdentity.structure_key(identity)

    def observed_pools(self, raw, registrations, groups):

        # The native session identifies the actual destination independently of the predictor's routing rules.
        targets = sorted({registrations[scope["native_transfer"]["session"]]
                          for scope in raw["scopes"] if "native_transfer" in scope})
        self.pools = {f"destination/{position}": groups[rank] for position, rank in enumerate(targets)}
        bindings = {rank: f"destination/{position}" for position, rank in enumerate(targets)}
        for scope in self.graph["scopes"]:
            if scope["role"] == "kv_transfer":
                expected = set()
                for origin in scope["bindings"]["source_scopes"]:
                    transfer = raw["scopes"][origin]["native_transfer"]
                    field = "kv_accesses" if transfer["kind"] == "kv" else "slot_accesses"
                    if transfer["kind"] == "kv" and not transfer["source_pages"]:
                        continue
                    destination = bindings[registrations[transfer["session"]]]
                    expected.update(((field, "local", "read"), (field, destination, "write")))
                actual = {(field, access["pool"], access["access"])
                          for field in ("kv_accesses", "slot_accesses") for access in scope[field]}
                if actual != expected:
                    raise ValueError("coarse transfer loses or invents a native payload endpoint")

    def pool(self, name, group):
        return group if name == "local" else self.pools.get(name, name)

    def resource(self, index):
        resource = self.graph["resources"][index]
        return {name: resource[name] for name in ("kind", "role", "rank", "serial")}

    def paths(self, root):
        paths, pending = {root: "root"}, [root]
        for index in pending:
            for child, scope in enumerate(self.children[index]):
                paths[scope] = f"{paths[index]}/{child}"
                pending.append(scope)
        return paths

    def completion_boundaries(self, root):
        graph = self.graph
        scopes = [root]
        for scope in scopes:
            scopes.extend(self.children[scope])
        retained = {event for scope in scopes for event in self.events[scope]}
        for scope in scopes:
            for work in self.work[scope]:
                retained.update((work["start"], work["finish"]))
            for port in graph["scopes"][scope]["ports"]:
                retained.add(port["event"])
                if "wait" in port:
                    retained.add(port["wait"]["begin"])
                if "rendezvous" in port:
                    retained.add(port["rendezvous"])
            for access in graph["scopes"][scope]["kv_accesses"] + graph["scopes"][scope]["slot_accesses"]:
                retained.update((access["start"], access["finish"]))

        ports = graph["scopes"][root]["ports"]
        named_resources = {graph["events"][port["event"]]["resource"] for port in ports}
        result = [{"at": {"port": port["name"], "direction": port["direction"]}, "event": port["event"]} for port in ports]
        tails = {}
        for event in retained:
            resource = graph["events"][event]["resource"]
            if not graph["resources"][resource]["serial"]:
                continue
            previous = tails.get(resource)
            if previous is None or graph["evidence"]["events"][event]["order"] > graph["evidence"]["events"][previous]["order"]:
                tails[resource] = event
        for resource, event in sorted(tails.items()):
            names = sorted((port["name"], port["direction"]) for port in ports
                           if graph["events"][port["event"]]["resource"] == resource)
            if names:
                name, direction = names[0]
                identity = {"port": name, "direction": direction}
            else:
                identity = {key: graph["resources"][resource][key] for key in ("kind", "role")}
                if sum(all(graph["resources"][peer][key] == value for key, value in identity.items())
                       for peer in tails.keys() - named_resources) != 1:
                    raise ValueError(f"scope {root} ({graph['scopes'][root]['site']}) resource tail {identity} "
                                     "has no unique named interface or resource role")

            # A semantic resource identity selects a tail; native thread/stream IDs never cross the run boundary.
            result.append({"at": {"resource": identity}, "event": event})
        return result

    def boundary_event(self, root, selector):
        ports = self.graph["scopes"][root]["ports"]
        identity = selector.get("resource", selector)
        if "port" in identity:
            port, = [port for port in ports if (port["name"], port["direction"]) == (identity["port"], identity["direction"])]
            if "resource" not in selector:
                return port["event"]
            resource = self.graph["events"][port["event"]]["resource"]
            candidates = [port for port in ports if port["name"].startswith("resource.exit/")
                          and self.graph["events"][port["event"]]["resource"] == resource]
        else:
            named_resources = {self.graph["events"][port["event"]]["resource"] for port in ports
                               if not port["name"].startswith(("resource.enter/", "resource.exit/"))}
            candidates = [port for port in ports if port["name"].startswith("resource.exit/")
                          and self.graph["events"][port["event"]]["resource"] not in named_resources
                          and all(self.graph["resources"][self.graph["events"][port["event"]]["resource"]][key] == value
                                  for key, value in identity.items())]
        boundary, = candidates
        return boundary["event"]

    def snapshot(self, root, group):
        graph, paths = self.graph, self.paths(root)

        # Relative versions and alias partitions survive different addresses and allocation histories.
        versions = {}
        for index in paths:
            for port in graph["scopes"][index]["ports"]:
                if port["use"] == "data" and (tensor := port.get("tensor")):
                    storage = tensor["storage"]
                    versions[storage] = min(versions.get(storage, tensor["version"]), tensor["version"])
        storages, resources, channels = {}, {}, {}
        channel_origins = {}
        for index in paths:
            if communication := graph["scopes"][index].get("communication"):
                channel = communication["channel"]
                channel_origins[channel] = min(channel_origins.get(channel, communication["order"]), communication["order"])

        result = {}
        for index in paths:
            scope = graph["scopes"][index]
            contents = {name: scope[name] for name in ("role", "site", "implementation", "parameters")}
            contents["weights"] = scope["bindings"].get("weights")
            ports = {}
            for port in sorted(scope["ports"], key=lambda value: (value["name"], value["direction"])):

                # These ports are added during action cutting; cross-action serial frontiers need their own check.
                if index == root and port["name"].startswith(("resource.enter/", "resource.exit/")):
                    continue
                boundary = {name: port[name] for name in ("use", "relation")}
                resource = graph["events"][port["event"]]["resource"]
                resources.setdefault(resource, len(resources))
                boundary["resource"] = {**self.resource(resource), "alias": resources[resource]}
                if "wait" in port:
                    waited = port["wait"]["resource"]
                    resources.setdefault(waited, len(resources))
                    boundary["wait_resource"] = {**self.resource(waited), "alias": resources[waited]}
                boundary["rendezvous"] = "rendezvous" in port
                if tensor := port.get("tensor"):
                    boundary["tensor"] = {"dtype": tensor["dtype"], "shape": tensor["shape"]}

                    # Metadata describes layout; its runtime buffer lifecycle is not a graph data dependency.
                    if port["use"] == "data":
                        storage = tensor["storage"]
                        storages.setdefault(storage, len(storages))
                        boundary["tensor"].update(
                            storage_alias=storages[storage], version=tensor["version"] - versions[storage],
                            writer=paths.get(tensor["writer"], "external"),
                            allocation="internal" if tensor["allocation"] in paths else "external",
                            buffer=tensor["buffer"],
                        )
                ports[f"{port['direction']}:{port['name']}"] = boundary
            contents["ports"] = ports

            for field in ("kv_accesses", "slot_accesses"):
                accesses = []
                for access in scope[field]:
                    value = {name: value for name, value in access.items() if name not in {"start", "finish", "rule"}}
                    value["pool"] = self.pool(value["pool"], group)
                    accesses.append(value)
                contents[field] = accesses
            communication = scope.get("communication")
            if communication:

                # Agreement rules are template declarations; native geometry is compared through the tensor ports above.
                communication = {name: value for name, value in communication.items() if name != "agreements"}
                channel = communication["channel"]
                channels.setdefault(channel, len(channels))
                communication = {**communication, "channel": channels[channel], "order": communication["order"] - channel_origins[channel]}
            contents["communication"] = communication
            work = Counter(json.dumps({"kind": item["kind"], "resource": self.resource(item["resource"])}, sort_keys=True)
                           for item in self.work[index])
            contents["work"] = dict(work)
            result[paths[index]] = contents
        return result
