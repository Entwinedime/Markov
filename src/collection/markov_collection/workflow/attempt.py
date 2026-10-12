"""Own the native job, device lease and deadline inside its dedicated container."""

from __future__ import annotations

import argparse
import fcntl
import json
import os
import signal
import subprocess
import time
from pathlib import Path
from typing import Any

from markov_collection.artifacts.json_file import write_json


class NativeAttempt:
    def __init__(self, configuration: dict[str, Any], directory: Path) -> None:
        self.configuration = configuration
        self.directory = directory
        self.root = Path(__file__).resolve().parents[4]
        self.started_ns: int | None = None
        self.boot_ns = time.time_ns() - configuration["created_at_ns"]

        self.record: dict[str, Any] = {
            "status": "queued",
            "queued_at_ns": time.time_ns(),
            "boot_wall_time_ns": self.boot_ns,
            "admitted_at_ns": None,
            "ended_at_ns": None,
            "elapsed_wall_time_ns": None,
            "return_code": None,
        }

    def _interrupt(self, signum: int, frame: Any) -> None:
        raise KeyboardInterrupt

    def _save(self) -> None:
        write_json(self.directory / "attempt_status.json", self.record)

    def _stop_workers(self) -> None:
        # kill(-1) reaches all workers in this private PID namespace, including
        # new sessions and orphan descendants; Linux excludes the caller/PID 1.
        try:
            os.kill(-1, signal.SIGKILL)
        except ProcessLookupError:
            pass

        while True:
            try:
                os.waitpid(-1, 0)
            except ChildProcessError:
                return

    def _command(self) -> list[str]:
        build = self.root / "build/collection/ascend" / self.configuration["framework"]
        role = self.configuration["role"]
        setup = (
            'cmake -S src/collection/adapters/runtimes/ascend/native -B "$3" -DCMAKE_BUILD_TYPE=Release && '
            'cmake --build "$3" -j2 && '
            "python3 -m pip install --no-deps --no-build-isolation -e . && "
            'export LD_PRELOAD="$3/libmarkov_native_observation.so${LD_PRELOAD:+:$LD_PRELOAD}" && '
        )
        if role in {"base", "validation"} or self.configuration["measurement_kind"] in {"device", "communication"}:
            setup += 'export MARKOV_NATIVE_OBSERVATIONS="$2/native_api" && '

        module = (
            "markov_collection.workflow.capture"
            if role in {"base", "validation"}
            else "markov_collection.adapters.frameworks.sglang.sampling"
        )
        if role == "performance" and self.configuration["measurement_kind"] == "communication":
            module = "markov_collection.adapters.runtimes.ascend.collective_sampling"

        return self.configuration["native_entrypoint"] + [
            "bash",
            "-lc",
            setup + 'exec python3 -m "$4" --input "$1" --output "$2"',
            "markov-capture",
            str(self.directory / "prepared_input.json"),
            str(self.directory),
            str(build),
            module,
        ]

    def run(self) -> int:
        if os.getpid() != 1:
            raise RuntimeError("native attempt requires PID 1 in a dedicated container with a private PID namespace")

        signal.signal(signal.SIGTERM, self._interrupt)
        signal.signal(signal.SIGINT, self._interrupt)
        self._save()

        with (self.root / "data/collection.lock").open("a", encoding="utf-8") as lease:
            try:
                fcntl.flock(lease, fcntl.LOCK_EX)

                # Waiting for the device lease is outside the admitted execution budget.
                self.started_ns = time.perf_counter_ns()
                self.record.update(status="collecting", admitted_at_ns=time.time_ns())
                self._save()

                remaining_ns = self.configuration["remaining_wall_time_ns"] - self.boot_ns
                if remaining_ns <= 0:
                    raise subprocess.TimeoutExpired("container bootstrap", 0)

                with (self.directory / "process.log").open("w", encoding="utf-8") as log:
                    process = subprocess.Popen(self._command(), stdout=log, stderr=subprocess.STDOUT)
                    deadline_ns = self.started_ns + remaining_ns
                    code = process.wait(timeout=max(0, deadline_ns - time.perf_counter_ns()) / 1_000_000_000)

                self.record.update(status="completed" if code == 0 else "failed", return_code=code)
            except subprocess.TimeoutExpired:
                self.record.update(status="budget_exhausted", return_code=124)
            except KeyboardInterrupt:
                self.record.update(status="interrupted", return_code=130)
            except Exception as error:
                # Persist a monitor/bootstrap failure before releasing its lease.
                self.record.update(status="failed", return_code=1, error=str(error))
            finally:
                # Do not release the device lease while orphan workers remain.
                signal.signal(signal.SIGTERM, signal.SIG_IGN)
                signal.signal(signal.SIGINT, signal.SIG_IGN)
                self._stop_workers()

                elapsed_ns = self.boot_ns
                if self.started_ns is not None:
                    elapsed_ns += time.perf_counter_ns() - self.started_ns
                self.record.update(elapsed_wall_time_ns=elapsed_ns, ended_at_ns=time.time_ns())
                self._save()

        return self.record["return_code"]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--attempt", type=Path, required=True)
    arguments = parser.parse_args()

    with arguments.attempt.open(encoding="utf-8") as stream:
        configuration = json.load(stream)

    raise SystemExit(NativeAttempt(configuration, arguments.attempt.parent).run())


if __name__ == "__main__":
    main()
