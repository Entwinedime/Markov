"""Own Ascend device visibility and the native driver boundary."""

import re
import subprocess
import time


MAX_IDLE_POLL_SEC = 15.0


class AscendDevices:
    def __init__(self, devices: tuple[int, ...]):
        self.devices = devices

    def snapshot(self) -> str:
        return subprocess.check_output(["npu-smi", "info"], text=True, timeout=20)

    def wait_idle(self, deadline_sec: float) -> str:
        delay_sec = 1.0
        while True:
            state = self.snapshot()
            if all(re.search(rf"No running processes found in NPU {device}\b", state) for device in self.devices):
                return state

            remaining_sec = deadline_sec - time.monotonic()
            if remaining_sec <= 0:
                raise ValueError("The device-idle wait exhausted the remaining budget; no capture was started")
            time.sleep(min(delay_sec, remaining_sec))
            delay_sec = min(2 * delay_sec, MAX_IDLE_POLL_SEC)

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
