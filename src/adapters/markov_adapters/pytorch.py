"""Separate CPU transfer identity from its physical wire features."""

from collections import defaultdict


class CpuTransfer:
    def __init__(self, world) -> None:
        self.world = world

    @staticmethod
    def identity(description):

        # Private object serialization can change byte counts without changing the native control operation.
        return {name: description[name] for name in ("group", "source", "target", "tag", "receiving", "dtype")}

    def describe(self, tensor, group, tag, endpoint, group_endpoint, receiving):
        import torch.distributed as distributed

        group = distributed.group.WORLD if group is None else group
        if group_endpoint is not None:
            endpoint = distributed.get_global_rank(group, group_endpoint)
        if endpoint is None:
            raise ValueError("Native metadata receive requires an explicit sender")
        local = distributed.get_rank()
        source, target = (endpoint, local) if receiving else (local, endpoint)
        return {
            "group": group.group_name, "source": self.world[source], "target": self.world[target],
            "tag": tag, "receiving": receiving, "dtype": str(tensor.dtype), "shape": list(tensor.shape),
            "elements": tensor.numel(), "element_bytes": tensor.element_size(),
        }


class CpuProtocolWork:
    """Use the same native message geometry for demands and measurements."""

    def __init__(self, records):
        self.records = records

    @staticmethod
    def service(descriptor, execution, operation):
        route = execution["work"]["context"]["cpu_transfer"]
        identity = {name: descriptor[name] for name in ("source", "target", "dtype")}
        identity.update(operation=operation, channel=route["group"])
        axes = {f"parameter.{name}": descriptor[name] for name in ("elements", "element_bytes")}
        return identity, {"phase": "", "axes": axes}

    @classmethod
    def local(cls, record):
        for post in record["posts"]:
            intervals = [post["interval"]]
            if post["operation"] in {"isend", "irecv"} and post["consumed"] is not None:
                intervals.append(post["consumed"])
            for interval in intervals:
                execution = interval["execution"]
                if execution["in_flow"]:
                    identity, features = cls.service(post["descriptor"], execution, execution["operation"])
                    yield identity, features, interval

    def wires(self):
        messages = defaultdict(dict)
        for record in self.records.values():
            for post in record["posts"]:
                messages[tuple(post["key"])][post["descriptor"]["receiving"]] = post
        for peers in messages.values():
            if len(peers) != 2:
                continue
            sender, receiver = peers[False], peers[True]
            if sender["consumed"] is None and receiver["consumed"] is None:
                continue
            sent, received = (post["descriptor"] for post in (sender, receiver))
            if any(sent[name] != received[name] for name in ("dtype", "elements", "element_bytes")):
                raise ValueError("native CPU message endpoints disagree on transfer geometry")
            for post in (sender, receiver):
                execution = post["interval"]["execution"]
                if execution["in_flow"]:
                    yield self.service(sent, execution, "cpu_transport")
                    break
