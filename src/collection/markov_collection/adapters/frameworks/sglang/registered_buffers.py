"""Decode observed PD copy addresses using the native buffer registrations."""

from __future__ import annotations

from bisect import bisect_right
from collections import defaultdict
from typing import Any


class RegisteredBuffers:
    geometry = ("kind", "layer", "head_begin", "head_end", "head_bytes")
    slice_fields = (*geometry, "source_head_offset", "destination_head_offset")

    def __init__(self, descriptor: dict[str, Any]) -> None:
        self.session = descriptor["session"]
        self.rank = descriptor["rank"]
        layout = descriptor["kv_layout"]
        self.page_size = layout["page_size"]
        self.metadata_item_bytes = layout["metadata_item_bytes"]
        buffers = [
            *layout["buffers"],
            *({"kind": "metadata", "field": field, "item_bytes": size}
              for field, size in enumerate(self.metadata_item_bytes)),
        ]
        addresses = [*descriptor["kv_addresses"], *descriptor["metadata_addresses"]]
        lengths = [*descriptor["kv_bytes"], *descriptor["metadata_bytes"]]
        self.ranges = sorted(
            (begin, begin + length, buffer)
            for begin, length, buffer in zip(addresses, lengths, buffers, strict=True)
        )
        self.starts = [begin for begin, _, _ in self.ranges]

    def segment(self, address: int, size: int) -> dict[str, Any]:
        position = bisect_right(self.starts, address) - 1
        if position < 0 or address + size > self.ranges[position][1] or size <= 0:
            raise ValueError("native copy lies outside its registered buffers")
        begin, _, buffer = self.ranges[position]
        offset = address - begin
        if buffer["kind"] == "metadata":
            row, remainder = divmod(offset, buffer["item_bytes"])
            if remainder or size != buffer["item_bytes"]:
                raise ValueError("native metadata copy does not cover one complete row")
            return {"kind": "metadata", "field": buffer["field"], "row": row, "bytes": size}

        head_bytes = buffer["head_bytes"]
        stride = (buffer["head_end"] - buffer["head_begin"]) * head_bytes
        token, offset = divmod(offset, stride)
        if offset % head_bytes or size % head_bytes:
            raise ValueError("native KV copy is not aligned to a registered head")

        # A partial-head copy covers one token; whole heads can span a contiguous token run.
        if size <= stride - offset:
            first = buffer["head_begin"] + offset // head_bytes
            last, count = first + size // head_bytes, 1
        elif offset == 0 and size % stride == 0:
            first, last, count = buffer["head_begin"], buffer["head_end"], size // stride
        else:
            raise ValueError("native KV copy has no contiguous registered head/token interpretation")
        return {
            "kind": buffer["kind"], "layer": buffer["layer"], "head_begin": first, "head_end": last,
            "head_bytes": head_bytes, "head_offset": first - buffer["head_begin"],
            "token": token, "tokens": count,
        }

    def token_intervals(self, segment: dict[str, Any], pages: dict[int, int], begin: int) -> list[tuple[int, int]]:
        token, remaining = segment["token"], segment["tokens"]
        intervals = []
        while remaining:
            page, offset = divmod(token, self.page_size)
            count = min(remaining, self.page_size - offset)

            # Native page order supplies logical positions; no allocator policy is reconstructed.
            first = begin + pages[page] * self.page_size + offset
            intervals.append((first, first + count))
            token += count
            remaining -= count
        return intervals

    def payload(self, destination: RegisteredBuffers, copy: dict[str, Any], context: dict[str, Any]) -> dict[str, Any]:
        slices = defaultdict(list)
        fields = []
        pages = None
        for source, target, size in zip(
            copy["source_addresses"], copy["destination_addresses"], copy["segments"], strict=True
        ):
            left, right = self.segment(source, size), destination.segment(target, size)
            if left["kind"] == "metadata":
                if right != {**left, "row": context["destination_metadata_slot"]} or (
                    left["row"] != context["parameters"]["metadata_slot"]
                ):
                    raise ValueError("native metadata copy has a different field or request row")
                fields.append({"field": left["field"], "bytes": size})
                continue
            if any(left[name] != right[name] for name in self.geometry):
                raise ValueError("native KV copy connects different layer/head slices")
            if pages is None:
                pages = []
                for name in ("source_pages", "destination_pages"):
                    indices = {page: index for index, page in enumerate(context[name])}
                    if len(indices) != len(context[name]):
                        raise ValueError("native copy page sequence has ambiguous logical positions")
                    pages.append(indices)
            begin = context["parameters"]["send_begin"]
            source_tokens = self.token_intervals(left, pages[0], begin)
            target_tokens = destination.token_intervals(right, pages[1], begin)
            if source_tokens != target_tokens:
                raise ValueError("native KV copy connects different logical token intervals")
            key = (*[left[name] for name in self.geometry], left["head_offset"], right["head_offset"])
            slices[key].extend(source_tokens)

        payload = []
        for key, intervals in sorted(slices.items()):
            merged = []
            for begin, end in sorted(intervals):
                if merged and begin < merged[-1][1]:
                    raise ValueError("native KV copy writes the same logical slice twice")
                if merged and begin == merged[-1][1]:
                    merged[-1][1] = end
                else:
                    merged.append([begin, end])
            payload.append({**dict(zip(self.slice_fields, key, strict=True)), "tokens": merged})

        # Metadata acknowledges every registered field, independently of target slot allocation.
        if fields and (payload or sorted(fields, key=lambda item: item["field"]) != [
            {"field": field, "bytes": size} for field, size in enumerate(self.metadata_item_bytes)
        ]):
            raise ValueError("native copy does not contain one complete metadata acknowledgement")
        return {"kind": "metadata" if fields else "kv", "slices": payload, "metadata_fields": fields}
