#!/usr/bin/env python3
import runpy
from pathlib import Path

runpy.run_path(str(Path(__file__).resolve().parent / "common" / "merge_all_traces.py"), run_name="__main__")
