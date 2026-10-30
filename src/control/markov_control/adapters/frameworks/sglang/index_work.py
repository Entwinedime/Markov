"""Describe native cache tensor work without duplicating the core's prefix or page ledger."""

from contextlib import contextmanager

import torch
from torch.overrides import TorchFunctionMode
from torch.utils.weak import WeakIdKeyDictionary


class IndexTrace(TorchFunctionMode):
    def __init__(self, peer, request_map, empty) -> None:
        super().__init__()
        self.peer = peer
        self.request_map = request_map
        self.handles = WeakIdKeyDictionary({empty: 0})
        self.sequence = 1
        self.depth = 0
        self.recording = True
        self.request = None
        self.operations = []
        self.effects = {}
        self.result_tail = None

    def value(self, tensor):
        return self.handles[tensor]

    def output(self, tensor):
        handle = self.sequence
        self.sequence += 1
        self.handles[tensor] = handle
        return handle

    def document(self):
        return {"operations": self.operations, "live_indices": sorted(self.handles.values()), **self.effects}

    @contextmanager
    def metadata(self):

        # Page IDs describe resource effects; extracting them is not native device work.
        previous = self.recording
        self.recording = False
        try:
            yield
        finally:
            self.recording = previous

    @contextmanager
    def service(self, name, request=None):
        outer = self.depth == 0
        if outer:
            self.operations, self.effects = [], {}
            self.request = request
            self.__enter__()
        self.depth += 1
        self.operations.append({"kind": "call", "service": name, "enter": True})
        try:
            yield
            self.operations.append({"kind": "call", "service": name, "enter": False})
        except BaseException:
            if outer:
                self.__exit__(None, None, None)
            raise
        else:
            if outer:
                self.__exit__(None, None, None)
                self.peer.call("index_work", **self.document())
        finally:
            self.depth -= 1
            if outer:
                self.request = None

    def prefix(self, request):
        self.effects["prefix"] = {"request": int(request.rid), "indices": self.value(request.prefix_indices)}

    def release(self, tensors, pages, mode, pending_tokens=0):
        operation = {
            "kind": "release",
            "mode": mode,
            "inputs": [self.value(tensor) for tensor in tensors],
            "pages": pages,
            "pending_tokens_before": pending_tokens,
        }
        if mode == "group":

            # Native publication precedes this final Decode update; expand it at the result tail.
            self.result_tail = {"operations": [operation], "live_indices": sorted(self.handles.values())}
        else:
            self.operations.append(operation)

    def __torch_function__(self, function, types, args=(), kwargs=None):
        result = function(*args, **(kwargs or {}))
        if not self.recording:
            return result

        if function is torch.Tensor.__getitem__:
            tensor, index = args
            bounds = index[1] if tensor is self.request_map else index
            begin, end, step = bounds.indices(tensor.shape[-1])
            if step != 1:
                raise ValueError("cache work requires contiguous index slices")
            operation = {"output": self.output(result), "begin": begin, "tokens": max(0, end - begin)}
            if tensor is self.request_map:
                operation.update(kind="read", request=int(self.request.rid))
            else:
                operation.update(kind="slice", input=self.value(tensor))
        elif function is torch.Tensor.__setitem__:
            tensor, index, payload = args
            if tensor is not self.request_map:
                raise ValueError("cache work can only write the request map")
            begin, end, step = index[1].indices(tensor.shape[-1])
            if step != 1 or end - begin != payload.numel():
                raise ValueError("cache map writes require their complete contiguous interval")
            operation = {
                "kind": "write",
                "request": int(self.request.rid),
                "input": self.value(payload),
                "begin": begin,
                "tokens": end - begin,
            }
        elif function in (torch.Tensor.to, torch.Tensor.clone):
            operation = {"kind": "copy", "input": self.value(args[0]), "output": self.output(result)}
        elif function is torch.cat:
            operation = {
                "kind": "concat",
                "inputs": [self.value(tensor) for tensor in args[0]],
                "output": self.output(result),
            }
        else:
            return result
        self.operations.append(operation)
        return result
