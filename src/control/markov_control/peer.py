"""A synchronous local channel preserves the native call stack while core work advances."""

import json
from typing import TextIO


class ControlPeer:
    def __init__(self, reader: TextIO, writer: TextIO) -> None:
        self.reader = reader
        self.writer = writer

    def receive(self):
        line = self.reader.readline()
        if not line:
            raise EOFError("Core execution disconnected")
        return json.loads(line)

    def send(self, message) -> None:
        self.writer.write(json.dumps(message, separators=(",", ":")) + "\n")
        self.writer.flush()

    def call(self, kind: str, **parameters):
        self.send({"kind": kind, **parameters})
        return self.receive()
