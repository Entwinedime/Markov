"""Resource and cumulative-use limits for a real base capture."""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path

from markov.artifacts.request import positive_integer, reject_unknown, required_text


def parse_budget(raw: dict) -> dict[str, int]:
    fields = {"starts", "wall_sec", "requests", "input_tokens", "output_tokens", "retained_mib"}
    reject_unknown(raw, fields, "capture budget")
    return {key: positive_integer(raw[key], f"budget.{key}") for key in sorted(fields)}


@dataclass(frozen=True)
class DeviceCommand:
    module: str
    arguments: tuple[str, ...]
    payload: dict
    artifact: str
    result_key: str
    mounts: tuple[tuple[Path, str, bool], ...] = ()
    environment: dict[str, str] = field(default_factory=dict)
    requests: int = 0
    input_tokens: int | None = 0
    output_tokens: int = 0
    usage_file: str | None = None
    measurement_adapter: str | None = None


@dataclass(frozen=True)
class CaptureSettings:
    image: str
    devices: tuple[int, ...]
    memory_limit_gib: int
    cpus: int
    budget: dict[str, int]

    @classmethod
    def parse(cls, raw: dict):
        reject_unknown(raw, {"image", "devices", "memory_limit_gib", "cpus", "budget"}, "capture")
        devices = raw["devices"]
        if (
            not isinstance(devices, list)
            or not devices
            or any(type(device) is not int or device < 0 for device in devices)
            or len(set(devices)) != len(devices)
        ):
            raise ValueError("capture.devices must be unique nonnegative device indices")
        budget = parse_budget(raw["budget"])
        return cls(
            required_text(raw["image"], "capture.image"),
            tuple(devices),
            positive_integer(raw["memory_limit_gib"], "capture.memory_limit_gib"),
            positive_integer(raw["cpus"], "capture.cpus"),
            budget,
        )
