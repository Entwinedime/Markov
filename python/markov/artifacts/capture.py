"""Resource and cumulative-use limits for a real base capture."""

from __future__ import annotations

from dataclasses import dataclass

from markov.artifacts.request import positive_integer, reject_unknown, required_text


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
        limits = raw["budget"]
        fields = {"starts", "wall_sec", "requests", "input_tokens", "output_tokens", "retained_mib"}
        reject_unknown(limits, fields, "capture budget")
        budget = {key: positive_integer(limits[key], f"capture.budget.{key}") for key in sorted(fields)}
        return cls(
            required_text(raw["image"], "capture.image"),
            tuple(devices),
            positive_integer(raw["memory_limit_gib"], "capture.memory_limit_gib"),
            positive_integer(raw["cpus"], "capture.cpus"),
            budget,
        )
