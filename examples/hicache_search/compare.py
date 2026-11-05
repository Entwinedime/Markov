"""Run the declared seeds with common prepared inputs; evaluate against the reference afterwards."""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import replace
import csv
import fcntl
import os
from pathlib import Path
import signal
from statistics import mean, median
import subprocess
import threading
import time

from markov.artifacts.request import read_json, resolve_path
from markov.artifacts.writing import write_json
from markov.workflow.search import ResourcePlan, SearchPolicy


EXAMPLE = Path(__file__).resolve().parent


class Comparison:
    def __init__(self, path: Path, output: Path, jobs: int) -> None:
        self.settings = read_json(path)
        self.directory = path.resolve().parent
        self.output = output.resolve()
        self.jobs = jobs
        self.active: dict[str, subprocess.Popen] = {}
        self.active_lock = threading.Lock()
        self.stopping = threading.Event()

    @staticmethod
    def stop(process: subprocess.Popen) -> None:
        if process.poll() is None:
            try:
                os.killpg(process.pid, signal.SIGINT)
                try:
                    process.wait(timeout=20)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
            except ProcessLookupError:
                process.wait()

    def command(self, scene: str, ratio: float, method: str, seed: int) -> tuple[str, list[str]]:
        name = f"{scene}/limit_{ratio:g}/{method}/{seed}"
        settings = read_json(resolve_path(self.directory, self.settings["space"]))
        settings["policy"].pop("maximum_time_sec", None)
        settings["policy"]["maximum_time_ratio"] = ratio
        path = self.output / "requests" / f"{scene}_{ratio:g}.json"
        if not path.exists():
            write_json(path, settings)
        return name, [
            str(EXAMPLE / "run.sh"),
            "--model",
            self.settings["model"],
            "--workload",
            str(resolve_path(self.directory, self.settings["workloads"][scene])),
            "--output",
            str(self.output / name),
            "--settings",
            str(path),
            "--base-manifest",
            str(resolve_path(self.directory, self.settings["sources"][scene])),
            "--database",
            str(resolve_path(self.directory, self.settings["database"])),
            "--phase",
            "predict",
            "--method",
            method,
            "--budgets",
            *[str(value) for value in self.settings["prediction_budgets"]],
            "--seed",
            str(seed),
        ]

    def execute(self, item: dict, deadline: float) -> dict:
        begin = time.monotonic()
        log = self.output / item["name"] / "command.log"
        log.parent.mkdir(parents=True, exist_ok=True)
        remaining = deadline - begin
        if remaining <= 0:
            return {**item, "status": "not_started", "reason": "CPU wall budget exhausted", "wall_sec": None}
        with log.open("a", encoding="utf-8") as stream:
            with self.active_lock:
                if self.stopping.is_set():
                    return {**item, "status": "not_started", "reason": "Study interrupted", "wall_sec": None}
                process = subprocess.Popen(
                    item["command"], stdout=stream, stderr=subprocess.STDOUT, start_new_session=True
                )
                self.active[item["name"]] = process
            try:
                try:
                    code = process.wait(timeout=remaining)
                    status = "completed" if code == 0 else "failed"
                except subprocess.TimeoutExpired:
                    self.stop(process)
                    code, status = process.returncode, "time_budget_exceeded"
            finally:
                self.stop(process)
                with self.active_lock:
                    self.active.pop(item["name"])
        return {**item, "status": status, "exit_code": code, "wall_sec": time.monotonic() - begin, "log": str(log)}

    def run(self) -> dict:
        begin = time.monotonic()
        (self.output / "requests").mkdir(parents=True, exist_ok=True)
        path = self.output / "runs.json"
        state = read_json(path) if path.exists() else {"runs": [], "host_wall_sec": 0.0}
        runs = state["runs"]
        known = {row["name"] for row in runs if row["status"] != "not_started"}
        pending = []
        for scene in self.settings["scenarios"]:
            for ratio in self.settings["baseline_relative_limits"]:
                for method in self.settings["methods"]:
                    for seed in self.settings["seeds"]:
                        name, command = self.command(scene, ratio, method, seed)
                        if name not in known:
                            pending.append(
                                {
                                    "name": name,
                                    "scene": scene,
                                    "ratio": ratio,
                                    "method": method,
                                    "seed": seed,
                                    "command": command,
                                }
                            )
        previous = state["host_wall_sec"]
        deadline = begin + max(0, self.settings["cpu_wall_limit_sec"] - previous)
        pool = ThreadPoolExecutor(max_workers=self.jobs)
        try:
            futures = [pool.submit(self.execute, row, deadline) for row in pending]
            for future in as_completed(futures):
                row = future.result()
                runs.append(row)
                write_json(
                    path, {"runs": runs, "jobs": self.jobs, "host_wall_sec": previous + time.monotonic() - begin}
                )
                print(f"{len(runs)} runs: {row['name']} {row['status']}", flush=True)
        finally:
            self.stopping.set()
            pool.shutdown(wait=False, cancel_futures=True)
            with self.active_lock:
                active = list(self.active.values())
            for process in active:
                self.stop(process)
            pool.shutdown(wait=True, cancel_futures=True)
            write_json(path, {"runs": runs, "jobs": self.jobs, "host_wall_sec": previous + time.monotonic() - begin})
        return {"runs": len(runs), "report": str(path)}

    def report(self) -> dict:
        # References enter this evaluator only; child search commands never receive them.
        references = {
            scene: read_json(resolve_path(self.directory, path))
            for scene, path in self.settings["reference_reports"].items()
        }
        history = read_json(self.output / "runs.json")["runs"]
        runs = list({row["name"]: row for row in history}.values())
        base_policy = SearchPolicy.parse(
            {"prediction": "unused", **read_json(resolve_path(self.directory, self.settings["space"]))["policy"]}
        )
        policies = {}
        reference_costs = {}
        for scene, reference in references.items():
            baseline = next(c["formal_http_sec"] for c in reference["cells"] if c["name"] == base_policy.baseline)
            for ratio in self.settings["baseline_relative_limits"]:
                for price, plans in self.settings["prices"].items():
                    key = scene, ratio, price
                    policies[key] = replace(
                        base_policy,
                        plans=tuple(ResourcePlan.parse(p) for p in plans),
                        maximum_time_ratio=None,
                        maximum_time_sec=baseline * ratio,
                    )
                    full = policies[key].rank(reference)
                    reference_costs[key] = next(
                        (r["hourly_cost_units"] for r in full["candidates"] if r["name"] == full["selected"]), None
                    )
        rows = []
        for run in runs:
            report = self.output / run["name"] / "prediction_run/search/report.json"
            state_path = report.parent / "trials.json"
            state = read_json(state_path) if state_path.exists() else None
            trial = read_json(report) if run["status"] == "completed" else state
            reference = references[run["scene"]]
            cells = []
            if state is not None:
                for entry in state["rounds"]:
                    if entry["status"] == "completed":
                        cells.extend(read_json(Path(entry["report"]))["cells"])
            for price in self.settings["prices"]:
                key = run["scene"], run["ratio"], price
                policy, full_cost = policies[key], reference_costs[key]
                for budget in self.settings["prediction_budgets"]:
                    checkpoint = (
                        next((r for r in trial["checkpoints"] if r["evaluations"] == budget), None) if trial else None
                    )
                    ranked = (
                        policy.rank({"deployment": reference["deployment"], "cells": cells[:budget]})
                        if checkpoint
                        else None
                    )
                    chosen = (
                        next((r for r in ranked["candidates"] if r["name"] == ranked["selected"]), None)
                        if ranked
                        else None
                    )
                    cost = chosen["hourly_cost_units"] if chosen else None
                    rows.append(
                        {
                            "scene": run["scene"],
                            "ratio": run["ratio"],
                            "method": run["method"],
                            "seed": run["seed"],
                            "price": price,
                            "budget": budget,
                            "status": "completed" if checkpoint else run["status"],
                            "run_status": run["status"],
                            "selected": chosen["name"] if chosen else None,
                            "cost": cost,
                            "reference_cost": full_cost,
                            "reference_feasible": full_cost is not None,
                            "regret_percent": 100 * (cost - full_cost) / full_cost
                            if cost is not None and full_cost is not None
                            else None,
                            "checkpoint_wall_sec": checkpoint["wall_sec"] if checkpoint else None,
                        }
                    )
        groups = []
        keys = sorted({(r["scene"], r["ratio"], r["method"], r["price"], r["budget"]) for r in rows})
        for scene, ratio, method, price, budget in keys:
            values = [
                r
                for r in rows
                if (r["scene"], r["ratio"], r["method"], r["price"], r["budget"])
                == (scene, ratio, method, price, budget)
            ]
            regret = [r["regret_percent"] for r in values if r["regret_percent"] is not None]
            times = [r["checkpoint_wall_sec"] for r in values if r["checkpoint_wall_sec"] is not None]
            groups.append(
                {
                    "scene": scene,
                    "ratio": ratio,
                    "method": method,
                    "price": price,
                    "budget": budget,
                    "seeds": len(values),
                    "completed_seeds": sum(r["status"] == "completed" for r in values),
                    "feasible_fraction": mean(r["selected"] is not None for r in values),
                    "reference_feasible": values[0]["reference_feasible"],
                    "mean_conditional_regret_percent": mean(regret) if regret else None,
                    "maximum_conditional_regret_percent": max(regret) if regret else None,
                    "median_checkpoint_wall_sec": median(times) if times else None,
                }
            )
        result = {
            "rows": rows,
            "groups": groups,
            "qualification": "Actual online calls; full tables read only by this evaluator. All seeds and failed runs retained. Cost regret is conditional on finding a feasible point; no finite-space feasible reference means regret is undefined. Pricing sensitivity reuses the same queried points; methods optimize the balanced scenario. No normal confirmation or real global optimum claim.",
        }
        write_json(self.output / "comparison.json", result)
        with (self.output / "comparison.csv").open("w", encoding="utf-8", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)
        lines = [
            "# 固定预算下的配置搜索",
            "",
            "每种方法使用同一来源、成本模型和预测预算。完整空间只供评价：这里的成本差距相对预测参照，不能称为真实最优差距。",
            "找到可行方案的比例包含全部种子；平均成本差距只统计找到方案的运行。无可行参照时成本差距没有定义。",
            "以下使用 balanced 归一化报价；另外两种报价及全部种子见 CSV。正常实测由独立确认报告评价。",
            "",
            "| 场景 | 耗时比例上限 | 方法 | 预测预算 | 完成种子 | 找到方案 | 条件平均成本差距 % | 中位耗时 sec |",
            "| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: |",
        ]
        for group in groups:
            if group["price"] != "balanced":
                continue
            regret = group["mean_conditional_regret_percent"]
            wall = group["median_checkpoint_wall_sec"]
            regret_text = f"{regret:.3f}" if regret is not None else "—"
            wall_text = f"{wall:.3f}" if wall is not None else "—"
            lines.append(
                f"| {group['scene']} | {group['ratio']:.0%} | {group['method']} | {group['budget']} | "
                f"{group['completed_seeds']}/{group['seeds']} | {group['feasible_fraction']:.0%} | "
                f"{regret_text} | {wall_text} |"
            )
        (self.output / "comparison.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
        return {"rows": len(rows), "groups": len(groups), "report": str(self.output / "comparison.json")}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--study", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--jobs", type=int, choices=(1, 2, 3), default=3)
    parser.add_argument("--phase", choices=("run", "report", "all"), default="all")
    args = parser.parse_args()
    comparison = Comparison(args.study, args.output, args.jobs)
    comparison.output.mkdir(parents=True, exist_ok=True)
    with (comparison.output / "comparison.lock").open("a") as lease:
        fcntl.flock(lease, fcntl.LOCK_EX | fcntl.LOCK_NB)
        if args.phase in {"run", "all"}:
            print(comparison.run())
        if args.phase in {"report", "all"}:
            print(comparison.report())


if __name__ == "__main__":
    main()
