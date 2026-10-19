"""A compact user report derived from the run ledger and published results."""

from __future__ import annotations

from collections import Counter
from pathlib import Path
from typing import Any

from markov_collection.artifacts.json_file import read_json


class RunSummary:
    def __init__(self, directory: Path, state: dict[str, Any]) -> None:
        self.directory = directory
        self.state = state

    def write(self) -> str:
        project = read_json(self.directory / "inputs/project.json")
        state = self.state
        lines = [
            "# Markov 运行结果",
            "",
            f"模型：`{project['model']['path']}`。",
            f"流程状态：`{state['status']}`；当前阶段：`{state['task_status']}`。",
            "",
            "预测计入统一流程内的 CPU 与设备工作；初始化、预热和模板外服务不计入。",
            "原生窗口与预测范围的差异见对照报告；流程完成和成本命中不代表精度合格。",
        ]
        if state.get("reason"):
            lines += ["", f"本次停止原因：`{state['reason']}`。"]

        result = state["prediction"]["result"]
        if result is not None:
            prediction = read_json(self.directory / result)
            comparison = read_json(self.directory / state["comparison"]) if state["comparison"] else None
            observed = (
                {(item["configuration_id"], item["request_workload_id"]): item for item in comparison["combinations"]}
                if comparison
                else {}
            )
            lines += [
                "",
                "| 配置 | 请求集合 | 预测状态 | 预测窗口 ms | 原生窗口 ms | 偏差 |",
                "| --- | --- | --- | ---: | ---: | ---: |",
            ]
            for item in prediction["combinations"]:
                native = observed.get((item["configuration_id"], item["request_workload_id"]))
                predicted = "—" if item["metrics"] is None else f"{item['metrics']['window_time_ns'] / 1e6:.3f}"
                actual, error = "—", "—"
                if native is not None and native["status"] == "compared":
                    actual = f"{native['window']['observed_ns'] / 1e6:.3f}"
                    value = native["window"]["relative_error_percent"]
                    error = f"{value:+.2f}%" if value is not None else "—"
                lines.append(
                    f"| {item['configuration_id']} | {item['request_workload_id']} | {item['status']} | "
                    f"{predicted} | {actual} | {error} |"
                )
            lines += ["", "“—”表示尚无可比较结果，原因见预测或对照报告；不表示零耗时。"]

        attempts = Counter(item["role"] for item in state["attempts"])
        lines += ["", "| 原生采集用途 | 次数 | 已用秒数 | 总预算秒数 |", "| --- | ---: | ---: | ---: |"]
        for role, permission in state["collection"].items():
            lines.append(
                f"| {role} | {attempts[role]} | {permission['consumed_wall_time_ns'] / 1e9:.3f} | "
                f"{permission['max_wall_time_sec']} |"
            )
        lines += ["", "预热和失败尝试计入原生采集用量；上述用量不包含离线处理时间。", "", "结果入口：", ""]
        if result is not None:
            lines.append("- [预测结果、范围和缺口](prediction.json)")
        if state["comparison"]:
            lines.append("- [独立端口对照](comparison.json)")
        if state["performance_plan"]:
            lines.append("- [未完成的性能采样需求](sampling-plan.json)")
        lines += ["- [运行状态与资产引用](../run.json)", "- [任务日志](../logs/)"]
        if state["task_status"] != "finished":
            lines += ["", f"修正停止原因后继续：`scripts/markov.sh resume --run {self.directory}`。"]
        lines += [
            "",
            "支持域及限制以本次预测报告为准；成本数据命中和流程完成均不表示精度合格。",
            "独立验证只用于对照，不进入模板或成本拟合。",
        ]
        relative = Path("results/summary.md")
        target = self.directory / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        temporary = target.with_suffix(".tmp")
        temporary.write_text("\n".join(lines) + "\n", encoding="utf-8")
        temporary.replace(target)
        return str(relative)
