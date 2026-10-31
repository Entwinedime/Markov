"""Own Ascend device visibility and the native driver boundary."""

import re
import subprocess


class AscendDevices:
    def __init__(self, devices: tuple[int, ...]):
        self.devices = devices

    def snapshot(self) -> str:
        return subprocess.check_output(["npu-smi", "info"], text=True, timeout=20)

    def require_idle(self) -> str:
        state = self.snapshot()
        if any(not re.search(rf"No running processes found in NPU {device}\b", state) for device in self.devices):
            raise ValueError("The declared devices are not confirmed idle; no capture was started")
        return state

    def container_arguments(self) -> list[str]:
        result = ["--runtime", "ascend"]
        for device in (*[f"davinci{device}" for device in self.devices], "davinci_manager", "devmm_svm", "hisi_hdc"):
            result += ["--device", f"/dev/{device}"]
        result += [
            "--mount",
            "type=bind,source=/usr/local/Ascend/driver,target=/usr/local/Ascend/driver,readonly",
            "-e",
            "ASCEND_VISIBLE_DEVICES=" + ",".join(map(str, self.devices)),
            "-e",
            "HCCL_NPU_SOCKET_PORT_RANGE=auto",
        ]
        # Ascend's container runtime maps selected host devices to local IDs.
        # HCCL allocates ports rather than sharing its fixed default with peers.
        return result
