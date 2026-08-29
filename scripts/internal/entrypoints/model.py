#!/usr/bin/env python3
"""Container entry point for one generated C++ modeling run."""

from __future__ import annotations

import sys
from pathlib import Path


INTERNAL_ROOT = Path(__file__).resolve().parents[1]
if str(INTERNAL_ROOT) not in sys.path:
    sys.path.insert(0, str(INTERNAL_ROOT))

from markov_internal.modeling.runner import main as run_modeling  # noqa: E402


def main(argv: list[str] | None = None) -> int:
    """Run one generated C++ modeling config."""

    return run_modeling(argv)


if __name__ == "__main__":
    raise SystemExit(main())
