# Hook Framework

This repository contains a lightweight C++ hook framework for dynamic function interception, call-site filtering, argument logging, and optional PAPI-based PMU collection. The current examples are wired for Ascend and CPUInfer targets, but the framework itself is generic.

## What It Does

The hook library is loaded with `LD_PRELOAD`. Each intercepted function resolves the original symbol from a target shared library, checks whether the current caller should be traced, and then executes the real function while recording timing, arguments, and optional performance counters.

The current implementation supports:

- Lazy symbol resolution with `dlopen` and `dlsym`.
- Rule-based caller-to-callee filtering.
- Function argument capture for trace output.
- Optional PAPI event collection when the library is available.
- Chrome trace style JSON output.

## Repository Layout

- `hook.cpp` contains an empty template hook target.
- `targets/` contains active hook targets such as AscendCL wrappers.
- `include/framework/` contains the framework headers.
- `src/` contains the framework implementation.
- The top-level repository `build/` directory contains CMake outputs.

## Build

### Requirements

- CMake 3.16 or newer
- A C++17 compiler
- `dl`
- Optional: PAPI development libraries if you want PMU counters

### Configure and Build

```bash
cmake -S . -B build -DHOOK_ENABLE_PAPI=ON
cmake --build build -j$(nproc)
```

For host-side smoke tests, the shared library is written under the selected CMake
build directory. For profiling, build it inside the runtime container:

```bash
scripts/hooks/build.sh sglang
scripts/hooks/build.sh ktransformers
```

Those commands produce:

- `build/docker/sglang/lib/libhook.so`
- `build/docker/ktransformers/lib/libhook.so`

If PAPI is not available on your machine, build without it:

```bash
cmake -S . -B build -DHOOK_ENABLE_PAPI=OFF
cmake --build build -j$(nproc)
```

From the top-level repository, use:

```bash
cmake -S . -B build -DHOOK_ENABLE_PAPI=OFF
cmake --build build -j$(nproc)
```

## Runtime Output

Trace events are written as JSON to a file in the current working directory by default.

By default, the output file name is based on the current rank and process id:

```text
cpu_trace.json.rank<RANK>.pid<PID>.json
```

You can override the base path with `HOOK_TRACE_OUTPUT`.

The rank suffix is derived from one of these environment variables, in order:

- `RANK_ID`
- `HOROVOD_RANK`
- `OMPI_COMM_WORLD_RANK`

If none of them are set, the rank is reported as `unknown`.

## How Hooking Works

Each target is registered with `HOOKFW_DEFINE_TARGET`, which stores:

- a human-readable trace name,
- the mangled symbol name,
- and the target shared library path.

When the hook wrapper calls `HOOKFW_INVOKE`, the framework:

1. Resolves the original symbol from the target shared library.
2. Registers the resolved function scope for caller filtering.
3. Checks whether the current caller matches the rule map.
4. Records duration, arguments, and PMU deltas if enabled.

If symbol resolution fails, the process exits with a fatal error.

## Rule Filtering

The rule map controls which call sites are traced for a given callee.

- If no rule exists for a callee, tracing is enabled for all callers.
- If a callee has an empty caller list, tracing is also enabled for all callers.
- If callers are configured, the hook only records events when the return address belongs to one of those caller scopes.

Example:

```cpp
HOOKFW_SET_RULE("aclrtStreamWaitEvent",
                "CPUInfer::submit",
                "CPUInfer::sync");
```

## Capturing Arguments

Arguments are passed to `HOOKFW_INVOKE` as a list of name/value pairs that are already stringified by the hook wrapper.

```cpp
HOOKFW_INVOKE(example, {{"stream", std::to_string(reinterpret_cast<uintptr_t>(stream))},
                        {"event", std::to_string(reinterpret_cast<uintptr_t>(event))}},
              stream,
              event);
```

Those names and values are emitted under the `Function-Args` section in the trace JSON.

## Adding a New Hook Target

To hook a new function, follow the same pattern used in `hook.cpp`.

### 1. Declare the function type

```cpp
using example_fn_t = int (*)(void * self, int value);
```

### 2. Register the target

```cpp
HOOKFW_DEFINE_TARGET(example,
                     example_fn_t,
                     "Example::run",
                     "_ZN7Example3runEi",
                     "/path/to/libexample.so")
```

### 3. Implement the wrapper

```cpp
int __attribute__((noinline, visibility("default")))
example_hook(void * self, int value) asm("_ZN7Example3runEi");

int example_hook(void * self, int value) {
    return HOOKFW_INVOKE(example, {{"self", std::to_string(reinterpret_cast<uintptr_t>(self))},
                                    {"value", std::to_string(value)}},
                         self,
                         value);
}
```

The `asm(...)` declaration must match the exported symbol exactly.

## PAPI / PMU Collection

PMU collection is optional and requires PAPI support at build time. At runtime, the recorder also checks the `HOOK_ENABLE_PAPI` environment variable, so PMU collection can be turned on or off without rebuilding.

If `HOOK_ENABLE_PAPI` is unset, the runtime default is enabled. Set it to a false-like value such as `0`, `false`, `off`, or `no` to disable PMU collection at runtime.

At runtime, the recorder reads events from `HOOK_PAPI_EVENTS` if it is set. Otherwise, it falls back to:

```text
perf::CYCLES,perf::INSTRUCTIONS,perf::CACHE-REFERENCES,perf::CACHE-MISSES
```

If PAPI is unavailable or the configured events cannot be initialized, the hook still works without PMU data.

## Notes

- Keep the mangled symbol names exact. A mismatch will prevent the hook from resolving the original function.
- If the compiler inlines the target function, the hook may never be hit.
- The current examples are specific to the paths in `hook.cpp`; they are not portable as-is.
- The trace output is JSON and can be consumed by trace viewers that understand Chrome trace event format.
