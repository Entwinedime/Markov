# Python Probe

Non-invasive Python runtime instrumentation for semantic events that are not
visible from LD_PRELOAD alone.

Enable it by putting this directory on `PYTHONPATH` and setting:

```bash
TRACE_SIM_PYTHON_PROBE=1
TRACE_SIM_PYTHON_PROBES=sglang.hicache,sglang.kvcacheio
TRACE_SIM_PYTHON_PROBE_OUTPUT=<trace-dir>
TRACE_SIM_PYTHON_PROBE_KEY_MODE=none|hash|raw
TRACE_SIM_PYTHON_PROBE_MAX_KEYS_PER_EVENT=4096
```

`sitecustomize.py` installs an import hook only when the probe is enabled. The
default probe set wraps SGLang HiCache/cache-controller methods and
`sgl_kernel.kvcacheio` transfer helpers, then emits Chrome Trace JSON with
`cat: "hicache"`.

`TRACE_SIM_PYTHON_PROBE_KEY_MODE` defaults to `none`. Use `hash` for base traces
that will feed capacity, eviction, or prefetch-policy what-if replay. `raw` is
intended only for local debugging because it may expose request/cache key
material.

The probe does not modify `third_party/sglang` source. Missing target modules are
ignored so the same package can be present in non-SGLang runs.
