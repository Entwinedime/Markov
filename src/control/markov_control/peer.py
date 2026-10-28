"""Local JSON framing shared by core transport and native worker call interfaces."""

import json
import os
from typing import TextIO


class ControlPeer:
    def __init__(self, reader: TextIO, writer: TextIO) -> None:
        self.reader = reader
        self.writer = writer
        self.buffer = b""

    def buffered(self) -> bool:
        return b"\n" in self.buffer

    def receive(self):
        while not self.buffered():
            data = os.read(self.reader.fileno(), 65536)
            if not data:
                raise EOFError("Core execution disconnected")
            self.buffer += data
        line, self.buffer = self.buffer.split(b"\n", 1)
        return json.loads(line)

    def send(self, message) -> None:
        self.writer.write(json.dumps(message, separators=(",", ":")) + "\n")
        self.writer.flush()

    def call(self, kind: str, **parameters):
        self.send({"kind": kind, **parameters})
        return self.receive()
