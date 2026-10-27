"""Ascend measurement waiting shared by application and research captures."""

from pathlib import Path
import re
import time

from markov_internal.common.io import load_json, write_json
from markov_internal.common.process import run_command

# Let HCCL listeners close after other applications exit.
RESOURCE_RECHECK_SECONDS = 30


def wait_for_normal_resources(output: Path) -> None:
    """Wait for other NPU applications; record waiting separately from captures."""

    started = time.monotonic()
    started_at_unix = time.time()
    waited = False
    idle_since = None
    try:
        while True:
            observed = run_command(["npu-smi", "info"], capture_output=True)
            observed.check_returncode()
            process_table = observed.stdout.split("Process id", 1)[1]
            busy = re.findall(r"^\|\s*(\d+)\s+\d+\s*\|\s*(\d+)\s*\|", process_table, re.MULTILINE)
            now = time.monotonic()
            if not busy:
                if not waited:
                    return
                if idle_since is None:
                    idle_since = now
                elif now - idle_since >= RESOURCE_RECHECK_SECONDS:
                    break
            else:
                idle_since = None
                if not waited:
                    print(f"Normal capture waiting for NPU applications (device, PID): {busy}", flush=True)
                waited = True

            # shortcut: This checks occupancy; use a scheduler reservation when exclusivity must be enforced.
            time.sleep(RESOURCE_RECHECK_SECONDS)
    finally:
        if waited:
            path = output / "resource_wait.json"
            ledger = load_json(path) if path.exists() else {"wall_seconds": 0.0, "intervals": []}
            wall_seconds = time.monotonic() - started
            ledger["intervals"].append({"started_at_unix": started_at_unix, "wall_seconds": wall_seconds})
            ledger["wall_seconds"] += wall_seconds
            write_json(path, ledger)

    print("NPU applications exited; normal capture continues.", flush=True)
