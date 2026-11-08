"""Stable identities for native batch lifetimes."""

from torch.utils.weak import WeakIdKeyDictionary


class BatchNames:
    """Name native batch objects without keeping completed batches alive."""

    def __init__(self) -> None:
        self.names = WeakIdKeyDictionary()
        self.sequence = 0

    def describe(self, batch):
        if batch is not None and batch not in self.names:
            self.names[batch] = self.sequence
            self.sequence += 1
        return {"batch": self.names[batch] if batch is not None else None}

    def live(self):
        return list(self.names.values())
