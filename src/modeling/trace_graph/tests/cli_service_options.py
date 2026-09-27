"""Check public service-preparation CLI constraints without loading a trace."""

import subprocess
import sys


def check(binary):
    pair = ["--prepare-cpu-service", "measurements.json", "--cpu-service-output", "service.json"]
    cases = [
        (["--prepare-cpu-service", "measurements.json"], "must be supplied together"),
        (["--cpu-service-output", "service.json"], "must be supplied together"),
        (pair + ["--model-config", "target.json"], "not target models"),
        (pair + ["--cpu-service-cost", "existing.json"], "not target models"),
        (["--source-observations-only", "--model-config", "target.json"], "cannot be combined"),
        (["--source-observations-only", "--graph-output", "graph.json"], "cannot be combined"),
        ([], "--run-summary is required"),
    ]
    for options, expected in cases:
        result = subprocess.run(
            [binary, "--profile-manifest", "unused.json", *options], capture_output=True, text=True, timeout=10
        )
        if result.returncode != 2 or expected not in result.stderr:
            raise AssertionError((options, result.returncode, result.stderr))
    result = subprocess.run([binary, "--help"], capture_output=True, text=True, timeout=10)
    if result.returncode or "--prepare-cpu-service" not in result.stdout:
        raise AssertionError("CPU preparation is missing from public help")
    print(f"CPU service CLI constraints passed: {binary} ({len(cases)} rejection cases + help)")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit("usage: cli_service_options.py <trace_graph> [<trace_graph> ...]")
    for executable in sys.argv[1:]:
        check(executable)
