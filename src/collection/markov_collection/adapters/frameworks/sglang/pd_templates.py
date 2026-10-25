"""Declare PD interfaces from native ports, request bindings and registered buffers."""

from __future__ import annotations

from copy import deepcopy
from typing import Any


class DisaggregatedTemplates:
    def __init__(self, source: dict[str, Any]) -> None:
        self.source = source
        self.regions = source["regions"]
        self.values = source["values"]
        self.owners: list[int | None] = []
        self.batches: list[int | None] = []
        for region in self.regions:
            parent = region["parent"]
            owner = self.owners[parent] if parent is not None else None
            batch = self.batches[parent] if parent is not None else None
            if region["role"] in ("KvReserve", "BatchAccepted", "KvSend", "KvReceived"):
                owner = region["id"]
            self.owners.append(owner)
            self.batches.append(region["metadata"].get("batch", batch))

        self.operations: dict[int, list[dict[str, Any]]] = {}
        self.children: dict[int, list[dict[str, Any]]] = {}
        self.request_maps: dict[int, int] = {}
        self.request_slots: dict[int, int] = {}
        for operation in source["operations"]:
            owner = self.owners[operation["region"]]
            if owner is not None:
                self.operations.setdefault(owner, []).append(operation)
        for region in self.regions:
            owner = self.owners[region["id"]]
            if owner is not None:
                self.children.setdefault(owner, []).append(region)
            metadata = region["metadata"]
            if metadata.get("entrypoint") == "request_cache_retain":
                batch = self.batches[region["id"]]
                self.request_maps[batch] = metadata["ports"]["inputs"]["request_map"]
                self.request_slots[batch] = metadata["parameters"]["request_slot"]

    @classmethod
    def declare(cls, source: dict[str, Any]) -> None:
        if source["environment"]["source_transport"] != "native_disaggregation":
            return

        from markov_collection.adapters.runtimes.ascend.transfer import MemfabricObservation

        MemfabricObservation.declare(source["regions"])
        cls(source).build()

    def build(self) -> None:
        buffers = {}
        for region in self.regions:
            metadata = region["metadata"]
            for identity in metadata.get("metadata_buffers", {}).values():
                value = self.values[identity]
                buffers[value["storage"]] = value
            if region["role"] == "KvReserve":
                self.reserve(region)
            elif region["role"] == "BatchAccepted":
                self.accept(region)
            elif region["role"] in ("KvSend", "KvReceived"):
                self.metadata_indices(region)
                if region["role"] == "KvSend":
                    self.send(region)
            elif region["role"] == "PrebuiltResult":
                metadata.update(entrypoint="prebuilt_result", entrypoint_phase=None, dynamic_calls=True)
                metadata["parameters"] = {"result_members": len(metadata["effects"])}
            elif region["role"] == "RequestOutput":
                metadata["entrypoint_phase"] = None

        for value in self.values:
            buffer = buffers.get(value["storage"])
            if buffer is not None and (value["shape"], value["stride"], value["offset"]) == (
                buffer["shape"],
                buffer["stride"],
                buffer["offset"],
            ):
                value["shape_relation"] = [["mul", 2, "R"], *value["shape"][1:]]

        sends = [region for region in self.regions if region["role"] == "KvSend"]
        if sends:
            sent = sends[0]["metadata"]
            auxiliary = next(region for region in self.regions if region["role"] == "KvMetadataTransfer")
            copied = next(
                region
                for region in self.regions
                if region["parent"] == auxiliary["id"] and region["role"] == "RemoteCopy"
            )
            self.source["environment"]["kv_layout"] = {
                "page_size": sent["page_size"],
                "buffer_item_bytes": sent["kv_item_bytes"],
                "metadata_item_bytes": copied["metadata"]["segments"],
            }

    def reserve(self, region: dict[str, Any]) -> None:
        metadata = region["metadata"]
        metadata["parameters"]["request_slot"] = metadata["slot"]
        metadata["guards"] = [
            {"condition": ["lt", 0, "request_slot"], "expected": True},
            {"condition": ["le", "request_slot", "R"], "expected": True},
        ]
        storage = self.values[metadata["ports"]["inputs"]["request_map"]]["storage"]
        for child in self.children[region["id"]]:
            declaration = child["metadata"]
            if declaration.get("interface") != "tensor_assignment":
                continue
            arguments = declaration["arguments"]
            if self.values[arguments[0]["value"]]["storage"] != storage:
                continue

            # The cold destination writes precisely this request's newly reserved token interval.
            relation = deepcopy(arguments)
            relation[1] = [{"scalar": "request_slot"}, {"slice": [0, {"scalar": "M"}, None]}]
            declaration["argument_relations"] = relation

    def accept(self, region: dict[str, Any]) -> None:
        metadata = region["metadata"]
        batch = self.source["batches"][metadata["batch"]]
        parameters = {**batch["parameters"], "M": sum(member["context_tokens"] for member in batch["members"])}
        outputs = metadata["ports"]["outputs"]
        outputs["request_map"] = self.request_maps[metadata["batch"]]
        token_storage = self.values[outputs["token_relay"]]["storage"]
        token_inputs = [
            value
            for operation in self.operations[region["id"]]
            for identity in operation["inputs"]
            if (value := self.values[identity])["storage"] == token_storage
            and value["shape"] == self.values[outputs["token_relay"]]["shape"]
        ]
        token_input = min(token_inputs, key=lambda value: value["version"])["id"]
        inputs = {
            "request_map": outputs["request_map"],
            "token_relay": token_input,
            "free_pages": outputs["free_pages"],
        }
        parameters["free_pages_before"] = self.values[outputs["free_pages"]]["shape"][0]
        metadata.update(
            interface="batch_accept",
            entrypoint_phase=None,
            inputs=list(inputs.values()),
            outputs=list(outputs.values()),
            writes=[token_storage],
            parameters=parameters,
            arguments=[{"scalar": "M"}, {"scalar": "B"}],
            keywords={"mapping": []},
            ports={"inputs": inputs, "outputs": outputs},
            guards=[{"condition": ["eq", "B", len(batch["members"])], "expected": True}],
            shape_contracts=[{"value": inputs["free_pages"], "shape": ["free_pages_before"]}],
        )
        for name, identity in outputs.items():
            if name not in ("request_map", "free_pages", "token_relay"):
                self.values[identity]["shape_relation"] = ["B", *self.values[identity]["shape"][1:]]

    def metadata_indices(self, region: dict[str, Any]) -> None:
        metadata = region["metadata"]
        storages = {self.values[identity]["storage"] for identity in metadata["metadata_buffers"].values()}
        selectors = [
            operation
            for operation in self.operations[region["id"]]
            if operation["name"] == "aten.select.int"
            and self.values[operation["inputs"][0]]["storage"] in storages
            and len(self.values[operation["inputs"][0]]["shape"]) == 2
        ]

        # Metadata rows are local to the instance; request-map slots separately reserve their padded row zero.
        metadata["parameters"].update(metadata_slot=selectors[0]["arguments"][2], handoff_id=metadata["room"])
        metadata["guards"] = [
            {"condition": ["le", 0, "metadata_slot"], "expected": True},
            {"condition": ["lt", "metadata_slot", ["mul", 2, "R"]], "expected": True},
        ]
        for operation in selectors:
            relation = deepcopy(operation["arguments"])
            relation[2] = {"scalar": "metadata_slot"}
            operation["argument_relations"] = relation

        room_storage = self.values[metadata["metadata_buffers"]["bootstrap_room"]]["storage"]
        for child in self.children[region["id"]]:
            declaration = child["metadata"]
            if declaration.get("interface") != "tensor_assignment":
                continue
            arguments = declaration["arguments"]
            target = self.values[arguments[0]["value"]]
            if target["storage"] not in storages:
                continue
            relation = deepcopy(arguments)
            if len(target["shape"]) == 2:
                relation[1][0] = {"scalar": "metadata_slot"}
            if target["storage"] == room_storage:
                relation[-1] = {"scalar": "handoff_id"}
            declaration["argument_relations"] = relation

    def send(self, region: dict[str, Any]) -> None:
        metadata = region["metadata"]
        batch = self.batches[region["id"]]
        mapping = self.request_maps[batch]
        metadata.update(
            entrypoint="kv_send", entrypoint_phase=None, ports={"inputs": {"request_map": mapping}, "outputs": {}}
        )
        metadata["parameters"]["request_slot"] = self.request_slots[batch]
        metadata["guards"].extend(
            [
                {"condition": ["lt", 0, "request_slot"], "expected": True},
                {"condition": ["le", "request_slot", "R"], "expected": True},
            ]
        )
        storage = self.values[mapping]["storage"]
        for operation in self.operations[region["id"]]:
            if not operation["inputs"] or self.values[operation["inputs"][0]]["storage"] != storage:
                continue
            relation = deepcopy(operation["arguments"])
            if operation["name"] == "aten.select.int" and len(self.values[operation["inputs"][0]]["shape"]) == 2:
                relation[2] = {"scalar": "request_slot"}
                operation["argument_relations"] = relation
            elif operation["name"] == "aten.slice.Tensor" and len(self.values[operation["inputs"][0]]["shape"]) == 1:
                relation[3] = {"scalar": "M"}
                operation["argument_relations"] = relation
                for identity in operation["outputs"]:
                    self.values[identity]["shape_relation"] = ["M"]

        for child in self.children[region["id"]]:
            if child["role"] != "HostRead" or child["metadata"]["path"] != "cpu":
                continue
            read = child["metadata"]
            source = read["source"]
            outputs = [
                identity
                for operation in self.operations[region["id"]]
                if operation["region"] == child["id"]
                for identity in operation["outputs"]
            ]
            read.update(
                interface="host_read",
                inputs=[source],
                outputs=outputs,
                writes=[],
                arguments=[{"value": source}],
                keywords={"mapping": []},
            )
            self.values[source]["shape_relation"] = ["M"]
            for identity in outputs:
                self.values[identity]["shape_relation"] = ["M"]
