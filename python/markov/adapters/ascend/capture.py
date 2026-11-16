"""Own Ascend device visibility and the native driver boundary."""

import json
import re
import subprocess
import time


MAX_IDLE_POLL_SEC = 15.0


class AscendDevices:
    def __init__(self, devices: tuple[int, ...]):
        self.devices = devices

    def snapshot(self) -> str:
        return subprocess.check_output(["npu-smi", "info"], text=True, timeout=20)

    def has_container_owner(self) -> bool:
        containers = subprocess.check_output(["docker", "ps", "-q"], text=True, timeout=20).split()
        if not containers:
            return False

        # A loading service owns its devices before the driver lists a process.
        result = subprocess.run(["docker", "inspect", *containers], text=True, capture_output=True, timeout=20)
        for container in json.loads(result.stdout):
            if not container["State"]["Running"] or container["HostConfig"]["Runtime"] != "ascend":
                continue
            environment = dict(value.split("=", 1) for value in container["Config"]["Env"] or ())
            for key in ("ASCEND_VISIBLE_DEVICES", "ASCEND_RT_VISIBLE_DEVICES"):
                visible = environment.get(key, "").split(",")
                if "all" in visible or any(str(device) in visible for device in self.devices):
                    return True
        return False

    def wait_idle(self, deadline_sec: float) -> str:
        delay_sec = 1.0
        while True:
            state = self.snapshot()
            if (
                all(re.search(rf"No running processes found in NPU {device}\b", state) for device in self.devices)
                and not self.has_container_owner()
            ):
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
