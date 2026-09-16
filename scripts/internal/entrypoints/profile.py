#!/usr/bin/env python3
"""Container entry point for one profiling run or suite."""

from __future__ import annotations

import sys
from pathlib import Path


ENTRY_DIR = Path(__file__).resolve().parent
# This script is not the stdlib profile module imported by cProfile/Torch.
sys.path[:] = [path for path in sys.path if Path(path).resolve() != ENTRY_DIR]
INTERNAL_ROOT = ENTRY_DIR.parent
if str(INTERNAL_ROOT) not in sys.path:
    sys.path.insert(0, str(INTERNAL_ROOT))

from markov_internal.profiling.runner import main  # noqa: E402


if __name__ == "__main__":
    raise SystemExit(main())
