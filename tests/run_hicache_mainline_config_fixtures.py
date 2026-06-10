#!/usr/bin/env python3
"""HiCache state 主线一配置约束 fixtures。"""

from __future__ import annotations

import json
import shlex
import importlib.util
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HICACHE_STATE_DIR = ROOT / "configs/experiments/hicache_state"
MAINLINE_CONFIG = HICACHE_STATE_DIR / "profiling_hicache_state_mainline_one_matrix.json"

# 这些签名来自 HCSV 已完成矩阵和主线一早期草案。它们必须保留在
# fixture 中，而不能只依赖 data/ 或当前仍存在的配置文件；否则清理旧
# 产物后，主线一候选配置可能无意复用已经跑过的联合配置。
KNOWN_PREVIOUS_SIGNATURES: dict[tuple[str, ...], str] = {
    ("128", "", "", "write_through", "timeout", "2.0", "10.0/0.0/10.0"): "C0 base / capacity-base / I2 base / I3 base",
    ("128", "56", "129", "write_back", "timeout", "2.0", "10.0/0.0/10.0"): "C1 write-back",
    ("128", "", "", "write_through_selective", "timeout", "2.0", "10.0/0.0/10.0"): "C2 write-through-selective",
    ("64", "128", "256", "write_through", "timeout", "2.0", "10.0/0.0/10.0"): "C3 page64 / I2 page64",
    ("128", "46", "96", "write_through", "timeout", "2.0", "10.0/0.0/10.0"): "C4 capacity",
    ("128", "", "", "write_through", "wait_complete", "2.0", "10.0/0.0/10.0"): "C5 prefetch-wait",
    ("128", "", "", "write_through", "best_effort", "2.0", "10.0/0.0/10.0"): "C6 best-effort",
    ("128", "", "", "write_through", "timeout", "2.0", "0.0/0.0/0.0"): "C7 aggressive timeout / I3 aggressive timeout",
    ("128", "46", "88", "write_back", "timeout", "2.0", "10.0/0.0/10.0"): "C8 write-back capacity",
    ("128", "64", "129", "write_through_selective", "wait_complete", "2.0", "10.0/0.0/10.0"): "earlier mainline-one S1A draft",
    ("64", "128", "256", "write_back", "best_effort", "2.0", "10.0/0.0/10.0"): "earlier mainline-one S1B draft config snapshot",
    ("64", "128", "257", "write_back", "best_effort", "2.0", "10.0/0.0/10.0"): "earlier mainline-one S1B draft observed effective capacity",
}


def main() -> int:
    run_mainline_one_signature_fixture()
    print("hicache mainline config fixtures passed")
    return 0


def run_mainline_one_signature_fixture() -> None:
    """验证主线一两个场景彼此不同，且不复用仓库内历史 HiCache state 配置。"""

    mainline_configs = [MAINLINE_CONFIG]
    assert MAINLINE_CONFIG.exists(), MAINLINE_CONFIG
    assert [path.name for path in mainline_configs] == ["profiling_hicache_state_mainline_one_matrix.json"], mainline_configs
    profile_runner = load_profile_runner()

    old_configs = sorted(
        path
        for path in HICACHE_STATE_DIR.rglob("*.json")
        if path != MAINLINE_CONFIG
    )
    old_signatures: dict[tuple[str, ...], list[Path]] = {}
    for path in old_configs:
        old_signatures.setdefault(config_signature(load_json(path)), []).append(path)

    scenario_signatures: dict[str, set[tuple[str, ...]]] = {}
    scenario_inputs: dict[str, set[str]] = {}
    expected_inputs = {"L1_manual_phased", "L2_bench_serving_large"}
    expanded_configs: list[tuple[Path, dict[str, object]]] = []
    for path in mainline_configs:
        for item in profile_runner.expand_suite(load_json(path)):
            expanded_configs.append((path, item))
    assert {str(cfg.get("id")) for _, cfg in expanded_configs} == {"s1a_manual", "s1a_bench", "s1b_manual", "s1b_bench"}

    for path, cfg in expanded_configs:
        metadata = cfg.get("metadata", {})
        scenario = metadata.get("mainline_one_scenario")
        input_id = metadata.get("mainline_one_input")
        assert scenario in {"S1A_baseline_large", "S1B_divergent_large"}, (path, scenario)
        assert input_id in expected_inputs, (path, input_id)

        signature = config_signature(cfg)
        assert float(signature[5]) > 1.0, (path, signature)
        assert metadata.get("mainline_one_config_signature") == signature_text(signature), (
            path,
            metadata.get("mainline_one_config_signature"),
            signature_text(signature),
        )
        assert metadata.get("mainline_one_config_novelty"), path
        assert metadata.get("mainline_one_config_comparison"), path
        assert old_signatures.get(signature, []) == [], (path, old_signatures.get(signature, []))
        assert signature not in KNOWN_PREVIOUS_SIGNATURES, (
            path,
            signature_text(signature),
            KNOWN_PREVIOUS_SIGNATURES.get(signature),
        )

        scenario_signatures.setdefault(str(scenario), set()).add(signature)
        scenario_inputs.setdefault(str(scenario), set()).add(str(input_id))

    assert set(scenario_signatures) == {"S1A_baseline_large", "S1B_divergent_large"}, scenario_signatures
    for scenario, signatures in scenario_signatures.items():
        assert len(signatures) == 1, (scenario, signatures)
        assert scenario_inputs[scenario] == expected_inputs, (scenario, scenario_inputs[scenario])

    unique_signatures = {next(iter(signatures)) for signatures in scenario_signatures.values()}
    assert len(unique_signatures) == len(scenario_signatures), scenario_signatures


def config_signature(cfg: dict[str, object]) -> tuple[str, ...]:
    args = command_args(cfg)
    hicache = cfg.get("modeling", {}).get("hicache", {})
    prefetch_extra = args.get("hicache-storage-backend-extra-config", "")
    if prefetch_extra:
        try:
            parsed = json.loads(prefetch_extra)
            prefetch_extra = "/".join(
                str(parsed.get(key, ""))
                for key in (
                    "prefetch_timeout_base",
                    "prefetch_timeout_per_ki_token",
                    "prefetch_timeout_max",
                )
            )
        except json.JSONDecodeError:
            pass

    return (
        args.get("page-size", "128"),
        str(hicache.get("l1_capacity_pages", "")),
        str(hicache.get("l2_capacity_pages", "")),
        args.get("hicache-write-policy", "write_through"),
        args.get("hicache-storage-prefetch-policy", "timeout"),
        args.get("hicache-ratio", ""),
        prefetch_extra,
    )


def signature_text(signature: tuple[str, ...]) -> str:
    keys = (
        "page_size",
        "l1_capacity_pages",
        "l2_capacity_pages",
        "write_policy",
        "prefetch_policy",
        "hicache_ratio",
        "prefetch_timeout_extra",
    )
    return ",".join(f"{key}={value}" for key, value in zip(keys, signature))


def command_args(cfg: dict[str, object]) -> dict[str, str]:
    command = cfg.get("server", {}).get("command", [])  # type: ignore[union-attr]
    if isinstance(command, str):
        command = shlex.split(command)

    result: dict[str, str] = {}
    index = 0
    while index < len(command):  # type: ignore[arg-type]
        item = str(command[index])  # type: ignore[index]
        if not item.startswith("--"):
            index += 1
            continue

        key = item[2:]
        next_index = index + 1
        if next_index < len(command) and not str(command[next_index]).startswith("--"):  # type: ignore[arg-type, index]
            result[key] = str(command[next_index])  # type: ignore[index]
            index += 2
        else:
            result[key] = "true"
            index += 1
    return result


def load_json(path: Path) -> dict[str, object]:
    return json.loads(path.read_text(encoding="utf-8"))


def load_profile_runner():
    spec = importlib.util.spec_from_file_location(
        "trace_sim_profile_runner_mainline_fixture",
        ROOT / "scripts/internal/profile_runner.py",
    )
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[str(spec.name)] = module
    spec.loader.exec_module(module)
    return module


if __name__ == "__main__":
    raise SystemExit(main())
