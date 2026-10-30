"""Expose native batch-column selection and concatenation without owning requests."""

from __future__ import annotations

from copy import copy, deepcopy
from itertools import product
from typing import Any

import torch

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
    def phase(batch: Any) -> str:
        if batch.forward_mode.is_mixed():
            return "mixed"
        return "prefill" if batch.forward_mode.is_extend() else "decode"

    @staticmethod
    def members(batch: Any) -> list[dict[str, Any]]:
        lengths = batch.extend_lens if batch.forward_mode.is_extend() else [1] * len(batch.reqs)
        decoding = {request.rid for request in batch.decoding_reqs or ()}

        # Prepared columns include each member's actual input, even when cached prefix indices lag behind.
        return [
            {
                "id": request.rid,
                "new_tokens": tokens,
                "context_tokens": sequence - tokens,
                "slot": int(request.req_pool_idx),
                "phase": "decode" if batch.forward_mode.is_decode() or request.rid in decoding else "prefill",
            }
            for request, sequence, tokens in zip(batch.reqs, batch.seq_lens_cpu.tolist(), lengths, strict=True)
        ]

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
            if region["role"] not in {"Prefill", "Decode", "Mixed"}:
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
        metadata["required_inputs"] = sorted(self.input_ports)
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


class BatchTransitionPrototypes:
    def __init__(self, operators: Any, fields: dict[str, str]) -> None:
        self.operators = operators
        self.fields = fields
        self.samples: dict[tuple[str, ...], Any] = {}
        self.captured = False

    @staticmethod
    def copy_batch(batch: Any) -> Any:
        result = copy(batch)

        # Copy tensor storage together to preserve aliases, and detach lists mutated by native merge.
        tensors = {name: value for name, value in vars(batch).items() if isinstance(value, torch.Tensor)}
        vars(result).update(deepcopy(tensors))
        vars(result).update({name: value.copy() for name, value in vars(batch).items() if isinstance(value, list)})
        result.reqs = [copy(request) for request in batch.reqs]
        for request in result.reqs:
            request.output_ids = copy(request.output_ids)

        # Source sampling is unpenalized greedy; the native constructor owns its tensor and weak-reference state.
        result.sampling_info = type(batch.sampling_info).from_schedule_batch(result, batch.sampling_info.vocab_size)
        return result

    def remember(self, batch: Any) -> None:
        if self.captured or not batch.reqs:
            return
        signature = tuple(BatchTensors.read(batch, self.fields))
        if signature not in self.samples:
            with self.operators.values.auxiliary():
                self.samples[signature] = self.copy_batch(batch)

    def prepare(self, left: Any, right: Any, selection: str | None) -> tuple[Any, Any]:
        first, second = self.copy_batch(left), self.copy_batch(right)
        if selection is not None:

            # Two copies guarantee that a proper subset exists even for a single-member source.
            first.merge_batch(second)
        return first, second

    @staticmethod
    def invoke(first: Any, second: Any, selection: str | None) -> None:
        if selection is None:
            first.merge_batch(second)
        else:
            rows = {"empty": [], "all": list(range(len(first.reqs))), "subset": [0]}
            first.filter_batch(keep_indices=rows[selection])

    def capture(self) -> None:
        if self.captured:
            return
        self.captured = True

        # Cover native filter branches and merge column pairs, independently of arrival timing or batch size.
        cases = [(left, right, None) for left, right in product(self.samples.values(), repeat=2)]
        cases += [
            (batch, batch, selection) for batch in self.samples.values() for selection in ("empty", "all", "subset")
        ]
        for left, right, selection in cases:
            with self.operators.values.auxiliary():
                first, second = self.prepare(left, right, selection)
                self.invoke(first, second, selection)
                torch.npu.synchronize()

            for observed in (False, True):
                with self.operators.values.auxiliary():
                    first, second = self.prepare(left, right, selection)
                with self.operators.prototype("BatchPrototype", observed):
                    self.invoke(first, second, selection)
                torch.npu.synchronize()

        self.samples.clear()
