"""Keep core value identities attached to live native tensor storage, including its views."""

from torch.multiprocessing.reductions import StorageWeakRef


class TensorValues:
    def __init__(self, namespace) -> None:
        self.namespace = namespace
        self.sequence = 0
        self.bindings = {}

    def allocate(self, tensor):
        storage = tensor.untyped_storage()
        entry = self.bindings.get(storage._cdata)
        if entry is None or entry[0].expired():
            binding = {"identity": f"native-storage/{self.namespace}/{self.sequence}", "version": 0}
            self.sequence += 1
        else:
            binding = {**entry[1], "version": entry[1]["version"] + 1}
        self.bindings[storage._cdata] = StorageWeakRef(storage), binding
        return binding

    def alias(self, source, target):
        self.publish({"value": target}, self.inputs({"value": source}))

    def inputs(self, tensors):
        values = {}
        for name, tensor in tensors.items():
            storage = tensor.untyped_storage()
            entry = self.bindings.get(storage._cdata)
            if entry is None or entry[0].expired():
                if tensor.numel() != 0:
                    raise ValueError(f"native tensor has no live core producer: {name}")

                # Empty native buffers carry geometry but require neither transport nor a data producer.
                self.allocate(tensor)
                entry = self.bindings[storage._cdata]
            _, binding = entry
            values[name] = binding
        return values

    def publish(self, tensors, values):
        for name, tensor in tensors.items():
            storage = tensor.untyped_storage()

            # References do not retain native allocations or make their simulated contents ready.
            self.bindings[storage._cdata] = StorageWeakRef(storage), values[name]

    def live(self):
        self.bindings = {key: entry for key, entry in self.bindings.items() if not entry[0].expired()}
        return sorted({entry[1]["identity"] for entry in self.bindings.values()})
