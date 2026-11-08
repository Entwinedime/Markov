"""Native MHA buffer geometry shared by observation and device-free PD control."""

from bisect import bisect_right
from dataclasses import asdict, dataclass
from itertools import accumulate
from math import prod


@dataclass(frozen=True)
class MhaKvLayout:
    start_layer: int
    end_layer: int
    head_num: int
    dtype: str
    key_token_bytes: int
    value_token_bytes: int

    @classmethod
    def from_pool(cls, pool):
        import torch

        buffers = pool.k_buffer, pool.v_buffer
        if any(not isinstance(buffer, torch.Tensor) or buffer.ndim != 5 for buffer in buffers):
            raise NotImplementedError("KV transfer layout requires contiguous paged Ascend MHA buffers")
        widths = [prod(buffer.shape[3:]) * buffer.element_size() for buffer in buffers]
        return cls(pool.start_layer, pool.start_layer + pool.layer_num, pool.head_num, str(pool.k_buffer.dtype), *widths)

    @classmethod
    def read(cls, value):
        layout = cls(**value)
        if not isinstance(layout.dtype, str) or any(type(field) is not int for name, field in value.items() if name != "dtype"):
            raise TypeError("KV layout requires a dtype name and integer extents")
        if not 0 <= layout.start_layer < layout.end_layer or min(layout.head_num, layout.key_token_bytes, layout.value_token_bytes) <= 0:
            raise ValueError("KV layout requires an ordered layer interval and positive head/byte extents")
        if layout.key_token_bytes % layout.head_num or layout.value_token_bytes % layout.head_num:
            raise ValueError("KV token bytes must contain whole heads")
        return layout

    def document(self):
        return asdict(self)

    def buffers(self, token_capacity, page_size):
        layers = self.end_layer - self.start_layer
        widths = [self.key_token_bytes] * layers + [self.value_token_bytes] * layers
        lengths = [width * ((token_capacity // page_size + 1) * page_size) for width in widths]

        # These are disjoint coordinates for native address arithmetic, never process or device pointers.
        starts = list(accumulate(lengths, initial=1))[:-1]
        return KvBufferAddresses(self, page_size, starts, lengths, [width * page_size for width in widths])


class KvBufferAddresses:
    def __init__(self, layout, page_size, starts, lengths, item_bytes):
        self.layout = layout
        self.page_size = page_size
        layers = layout.end_layer - layout.start_layer
        self.token_bytes = [layout.key_token_bytes] * layers + [layout.value_token_bytes] * layers
        self.starts, self.lengths, self.item_bytes = starts, lengths, item_bytes
        if len(starts) != len(self.token_bytes) or len(lengths) != len(starts) or item_bytes != [width * page_size for width in self.token_bytes]:
            raise ValueError("registered KV buffers differ from the observed layer/page layout")
        self.addresses = sorted((address, index) for index, address in enumerate(starts))
        previous_end = 0
        for address, index in self.addresses:
            length = lengths[index]
            if address < previous_end or length <= 0 or length % item_bytes[index]:
                raise ValueError("registered KV buffers overlap or contain partial pages")
            previous_end = address + length

    def native_infos(self):
        return self.starts, self.lengths, self.item_bytes

    def locate(self, address, length):
        position = bisect_right(self.addresses, (address, len(self.starts))) - 1
        if position < 0:
            raise ValueError("native KV transfer precedes all registered buffers")
        index = self.addresses[position][1]
        if length < 0 or address + length > self.starts[index] + self.lengths[index]:
            raise ValueError("native KV transfer crosses an observed buffer boundary")
        return index, address - self.starts[index], length

    def regions(self, address, length):
        index, offset, length = self.locate(address, length)
        layers = self.layout.end_layer - self.layout.start_layer
        width = self.token_bytes[index]
        component = 0 if index < layers else self.layout.key_token_bytes
        token_bytes = self.layout.key_token_bytes + self.layout.value_token_bytes
        regions = []
        stop = offset + length

        # Only partial first/last rows split; any intervening full rows stay one range.
        while offset < stop:
            token, begin = divmod(offset, width)
            remaining = stop - offset
            if begin:
                tokens, end = 1, min(width, begin + remaining)
            elif remaining >= width:
                tokens, end = remaining // width, width
            else:
                tokens, end = 1, remaining
            regions.append({
                "layer": self.layout.start_layer + index % layers,
                "token_begin": token, "token_end": token + tokens,
                "slice": {"token_bytes": token_bytes, "begin": component + begin, "end": component + end},
            })
            offset += (end - begin) * tokens
        return regions
