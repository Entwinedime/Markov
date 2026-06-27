#!/usr/bin/env python3
"""Entrypoint for HiCache transition exactness diagnostics."""

from __future__ import annotations

import sys
from pathlib import Path


INTERNAL_ROOT = Path(__file__).resolve().parents[1]
if str(INTERNAL_ROOT) not in sys.path:
    sys.path.insert(0, str(INTERNAL_ROOT))

from markov_internal.hicache.transition import main  # noqa: E402


if __name__ == "__main__":
    raise SystemExit(main())
