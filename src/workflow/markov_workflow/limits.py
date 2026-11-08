"""Bound one external operation and monitor storage owned by its collection."""

import os
import subprocess
import time


class OperationLimit:
    def __init__(self, root=None, storage_bytes=None):
        self.root, self.storage_bytes = root, storage_bytes

    def bytes_used(self):
        total = 0
        for directory, _, files in os.walk(self.root):
            for name in files:
                try:
                    total += os.lstat(os.path.join(directory, name)).st_size
                except FileNotFoundError:

                    # Atomic publication and SDK cleanup can remove a listed file before stat.
                    continue
        return total

    def check(self):
        if self.root is not None and self.bytes_used() > self.storage_bytes:
            raise RuntimeError("collection storage allowance exhausted")

    def run(self, command, log, timeout):
        self.check()
        deadline = time.monotonic() + timeout
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        try:
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise subprocess.TimeoutExpired(command, timeout)
                try:
                    return process.wait(timeout=min(1, remaining) if self.root is not None else remaining)
                except subprocess.TimeoutExpired:
                    self.check()
        finally:
            if process.poll() is None:
                process.kill()
            process.wait()
