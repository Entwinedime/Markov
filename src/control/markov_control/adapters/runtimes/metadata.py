"""Publish received CPU metadata only when its native Work is consumed."""

import torch


class MetadataWork:
    def __init__(self, peer, handle, tensor, wait_operation: str, *, context=None) -> None:
        self.peer = peer
        self.handle = handle
        self.tensor = tensor
        self.wait_operation = wait_operation
        self.finished = handle is None
        self.context = context

    def wait(self):
        if not self.finished:
            message = self.handle if self.context is None else {"key": self.handle, "execution": self.context.get()}
            if self.context is not None and message["execution"] is None:
                raise ValueError("CPU metadata wait requires its native protocol context")
            values = self.peer.exchange(self.wait_operation, message)
            if self.tensor is not None:
                self.tensor.copy_(torch.tensor(values, dtype=self.tensor.dtype).reshape_as(self.tensor))
            self.finished = True
        return True
