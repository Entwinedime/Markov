"""Publish received CPU metadata only when its native Work is consumed."""

import torch


class MetadataWork:
    def __init__(self, peer, handle, tensor, wait_operation: str) -> None:
        self.peer = peer
        self.handle = handle
        self.tensor = tensor
        self.wait_operation = wait_operation
        self.finished = handle is None

    def wait(self):
        if not self.finished:
            values = self.peer.exchange(self.wait_operation, self.handle)
            if self.tensor is not None:
                self.tensor.copy_(torch.tensor(values, dtype=self.tensor.dtype).reshape_as(self.tensor))
            self.finished = True
        return True
