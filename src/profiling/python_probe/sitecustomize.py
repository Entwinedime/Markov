"""Environment-gated entry point for trace-sim Python probes.

Put src/profiling/python_probe on PYTHONPATH and set TRACE_SIM_PYTHON_PROBE=1 to
enable this hook. Import failures are deliberately swallowed so normal runtime
startup is unchanged when the probe package is incomplete or disabled.
"""

try:
    from trace_sim_probe.bootstrap import bootstrap

    bootstrap()
except Exception:
    pass
