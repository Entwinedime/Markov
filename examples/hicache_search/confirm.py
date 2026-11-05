"""Measure fixed recommendations serially and retain every observation in its capture owner."""

from __future__ import annotations

import argparse
import fcntl
from pathlib import Path

from markov.artifacts.request import PredictionRequest, read_json, resolve_path
from markov.artifacts.writing import write_json
from markov.workflow.capture import InferenceCapture
from markov.workflow.device_task import DeviceTask
from markov.workflow.evaluation import normal_statistics


class Confirmation:
    def __init__(self, path: Path, output: Path) -> None:
        self.plan = read_json(path)
        self.output = output.resolve()
        self.output.mkdir(parents=True, exist_ok=True)
        self.requests = [
            PredictionRequest.load(resolve_path(path.resolve().parent, case["request"])) for case in self.plan["cases"]
        ]
        self.selection_path = self.output / "additional_cases.json"

    def case_result(self, index: int) -> dict:
        case = self.plan["cases"][index]
        request = self.requests[index]
        task = DeviceTask(request.execution.capture, request.execution.output / "normal")
        attempts = task.state["attempts"]
        observations = [str(Path(row["output"]) / row["artifact"]) for row in attempts if row["status"] == "completed"]

        actual = normal_statistics(observations)
        value = actual["mean_sec"]
        cv = actual["stddev_sec"] / value if actual["stddev_sec"] is not None else None
        return {
            **case,
            "host_limit_gib": request.execution.capture.memory_limit_gib,
            "observations": observations,
            "capture_state": str(task.state_path),
            "failed_attempts": [row for row in attempts if row["status"] == "failed"],
            "normal": actual,
            "cv_ratio": cv,
            "ape_percent": 100 * abs(case["predicted_sec"] - value) / value if value is not None else None,
            "normal_meets_limits": [value <= limit for limit in case["applicable_limits_sec"]]
            if value is not None
            else None,
        }

    def collect(self, index: int, required: int) -> bool:
        request = self.requests[index]
        DeviceTask(request.execution.capture, request.execution.output / "normal").recover()
        while self.case_result(index)["normal"]["n"] < required:
            result = InferenceCapture(request, profiling=False).run(recapture=True)

            row = self.case_result(index)
            write_json(self.output / "progress.json", {"case": index, "required_repeats": required, "result": result})
            print(f"{row['scene']}/{row['target']}: {row['normal']['n']} observations, {result['status']}", flush=True)
            if result["status"] != "completed":
                return False
        return True

    def order(self, round_index: int) -> list[int]:
        scenes = list(dict.fromkeys(case["scene"] for case in self.plan["cases"]))
        result = []
        for scene in scenes if round_index % 2 == 0 else reversed(scenes):
            indices = [i for i, case in enumerate(self.plan["cases"]) if case["scene"] == scene]
            shift = round_index % len(indices)
            result.extend(indices[shift:] + indices[:shift])
        return result

    def measure(self) -> bool:
        initial = self.plan["initial_repetitions"]
        for round_index in range(initial):
            for index in self.order(round_index):
                if not self.collect(index, round_index + 1):
                    return False

        if not self.selection_path.exists():
            eligible = []
            for index in range(len(self.requests)):
                row = self.case_result(index)
                value = row["normal"]["mean_sec"]
                distance = min(abs(value / limit - 1) for limit in row["applicable_limits_sec"])
                variable = row["cv_ratio"] > self.plan["extra_cv_ratio"]
                crossed = any(
                    row["normal"]["minimum_sec"] <= limit <= row["normal"]["maximum_sec"]
                    for limit in row["applicable_limits_sec"]
                )
                if variable or crossed or distance <= self.plan["near_ceiling_ratio"]:
                    eligible.append((not variable, distance, index))
            eligible.sort()
            write_json(
                self.selection_path,
                {
                    "indices": [row[2] for row in eligible[: self.plan["maximum_extra_conditions"]]],
                    "eligible_priority": eligible,
                    "initial_observations": [self.case_result(i) for i in range(len(self.requests))],
                    "rule": self.plan["extra_rule"],
                },
            )
        selected = set(read_json(self.selection_path)["indices"])
        for round_index in range(initial, self.plan["maximum_repetitions"]):
            for index in self.order(round_index):
                if index in selected and not self.collect(index, round_index + 1):
                    return False
        return True

    def report(self) -> dict:
        rows = [self.case_result(i) for i in range(len(self.requests))]
        selected = set(read_json(self.selection_path)["indices"]) if self.selection_path.exists() else set()
        complete = self.selection_path.exists() and all(
            row["normal"]["n"]
            >= (self.plan["maximum_repetitions"] if i in selected else self.plan["initial_repetitions"])
            for i, row in enumerate(rows)
        )

        baselines = {row["scene"]: row for row in rows if "baseline" in row["roles"]}
        for row in rows:
            base = baselines[row["scene"]]
            measured = row["normal"]["mean_sec"]
            reference = base["normal"]["mean_sec"]
            row["actual_improvement_percent"] = (
                100 * (reference - measured) / reference if measured is not None and reference is not None else None
            )
            row["reservation_cost_change_percent"] = (
                100 * (row["hourly_cost_units"] - base["hourly_cost_units"]) / base["hourly_cost_units"]
            )
        result = {
            "status": "completed" if complete else "incomplete",
            "rows": rows,
            "normal_collection_complete": complete,
            "all_ape_under_ten_percent": all(row["ape_percent"] < 10 for row in rows) if complete else None,
            "qualification": "Fixed independent normal conditions, complete formal HTTP and all successful repeats. Failures remain in capture histories. Completion means the declared observations exist, not that prediction accuracy or every time ceiling passed. Reservation prices are normalized scenarios; no real global optimum or unique ranking claim.",
        }
        write_json(self.output / "results.json", result)

        def number(value: float | None) -> str:
            return f"{value:.3f}" if value is not None else "—"

        lines = [
            "# 固定条件的正常实测确认",
            "",
            "所有条件在读取正常结果前确定；三次初测后按预先规则追加。表中误差覆盖完整 HTTP，不扣 gap、未归属成本或计算。",
            "耗时门槛沿用预测时的同一秒数；完成采集不代表推荐达标。资源价格为声明情景。",
            "",
            "| 场景 | 条件 | 次数 | 预测 sec | 正常均值 sec | 误差 % | CV % | 耗时门槛 sec | 是否达标 | 相对基准改善 % |",
            "| --- | --- | ---: | ---: | ---: | ---: | ---: | --- | --- | ---: |",
        ]
        for row in rows:
            meets = (
                "／".join("是" if value else "否" for value in row["normal_meets_limits"])
                if row["normal_meets_limits"] is not None
                else "—"
            )
            lines.append(
                f"| {row['scene']} | {row['target']} | {row['normal']['n']} | {row['predicted_sec']:.3f} | "
                f"{number(row['normal']['mean_sec'])} | {number(row['ape_percent'])} | "
                f"{number(row['cv_ratio'] * 100 if row['cv_ratio'] is not None else None)} | "
                f"{'／'.join(number(limit) for limit in row['applicable_limits_sec'])} | {meets} | "
                f"{number(row['actual_improvement_percent'])} |"
            )
        lines += [
            "",
            "资源按实际执行的主机配额及声明报价报告；报价变化不代表实付账单变化。",
            "",
            "| 场景 | 条件 | 主机配额 GiB | 报价单位/小时 | 相对基准报价变化 % |",
            "| --- | --- | ---: | ---: | ---: |",
        ]
        for row in rows:
            lines.append(
                f"| {row['scene']} | {row['target']} | {row['host_limit_gib']} | "
                f"{row['hourly_cost_units']:.3f} | {row['reservation_cost_change_percent']:.3f} |"
            )
        (self.output / "results.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
        return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--plan", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--phase", choices=("measure", "report", "all"), default="all")
    args = parser.parse_args()
    confirmation = Confirmation(args.plan, args.output)
    with (confirmation.output / "confirmation.lock").open("a") as lease:
        fcntl.flock(lease, fcntl.LOCK_EX | fcntl.LOCK_NB)
        if args.phase in {"measure", "all"}:
            confirmation.measure()
        result = confirmation.report()
        print({"status": result["status"], "report": str(confirmation.output / "results.json")}, flush=True)
        if not result["normal_collection_complete"]:
            raise SystemExit(1)


if __name__ == "__main__":
    main()
