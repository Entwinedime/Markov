"""Read allocatable device memory in a short-lived native process before loading."""

import json
import subprocess
import sys


def device_memory(participants: int) -> list[dict[str, int]]:
    # The child releases its contexts before the inference service starts.
    source = """
import json
import sys
import torch
import torch_npu

rows = []
for device in range(int(sys.argv[1])):
    free, total = torch.npu.mem_get_info(device)
    rows.append({'device': device, 'free_bytes': free, 'total_bytes': total})

print(json.dumps(rows))
"""
    result = subprocess.check_output([sys.executable, "-c", source, str(participants)], text=True, timeout=90)
    return json.loads(result.splitlines()[-1])
