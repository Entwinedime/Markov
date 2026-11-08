"""Capture semantic calls and their boundary facts without recording tensor operations."""

from __future__ import annotations

import threading
from contextvars import ContextVar, Token
from typing import Any


class CapturedCall:
    def __init__(self, capture: SemanticCapture, record: dict[str, Any], device: bool, stream: Any) -> None:
        self.capture = capture
        self.record = record
        self.identity = record["id"]
        self.device = device
        self.stream = stream

    def __enter__(self) -> CapturedCall:
        self.parent: Token = self.capture.current.set(self.identity)
        try:
            if self.device:
                self.record["device_entry"] = self.capture.runtime.point(self.identity, "entry", self.stream)
            if "service" in self.record:
                self.record["host_interval"] = {}
                self.native = self.capture.runtime.service_scope(self.record["host_interval"])
            else:
                self.native = self.capture.runtime.scope(f"markov.scope/{self.identity}")
            self.native.__enter__()
        except BaseException:
            self.capture.current.reset(self.parent)
            raise
        return self

    def __exit__(self, kind: Any, error: Any, traceback: Any) -> None:
        try:
            self.native.__exit__(kind, error, traceback)
            if error is None and self.device:
                self.record["device_return"] = self.capture.runtime.point(self.identity, "return", self.stream)
            self.record["complete"] = error is None
        finally:
            self.capture.current.reset(self.parent)

    def port(self, name: str, direction: str, tensor: dict[str, Any] | None, *, use: str) -> None:
        if direction not in {"input", "output"} or use not in {"host", "device", "metadata", "state"}:
            raise ValueError("a boundary port needs a direction and an explicit use")
        self.record["ports"].append({"name": name, "direction": direction, "tensor": tensor, "use": use})

    def output(self, name: str, tensor: dict[str, Any], *, use: str, preceding: list[int]) -> None:
        writer = tensor["writer"]
        if writer in preceding:

            # A private return value may be created before an intervening child call; retain its actual producer.
            if tensor["allocation"] != writer:
                raise ValueError("a deferred mutation needs an explicit input/output boundary at its writer")
            self.capture.scopes[writer]["ports"].append({
                "name": name, "direction": "output", "tensor": tensor, "use": use,
            })
            self.port(name, "input", tensor, use=use)
        self.port(name, "output", tensor, use=use)

    def signal(self, name: str, direction: str, point: int) -> None:
        if direction not in {"input", "output"}:
            raise ValueError("a completion signal needs an input or output direction")
        self.record["ports"].append({"name": name, "direction": direction, "use": "signal", "point": point})

    def work_signal(self, name: str, direction: str, producer: int) -> None:
        if direction not in {"input", "output"}:
            raise ValueError("a work completion needs an input or output direction")

        # The runtime resolves the Work's actual end event from its native wait, not from Python return time.
        self.record["ports"].append({"name": name, "direction": direction, "use": "signal", "producer": producer})


class SemanticCapture:
    def __init__(self, runtime: Any) -> None:
        self.runtime = runtime
        self.current: ContextVar[int] = ContextVar("semantic_call", default=0)
        self.lock = threading.Lock()
        self.closed = False
        self.scopes: list[dict[str, Any]] = [{
            "id": 0, "parent": None, "role": "window", "site": "", "implementation": "",
            "parameters": {}, "constants": {}, "arguments": {}, "work": {}, "bindings": {},
            "ports": [], "complete": True,
        }]

    def call(
        self, role: str, site: str, implementation: str, *,
        parameters: dict[str, int] | None = None,
        constants: dict[str, int] | None = None,
        arguments: dict[str, Any] | None = None,
        work: dict[str, Any] | None = None,
        bindings: dict[str, Any] | None = None,
        device: bool = False,
        stream: Any = None,
        parent: int | None = None,
        service: dict[str, Any] | None = None,
    ) -> CapturedCall:
        parent = self.current.get() if parent is None else parent
        enclosing = self.scopes[parent]

        # Inheritance is by declared parameter name; no observed dimension is matched by its numeric value.
        inherited = parameters is None
        if constants is None:
            fixed = dict(enclosing["constants"]) if inherited else {}
        else:
            fixed = dict(constants)
        supplied = dict(enclosing["parameters"] if inherited else parameters)
        for name, value in fixed.items():
            if name in supplied and supplied[name] != value:
                raise ValueError(f"fixed parameter differs from the native call: {site}:{name}")
            supplied[name] = value
        relations = {} if arguments is None else dict(arguments)
        if inherited and arguments is None:
            relations = {name: name for name in supplied if name not in fixed}
        if any(not isinstance(value, int) or isinstance(value, bool) for value in supplied.values()):
            raise TypeError("semantic parameters must be integers")

        with self.lock:
            if self.closed:
                raise RuntimeError("semantic observation is already closed")
            record = {
                "id": len(self.scopes), "parent": parent, "role": role, "site": site,
                "implementation": implementation, "parameters": supplied, "constants": fixed,
                "arguments": relations, "work": dict(enclosing["work"] if work is None else work),
                "bindings": {} if bindings is None else dict(bindings), "ports": [], "complete": False,
                "thread": threading.get_native_id(),
            }
            if service is not None:
                record["service"] = service
            self.scopes.append(record)
        return CapturedCall(self, record, device, stream)

    def document(self, environment: dict[str, Any]) -> dict[str, Any]:
        with self.lock:
            if self.current.get() != 0 or any(not scope["complete"] for scope in self.scopes):
                raise RuntimeError("native observation contains an unfinished semantic call")
            self.closed = True
            return {"environment": environment, "scopes": self.scopes, "points": self.runtime.points}
