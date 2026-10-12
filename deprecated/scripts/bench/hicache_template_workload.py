#!/usr/bin/env python3
"""Run the shared workload CLI from the repository checkout."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "internal"))

from markov_internal.workload_template.cli import main  # noqa: E402

if __name__ == "__main__":
    raise SystemExit(main())
