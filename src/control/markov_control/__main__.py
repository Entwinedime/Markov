"""Native control endpoint; its standard streams connect to markov simulate --control native."""

import argparse
import os
import sys
from pathlib import Path

from markov_control.adapters.runtimes.cpu import CpuDeclarations
from markov_control.peer import ControlPeer


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel-python", type=Path, required=True)
    arguments = parser.parse_args()

    # Native libraries also write directly to fd 1; reserve a separate descriptor for the protocol.
    with os.fdopen(os.dup(sys.stdout.fileno()), "w", buffering=1) as protocol:
        os.dup2(sys.stderr.fileno(), sys.stdout.fileno())
        os.environ["SGLANG_USE_CPU_ENGINE"] = "1"
        os.environ["OMP_NUM_THREADS"] = "1"
        CpuDeclarations.install(arguments.kernel_python)

        from markov_control.adapters.frameworks.sglang.workers import NativeWorkers

        NativeWorkers.run(ControlPeer(sys.stdin, protocol), arguments.kernel_python)


if __name__ == "__main__":
    main()
