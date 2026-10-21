"""Ascend device visibility and native observation-library launch setup."""

from pathlib import Path
from typing import Any


class AscendCaptureRuntime:
    @staticmethod
    def device_environment(devices: list[int]) -> dict[str, str]:
        visible = ",".join(str(device) for device in devices)
        return {"ASCEND_VISIBLE_DEVICES": visible, "ASCEND_RT_VISIBLE_DEVICES": visible}

    def command(self, root: Path, directory: Path, prepared: dict[str, Any], entrypoint: list[str]) -> list[str]:
        # Native objects cannot cross image ABIs, even when package paths match.
        image_id = prepared["environment"]["image_id"].removeprefix("sha256:")
        build = root / "build/collection/ascend" / prepared["framework"] / image_id
        role = prepared["role"]
        ports = role == "validation" and prepared["observation_purpose"] == "ports"
        setup = ""
        if not ports:
            setup = (
                'cmake -S src/collection/adapters/runtimes/ascend/native -B "$3" -DCMAKE_BUILD_TYPE=Release && '
                'cmake --build "$3" -j2 && '
            )

        # Port observation keeps the native lifecycle without loading device
        # instrumentation; other modes use the same installed runtime library.
        setup += "python3 -m pip install --no-deps --no-build-isolation -e . && "
        if not ports:
            setup += 'export LD_PRELOAD="$3/libmarkov_native_observation.so${LD_PRELOAD:+:$LD_PRELOAD}" && '
            if role in {"base", "validation"} or prepared["measurement_kind"] in {"device", "communication"}:
                setup += 'export MARKOV_NATIVE_OBSERVATIONS="$2/native_api" && '

        module = "markov_collection.workflow.capture"
        if role == "performance" and prepared["measurement_kind"] == "communication":
            module = "markov_collection.adapters.runtimes.ascend.collective_sampling"

        # Positional arguments keep user paths out of the shell program.
        return entrypoint + [
            "bash",
            "-lc",
            setup + 'exec python3 -m "$4" --input "$1" --output "$2"',
            "markov-capture",
            str(directory / "prepared_input.json"),
            str(directory),
            str(build),
            module,
        ]
