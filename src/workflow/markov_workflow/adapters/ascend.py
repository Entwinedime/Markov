"""Bind declared physical resources to one native Ascend capture container."""

from pathlib import Path


class AscendDeployment:
    def __init__(self, source: dict, workers: int, transfer: bool) -> None:
        if source.keys() - {"physical_devices", "store_url", "transfer_protocol"}:
            raise ValueError("Ascend deployment contains unsupported fields")
        devices = source["physical_devices"]
        if any(type(device) is not int or device < 0 for device in devices) or devices != sorted(set(devices)):
            raise ValueError("Ascend physical_devices must be distinct nonnegative integers in increasing order")
        if len(devices) < workers:
            raise ValueError(f"native protocol requires {workers} physical devices, got {len(devices)}")
        self.devices = devices[:workers]
        self.environment = {"ASCEND_RT_VISIBLE_DEVICES": ",".join(map(str, self.devices)), "OMP_NUM_THREADS": "1"}
        if transfer and not source.get("store_url"):
            raise ValueError("Ascend PD capture needs a reachable store_url")
        if "transfer_protocol" in source and "store_url" not in source:
            raise ValueError("transfer_protocol needs a store_url")
        if "store_url" in source:
            protocol = source.get("transfer_protocol", "sdma")

            # The native engine silently falls back to SDMA for an unknown protocol.
            if protocol not in ("sdma", "device_rdma"):
                raise ValueError("Ascend transfer_protocol must be sdma or device_rdma")

            self.environment["ASCEND_MF_STORE_URL"] = source["store_url"]
            self.environment["ASCEND_MF_TRANSFER_PROTOCOL"] = protocol

    def options(self) -> list[str]:
        arguments = ["--runtime", "ascend", "--privileged", "--ipc", "host", "--network", "host"]
        for path in ("/usr/local/Ascend/driver", "/etc/ascend_install.info", "/etc/hccn.conf"):
            source = Path(path).resolve(strict=True)
            arguments.extend(["--mount", f"type=bind,src={source},dst={path},readonly"])
        for name, value in self.environment.items():
            arguments.extend(["--env", f"{name}={value}"])
        return arguments
