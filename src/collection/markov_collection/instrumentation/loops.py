"""Observe a native Python loop without replacing its statements or iteration order."""

from __future__ import annotations

import ast
import inspect
import sys
import textwrap
from typing import Any


class NativeLoop:
    def __init__(self, original: Any) -> None:
        original = inspect.unwrap(original)
        self.code = original.__code__
        source, start = inspect.getsourcelines(original)
        loop = next(node for node in ast.walk(ast.parse(textwrap.dedent("".join(source)))) if isinstance(node, ast.For))
        self.header = start + loop.lineno - 1
        self.end = start + loop.end_lineno - 1
        self.active_part: Any = None
        self.active_sequence: Any = None
        self.error: tuple = (None, None, None)

    def __enter__(self) -> NativeLoop:
        self.previous = sys.gettrace()
        sys.settrace(self.trace)
        return self

    def close_part(self, error: tuple = (None, None, None)) -> None:
        if self.active_part is not None:
            part, self.active_part = self.active_part, None
            part.__exit__(*error)

    def close_sequence(self, error: tuple = (None, None, None)) -> None:
        if self.active_sequence is not None:
            sequence, self.active_sequence = self.active_sequence, None
            sequence.__exit__(*error)

    def __exit__(self, *error: Any) -> None:
        sys.settrace(self.previous)
        try:
            self.close_part(error)
        finally:
            self.close_sequence(error)

    def trace(self, frame: Any, event: str, result: Any) -> Any:
        if frame.f_code is not self.code:
            return None
        if event == "line":
            self.error = (None, None, None)
        if event == "call":
            self.active_part = self.part("prefix", frame)
            self.active_part.__enter__()
        elif event == "line" and frame.f_lineno == self.header:
            self.close_part()
            if self.active_sequence is None:
                self.active_sequence = self.sequence(frame)
                self.active_sequence.__enter__()
        elif event == "line" and self.active_sequence is not None:
            if frame.f_lineno > self.end:
                self.close_part()
                self.close_sequence()
                self.active_part = self.part("suffix", frame)
                self.active_part.__enter__()
            elif self.active_part is None:
                self.active_part = self.part("member", frame)
                self.active_part.__enter__()
        elif event == "exception":
            self.error = result
        elif event == "return":
            self.close_part(self.error)
            self.close_sequence(self.error)

        return self.trace

    def part(self, kind: str, frame: Any) -> Any:
        raise NotImplementedError

    def sequence(self, frame: Any) -> Any:
        raise NotImplementedError
