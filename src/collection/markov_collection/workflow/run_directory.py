"""Paths and process identity owned by one user run."""

from __future__ import annotations

import re
import shutil
from datetime import datetime, timezone
from pathlib import Path
from uuid import uuid4


class RunDirectory:
    def __init__(self, root: Path, identity: str) -> None:
        self.root = root
        self.identity = identity

    @classmethod
    def create(cls, parent: Path, framework: str, model: str) -> RunDirectory:
        identity = uuid4().hex
        purpose = re.sub(r"[^a-z0-9_-]+", "-", f"{framework}-{Path(model).name}".lower()).strip("-")
        # Profiler paths reject '+'. UTC keeps the timezone explicit using SDK-safe characters.
        name = f"{datetime.now(timezone.utc):%Y%m%dT%H%M%S.%fZ}_{purpose}"
        root = parent / name
        try:
            root.mkdir(parents=True)
        except FileExistsError:
            root = parent / f"{name}_{identity}"
            root.mkdir()
        return cls(root, identity)

    @property
    def inputs(self) -> Path:
        return self.root / "inputs"

    @property
    def catalogs(self) -> Path:
        return self.root / "assets/catalogs"

    def capture(self, role: str, ordinal: int) -> Path:
        return self.root / "captures" / role / f"{ordinal:03d}"

    def work(self, kind: str, ordinal: int) -> Path:
        return self.root / "work" / kind / f"{ordinal:03d}"

    def asset(self, kind: str, ordinal: int) -> Path:
        return self.root / "assets" / kind / f"{ordinal:03d}"

    def diagnostics(self, kind: str, ordinal: int) -> Path:
        return self.root / "diagnostics" / kind / f"{ordinal:03d}"

    def log(self, task: str) -> Path:
        return self.root / "logs" / f"{task}.log"

    def container(self, task: str) -> str:
        # Process names are global; output directories and user labels are not.
        return f"markov-{self.identity}-{task}"

    def consume_work(self, kind: str, ordinal: int) -> None:
        """Remove one acknowledged handoff; never infer completion from files."""
        path = self.work(kind, ordinal)
        if path.exists():
            shutil.rmtree(path)
        for parent in (path.parent, self.root / "work"):
            if parent.exists() and not any(parent.iterdir()):
                parent.rmdir()
