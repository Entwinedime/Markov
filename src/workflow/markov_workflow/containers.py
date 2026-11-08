"""Release only the named containers owned by one application invocation."""

import subprocess


class OwnedContainers:
    def __init__(self, names) -> None:
        self.names = set(names)

    def remaining(self) -> set[str]:
        names = subprocess.check_output(["docker", "ps", "--all", "--format", "{{.Names}}"], text=True)
        return self.names.intersection(names.splitlines())

    def close(self) -> None:
        remaining = self.remaining()
        if not remaining:
            return
        result = subprocess.run(["docker", "rm", "--force", *sorted(remaining)], capture_output=True, text=True)
        if result.returncode:

            # An --rm container may disappear between listing and removal; daemon failures must remain failures.
            remaining = self.remaining()
            if remaining:
                raise RuntimeError(f"failed to remove owned containers {sorted(remaining)}: {result.stderr.strip()}")
