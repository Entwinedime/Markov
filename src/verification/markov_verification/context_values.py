"""Share immutable context values within one conditions file or execution trace."""

import json


class ContextValues:
    snapshots = {"context", "entry_context", "exit_context", "observation_context"}
    frames = {"work", "continuation"}

    def __init__(self):
        self.values = []
        self.indices = {}

    @classmethod
    def transform(cls, frame, convert):
        if not isinstance(frame, dict):
            return frame
        mapped = frame.copy()
        for name, value in frame.items():
            if name in cls.snapshots and isinstance(value, dict):
                mapped[name] = {key: convert(field) for key, field in value.items()}
            elif name == "callers" and isinstance(value, list):
                mapped[name] = [cls.transform(caller, convert) for caller in value]
            elif name in cls.frames:
                mapped[name] = cls.transform(value, convert)
        return mapped

    def intern(self, value):
        key = json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False)
        if key not in self.indices:
            self.indices[key] = len(self.values)
            self.values.append(value)
        return self.indices[key]

    def encode(self, frame):
        return self.transform(frame, self.intern)

    def declare(self, record):
        if type(record["index"]) is not int or record["index"] != len(self.values):
            raise ValueError("execution trace context values must be declared in reference order")
        self.values.append(record["value"])

    def resolve(self, index):
        if type(index) is not int or not 0 <= index < len(self.values):
            raise ValueError("execution context refers to an undeclared value")
        return self.values[index]

    def decode(self, frame):
        return self.transform(frame, self.resolve)
