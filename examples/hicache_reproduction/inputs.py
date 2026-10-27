"""Experiment recipes only; no prediction, fitting or target observations."""

from __future__ import annotations

from copy import deepcopy
from pathlib import Path
from typing import Any
import json


def require_repo_path(path: Path | str) -> Path:
    """Translate declared repo/container paths; CLI runs with repo as cwd."""

    value = Path(path)
    container_root = Path("/workspace/trace-sim")
    if value.is_relative_to(container_root):
        value = value.relative_to(container_root)

    return value.resolve()


def relative(path: Path | str) -> str:
    return str(require_repo_path(path).relative_to(Path.cwd()))


def load_json(path: Path | str) -> Any:
    with require_repo_path(path).open(encoding="utf-8") as stream:
        return json.load(stream)


def write_json(path: Path, document: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(document, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def replace_command_option(command: list[str], flag: str, value: str) -> None:
    # The checked-in launch recipe uses separate flag/value arguments.
    if flag in command:
        command[command.index(flag) + 1] = value
    else:
        command.extend((flag, value))


def normal_channels(item: dict[str, Any]) -> None:
    """Disable profiler instrumentation, retaining APIs used by the workload."""

    item["profiling"] = {"enabled": False, "channels": []}
    for key in list(item["env"]):
        if "TIMING" in key or key.startswith("HOOK_") or key == "SGLANG_STEP_SCHEDSTAT":
            del item["env"][key]


def write_profile_suite(path: Path, experiments: list[dict[str, Any]], **settings: Any) -> None:
    """The profiler owns one common channel contract per suite."""

    write_json(
        path,
        {
            "framework": "sglang",
            "profiling": experiments[0]["profiling"],
            **settings,
            "experiments": [{key: value for key, value in item.items() if key != "profiling"} for item in experiments],
        },
    )


def configuration_sets(recipe: dict[str, Any]) -> dict[str, list[dict[str, Any]]]:
    original = load_json(require_repo_path(recipe["original_configs"]))["configs"]
    expanded = load_json(require_repo_path(recipe["expanded_configs"]))["configs"]
    return {"original": original, "expanded": expanded, "all": original + expanded}


def server_config(recipe: dict[str, Any], spec: dict[str, Any]) -> dict[str, Any]:
    """Reuse the deployed launch recipe, replacing explicit experiment dimensions."""

    template = load_json(require_repo_path(recipe["profile_template"]))
    server = deepcopy(template["matrix"]["servers"][0]["server"])
    command = server["command"]
    policy, launcher = spec["policy"], spec["launcher"]
    timeout = policy.get("timeout", {})
    extra = {
        "prefetch_threshold": policy["prefetch_threshold"],
        "prefetch_timeout_base": 2.0 if timeout.get("base_sec") is None else timeout["base_sec"],
        "prefetch_timeout_per_ki_token": timeout.get("per_ki_token_sec") or 0.0,
        "prefetch_timeout_max": 2.0 if timeout.get("max_sec") is None else timeout["max_sec"],
        "hicache_storage_pass_prefix_keys": True,
    }
    changes = {
        "--model-path": recipe["model_path"],
        "--page-size": spec["resolved"]["page_size"],
        "--max-total-tokens": launcher["max_total_tokens"],
        "--hicache-ratio": launcher["hicache_ratio"],
        "--hicache-write-policy": policy["write_policy"],
        "--hicache-storage-prefetch-policy": policy["prefetch_stop_policy"],
        "--hicache-storage-backend-extra-config": json.dumps(extra, separators=(",", ":")),
    }
    for flag, value in changes.items():
        replace_command_option(command, flag, str(value))

    # NUMA takes a variable-length list. Do not let a scalar option helper
    # leave the previous recipe's second rank behind.
    begin = command.index("--numa-node")
    end = begin + 1
    while end < len(command) and not command[end].startswith("--"):
        end += 1
    command[begin + 1 : end] = [str(node) for node in recipe["numa_nodes"]]
    replace_command_option(command, "--base-gpu-id", str(recipe["devices"][0]))
    replace_command_option(command, "--gpu-id-step", "1")
    server.update(model_path=recipe["model_path"], startup_max_attempts=1)
    return server


def experiment(
    recipe: dict[str, Any], spec: dict[str, Any], workload: Path, specs_path: Path, *, normal: bool = False
) -> dict[str, Any]:
    template = load_json(require_repo_path(recipe["profile_template"]))
    result = {key: deepcopy(value) for key, value in template.items() if key not in {"matrix", "metadata", "name"}}
    workload_id = load_json(workload)["id"]
    result.update(
        name=f"{spec['id']}_{workload_id}",
        continue_on_error=False,
        server=server_config(recipe, spec),
        metadata={"config_id": spec["id"], "workload_id": workload_id, "resource_environment": recipe["resources"]},
        bench={
            "command": [
                "python3",
                "scripts/bench/hicache_template_workload.py",
                "--template",
                relative(workload),
                "--config-specs",
                relative(specs_path),
                "--config-id",
                spec["id"],
                "--tokenizer-path",
                recipe["model_path"],
                "--base-url",
                "http://127.0.0.1:30000",
                "--output-dir",
                "{bench_dir}/workload",
                "--checkpoint-mode",
                "observe",
                "--forced-token-mode",
                "replay",
                "--forced-token-plan",
                "{forced_token_plan}",
            ]
        },
    )
    env = result["env"]
    env["SGLANG_CPU_OMP_THREADS_BIND"] = "|".join(recipe["cpu_sets"])
    env["ASCEND_VISIBLE_DEVICES"] = ",".join(map(str, recipe["devices"]))
    if normal:
        normal_channels(result)
    else:
        result["profiling"]["torch"].update(npu_profiler_level=2, stop_timeout_sec=1800)
        env.update(
            SGLANG_NPU_PROFILER_LEVEL="2",
            SGLANG_NPU_PROFILER_AIC_METRICS="0",
            SGLANG_NPU_PROFILER_MAX_FILE_BYTES=str(32 * 1024**3),
            SGLANG_STEP_TIMING_DIR="{run_dir}/timing",
            SGLANG_SCHEDULER_TIMING="1",
            SGLANG_HICACHE_HOST_TIMING="1",
            SGLANG_LAYER_WAIT_TIMING="1",
            SGLANG_PREFETCH_TIMING="1",
            SGLANG_HICACHE_IO_TIMING="1",
            SGLANG_STEP_TIMING_EMISSION="1",
            SGLANG_STEP_SCHEDSTAT="1",
            HOOK_SYNC_THREAD_CPU="1",
            HOOK_MEMCPY2D_TIMING="1",
            HOOK_EMISSION_TIMING="1",
        )

    return result


def manual_workloads(recipe: dict[str, Any]) -> list[Path]:
    return [
        require_repo_path(item["bench"]["command"][item["bench"]["command"].index("--template") + 1])
        for item in load_json(require_repo_path(recipe["profile_template"]))["matrix"]["inputs"]
    ]


def panel_layout(recipe: dict[str, Any]) -> list[dict[str, Any]]:
    sets = configuration_sets(recipe)
    kinds = load_json(require_repo_path(recipe["sharegpt_policy"]))["output_tokens"]
    workload_ids = {
        "manual": [load_json(path)["id"] for path in manual_workloads(recipe)],
        "development": [f"sharegpt_{kind}" for kind in kinds],
        "confirmation": [f"confirmation_sharegpt_{kind}" for kind in kinds],
    }
    result = []
    for panel in recipe["panels"]:
        cells = [
            {
                "base": group["base"],
                "workload_set": group["workloads"],
                "workload_ids": workload_ids[group["workloads"]],
                "targets": [spec["id"] for spec in sets[group["targets"]] if spec["id"] != group["base"]],
            }
            for group in panel["groups"]
        ]
        count = sum(len(group["targets"]) * len(group["workload_ids"]) for group in cells)
        if count != panel["expected_cross"]:
            raise ValueError(
                f"panel {panel['name']} declares {panel['expected_cross']} cross cells, expands to {count}"
            )
        result.append({"name": panel["name"], "cross_count": count, "groups": cells})
    return result


def profile_budget(recipe: dict[str, Any], workloads: list[Path], *, starts_per_workload: int) -> dict[str, int]:
    # Explicit upper bounds, not measured usage; the product records consumption.
    starts = starts_per_workload * len(workloads) * recipe["capture_attempt_allowance"]
    return {
        "wall_seconds": starts * recipe["capture_seconds_per_start"],
        "server_starts": starts,
        "requests": starts * recipe["cpu_pair_budget_per_start"]["requests"],
        "tokens": starts * recipe["cpu_pair_budget_per_start"]["tokens"],
    }


def sampling_inputs(recipe: dict[str, Any], root: Path, prompt: list[int]) -> tuple[Path, list[Path]]:
    """Declare independent phase/cache/release work before looking at any target."""

    sampling = recipe["sampling"]
    symbols = sorted(set(prompt))
    if len(symbols) < 2:
        raise ValueError("independent sampling requires at least two distinct corpus tokens for separate cache paths")
    configs, workloads, experiments = [], [], []
    for page in sampling["page_token_sizes"]:
        for policy in sampling["policies"]:
            # Prefetch-check work has rank/participants, not page-size features.
            # A paired control sample is shared; payload services are measured
            # separately at every declared physical page width.
            if policy == "best_effort" and page not in sampling["best_effort_page_token_sizes"]:
                continue

            for release in (False, True) if sampling.get("include_release", True) else (False,):
                if release and policy != "wait_complete":
                    continue
                name = f"sampling_page{page}_{'release' if release else policy}"
                if not release and (write_policy := sampling.get("write_policy")):
                    name += f"_{write_policy}"

                timeout_sec = sampling["timeout_sec"]
                if policy == "timeout":
                    # Pairing needs equal work with/without profiler. A short
                    # wall-clock deadline can change the number of cache hits.
                    timeout_sec = sampling["paired_timeout_sec"]
                    name += f"_{timeout_sec:g}sec"
                if not release:
                    name += f"_output{sampling['output_tokens']}"

                device_pages = sampling["device_tokens"] // page
                host_pages = int(device_pages * sampling["host_ratio"]) + 1
                spec = {
                    "id": name,
                    "resolved": {"page_size": page, "device_pages": device_pages, "host_pages": host_pages},
                    "launcher": {
                        "max_total_tokens": sampling["device_tokens"],
                        "hicache_ratio": sampling["host_ratio"],
                    },
                    "policy": {
                        "write_policy": "write_through_selective"
                        if release
                        else sampling.get("write_policy", "write_back"),
                        "prefetch_threshold": page,
                        "prefetch_stop_policy": policy,
                        "prefetch_capacity_limit_tokens": int(0.8 * (host_pages - device_pages) * page),
                        "timeout": {
                            "base_sec": timeout_sec,
                            "per_ki_token_sec": 0.0,
                            "max_sec": timeout_sec,
                        },
                    },
                }
                configs.append(spec)
                definitions: dict[str, Any] = {}
                steps = []
                lengths = sampling["prompt_tokens"]
                lengths = [*lengths, *([max(lengths)] * sampling["pressure_requests"])]
                cold = sampling.get("cold_requests")
                cold_count = 0 if cold is None else cold["count"]
                prefix_width = 2
                while len(symbols) ** prefix_width < len(lengths) + cold_count:
                    prefix_width += 1

                def sample_tokens(length: int, index: int) -> list[int]:
                    tokens = (prompt * ((length + len(prompt) - 1) // len(prompt)))[:length]
                    # Valid corpus token IDs, distinct leading pages. No target
                    # prompt or output is used to create the independent work.
                    number = index
                    for offset in reversed(range(prefix_width)):
                        number, digit = divmod(number, len(symbols))
                        tokens[offset] = symbols[digit]

                    return tokens

                for index, length in enumerate(lengths):
                    tokens = sample_tokens(length, index)
                    key = f"r{index}"
                    anchor = min(page, length - 1)
                    definitions[key] = {
                        "prompt_token_ids": tokens,
                        "token_contract": {"anchor_tokens": anchor, "tail_tokens": length - anchor},
                    }
                    steps.extend(
                        [
                            {"id": key, "kind": "request", "request": key, "phase": "prepare", "measure": False},
                            {
                                "id": f"settle_{key}",
                                "kind": "barrier",
                                "scope": "hicache_idle",
                                "timeout_sec": 180,
                                "phase": "settle",
                                "measure": False,
                            },
                        ]
                    )
                    if not release and index == len(sampling["prompt_tokens"]) - 1:
                        parts = sampling.get("branch_parts", 2)
                        for part in range(1, parts):
                            key = "fork" if parts == 2 else f"fork_{part}"
                            fork = list(tokens)
                            split = length * part // (parts * page) * page
                            fork[split] = symbols[(symbols.index(fork[split]) + 1) % len(symbols)]
                            definitions[key] = {
                                "prompt_token_ids": fork,
                                "token_contract": {"anchor_tokens": split, "tail_tokens": length - split},
                            }
                            steps.extend(
                                [
                                    {
                                        "id": key,
                                        "kind": "request",
                                        "request": key,
                                        "phase": "prepare",
                                        "measure": False,
                                    },
                                    {
                                        "id": f"settle_{key}",
                                        "kind": "barrier",
                                        "scope": "hicache_idle",
                                        "timeout_sec": 180,
                                        "phase": "settle",
                                        "measure": False,
                                    },
                                ]
                            )
                # Cold formal requests create a measured eviction burst; the
                # prepared branched path stays backed in the larger host pool.
                if cold is not None:
                    length = cold["prompt_tokens"]
                    anchor = min(page, length - 1)
                    cold_definitions = {
                        f"cold{index}": {
                            "prompt_token_ids": sample_tokens(length, len(lengths) + index),
                            "token_contract": {"anchor_tokens": anchor, "tail_tokens": length - anchor},
                        }
                        for index in range(cold_count)
                    }
                    definitions = {**cold_definitions, **definitions}

                observed = f"r{len(lengths) - 1}" if release else f"r{len(sampling['prompt_tokens']) - 1}"
                steps.append(
                    {
                        "id": "prepared",
                        "kind": "checkpoint",
                        "phase": "boundary",
                        "measure": False,
                        "assertions": [
                            {
                                "id": "prepared_payload",
                                "request": observed,
                                "range": "anchor",
                                "expect": {
                                    "device": "all" if release else "none",
                                    "host": "ignore",
                                    "storage": "ignore" if release else "all",
                                },
                            }
                        ],
                    }
                )
                steps.extend(
                    {"id": f"sample_{key}", "kind": "request", "request": key, "phase": "formal", "measure": True}
                    for key in definitions
                )
                if not release:
                    for index in (0, len(lengths) - sampling["pressure_requests"] - 1):
                        steps.append(
                            {
                                "id": f"recover_{index}",
                                "kind": "request",
                                "request": f"r{index}",
                                "phase": "formal",
                                "measure": True,
                            }
                        )
                requests = [step for step in steps if step["kind"] == "request" and step["measure"]]
                workload = root / "sampling" / f"{name}.json"
                write_json(
                    workload,
                    {
                        "id": name,
                        "description": "Independent phase, cache pressure and recovery sampling; no evaluation requests.",
                        "defaults": {
                            "sampling": {
                                "max_new_tokens": 1 if release else sampling["output_tokens"],
                                "temperature": 0,
                                "top_p": 1.0,
                                "top_k": 1,
                                "ignore_eos": True,
                            }
                        },
                        "fragments": {},
                        "request_defs": definitions,
                        "steps": steps,
                        "formal_window": {"start_step": requests[0]["id"], "end_step": requests[-1]["id"]},
                    },
                )
                workloads.append(workload)

    specs_path = root / "sampling" / "configs.json"
    write_json(specs_path, {"configs": configs})
    for spec, workload in zip(configs, workloads):
        item = experiment(recipe, spec, workload, specs_path)
        item["profiling"]["torch"]["record_shapes"] = True
        if operations := sampling.get("cost_operations"):
            item["metadata"]["cost_operations"] = operations

        command = item["bench"]["command"]
        replace_command_option(command, "--forced-token-mode", "capture")
        offset = command.index("--forced-token-plan")
        del command[offset : offset + 2]
        experiments.append(item)
    suite = root / "sampling" / "suite.json"
    write_profile_suite(suite, experiments)
    return suite, workloads


def target_config(recipe: dict[str, Any], spec: dict[str, Any]) -> dict[str, Any]:
    """Translate the example's resolved policy into the public target input."""

    resolved, policy = spec["resolved"], spec["policy"]
    timeout = policy.get("timeout", {})
    return {
        "name": spec["id"],
        "hicache": {
            "enabled": True,
            "page_size": resolved["page_size"],
            "write_policy": policy["write_policy"],
            "prefetch_policy": policy["prefetch_stop_policy"],
            "prefetch_threshold_pages": (policy["prefetch_threshold"] + resolved["page_size"] - 1)
            // resolved["page_size"],
            "prefetch_timeout_base_sec": 2.0 if timeout.get("base_sec") is None else timeout["base_sec"],
            "prefetch_timeout_per_ki_token_sec": timeout.get("per_ki_token_sec") or 0.0,
            "prefetch_timeout_max_sec": 2.0 if timeout.get("max_sec") is None else timeout["max_sec"],
            "l1_capacity_pages": resolved["device_pages"],
            "l2_capacity_pages": resolved["host_pages"],
            "prefetch_capacity_limit_pages": policy["prefetch_capacity_limit_tokens"] // resolved["page_size"],
        },
    }


def source_jobs(suite: Path) -> list[dict[str, Any]]:
    document = load_json(suite)
    return [dict(item, profiling=deepcopy(document["profiling"])) for item in document["experiments"]]
