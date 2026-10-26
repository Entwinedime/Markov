"""Expose native batch-column selection and concatenation without owning requests."""

from __future__ import annotations

from typing import Any

from markov_collection.adapters.runtimes.pytorch.factories import TensorGeometryObservation
from markov_collection.instrumentation.calls import NativeCall


class BatchTensors:
    fields = {
        "request_slots": "req_pool_indices",
        "request_slots_cpu": "req_pool_indices_cpu",
        "sequence_lengths": "seq_lens",
        "sequence_lengths_cpu": "seq_lens_cpu",
        "original_lengths": "orig_seq_lens",
        "sampling_temperatures": "sampling_info.temperatures",
        "sampling_top_ps": "sampling_info.top_ps",
        "sampling_top_ks": "sampling_info.top_ks",
        "sampling_min_ps": "sampling_info.min_ps",
    }

    @staticmethod
    def read(batch: Any, fields: dict[str, str] = fields) -> dict[str, Any]:
        result = {}
        for name, path in fields.items():
            value = batch
            for attribute in path.split("."):
                value = getattr(value, attribute)

            # Prebuilt/relay-staged batches can omit columns that native filtering also leaves absent.
            if value is not None:
                result[name] = value

        return result

    @staticmethod
    def declare_model_inputs(regions: list[dict[str, Any]], values: list[dict[str, Any]]) -> None:
        for region in regions:
            if region["role"] not in {"Prefill", "Decode"}:
                continue
            metadata = region["metadata"]
            columns = {
                value: ["B", *values[value]["shape"][1:]]
                for name, value in metadata["ports"]["inputs"].items()
                if name in BatchTensors.fields
            }
            if "model_tokens" in metadata["ports"]["inputs"]:
                value = metadata["ports"]["inputs"]["model_tokens"]

                # Relayed PP tokens become this batch's input before EP padding reads their geometry.
                axis = "B" if region["role"] == "Decode" else "M"
                columns[value] = [axis, *values[value]["shape"][1:]]

            # Prepared columns belong to this batch even when the native filter returned the same tensor object.
            metadata["shape_contracts"] = [
                contract for contract in metadata.get("shape_contracts", []) if contract["value"] not in columns
            ] + [{"value": value, "shape": shape} for value, shape in columns.items()]


class BatchTransitionCall(NativeCall):
    def __init__(
        self, values: Any, original: Any, batch: Any, other: Any = None, *, fields: dict[str, str] = BatchTensors.fields
    ) -> None:
        inputs = {f"self.{path}": {0: "before_members"} for path in fields.values()}
        if other is not None:
            inputs.update({f"other.{path}": {0: "added_members"} for path in fields.values()})

        super().__init__(
            values.recording,
            values,
            original.__qualname__,
            original,
            inputs,
            interface="batch_filter" if other is None else "batch_merge",
        )
        self.batch = batch
        self.other = other
        self.before_members = len(batch.reqs)
        self.fields = fields

    def resources(self, arguments: dict[str, Any]) -> tuple[Any, Any]:
        self.value_begin = len(self.recording.values)
        inputs = BatchTensors.read(self.batch, self.fields)
        if self.other is not None:
            inputs = {
                **{f"left.{name}": tensor for name, tensor in inputs.items()},
                **{f"right.{name}": tensor for name, tensor in BatchTensors.read(self.other, self.fields).items()},
            }

        self.input_ports = self.values.port_values(inputs)
        self.region = self.recording.current_scope.get()
        return tuple(inputs.values()), ()

    def invoke(self, *args: Any, **kwargs: Any) -> Any:

        # The filter's sole explicit tensor constructor materializes its selected row indices.
        with TensorGeometryObservation(self.values, ("after_members",)):
            return super().invoke(*args, **kwargs)

    def outputs(self, result: Any) -> Any:
        return tuple(self.output_tensors.values())

    def completed(
        self, args: tuple[Any, ...], kwargs: dict[str, Any], result: Any, metadata: dict[str, Any]
    ) -> str | None:
        after_members = len(self.batch.reqs)
        metadata["entrypoint"] = self.interface
        metadata["entrypoint_phase"] = None
        metadata["parameters"] = {"before_members": self.before_members, "after_members": after_members}
        metadata["keywords"] = {"mapping": []}
        metadata["argument_relations"] = [
            {"mapping": [[name, {"value": value}] for name, value in self.input_ports.items()]}
        ]

        if self.other is None:
            metadata["guards"] = [
                {"condition": ["eq", "after_members", 0], "expected": after_members == 0},
                {
                    "condition": ["eq", "after_members", "before_members"],
                    "expected": after_members == self.before_members,
                },
            ]
        else:
            metadata["parameters"]["added_members"] = len(self.other.reqs)
            metadata["derived_parameters"] = [
                {"name": "after_members", "expression": ["add", "before_members", "added_members"]}
            ]

        # An empty native batch leaves stale tensors behind; none are outputs of that transition.
        self.output_tensors = BatchTensors.read(self.batch, self.fields) if after_members else {}
        for tensor in self.output_tensors.values():
            self.values.bind(tensor, {0: "after_members"})
        outputs = self.values.port_values(self.output_tensors)

        if self.other is None and 0 < after_members < self.before_members:
            constructors = [
                record
                for record in self.recording.regions[self.region + 1 :]
                if record["parent"] == self.region and record["metadata"].get("interface") == "tensor_from_data"
            ]
            if len(constructors) != 1:
                raise ValueError("native batch filter must expose one selected-row constructor")
            outputs["kept_rows"] = constructors[0]["metadata"]["outputs"][0]

        metadata["ports"] = {"inputs": self.input_ports, "outputs": outputs}

        # These native column operations have no tensor state beyond their declared inputs.
        inputs = {self.recording.values[value]["storage"] for value in self.input_ports.values()}
        metadata["private_storages"] = sorted(
            {value["storage"] for value in self.recording.values[self.value_begin :]} - inputs
        )
        return None
