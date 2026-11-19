"""Publish comparable modeled results by workload."""

from collections import defaultdict


class ExplorationReport:
    def __init__(self, targets, assets, usage, reuse):
        self.targets, self.assets, self.usage, self.reuse = targets, assets, usage, reuse

    def rankings(self):
        workloads = defaultdict(list)
        for row in self.targets:
            if row["status"] == "predicted":
                workloads[row["workload"]].append(row)

        rankings = {}
        for workload, rows in workloads.items():
            metrics = {}
            for metric, reverse in (("p95_ttft_ns", False), ("mean_tpot_ns", False), ("tokens_per_second", True)):
                groups = defaultdict(list)
                for row in rows:
                    if row[metric] is not None:
                        groups[row[metric]].append(row["configuration"])
                metrics[metric] = [{"value": value, "configurations": groups[value]} for value in sorted(groups, reverse=reverse)]
            rankings[workload] = metrics
        return rankings

    def document(self):
        return {
            "status": "complete" if all(row["status"] == "predicted" for row in self.targets) else "partial",
            "qualification": "modeled_predictions", "targets": self.targets, "rankings": self.rankings(),
            "assets": self.assets, "usage": self.usage, "reuse": self.reuse,
        }

    @staticmethod
    def cell(value):
        return str(value).replace("|", "\\|").replace("\n", " ").replace("\r", " ")

    def markdown(self):
        completed = sum(row["status"] == "predicted" for row in self.targets)
        lines = [
            "# 配置集性能探索", "",
            f"{completed}/{len(self.targets)} 项完成自主预测。排序按同一 workload 的预测值。",
            f"原生采集 {self.usage['native_attempts']} 次，用时 {self.usage['native_charged_ns'] / 1e9:.2f} 秒。"
            f"来源和成本后处理 {self.usage['postprocessing_ns'] / 1e9:.2f} 秒。", "",
            "| 配置 | Workload | 状态 | P95 TTFT / ms | 平均 TPOT / ms | token/s |", "| --- | --- | --- | ---: | ---: | ---: |",
        ]
        for row in self.targets:
            values = [row["configuration"], row["workload"], row["status"]]
            for metric, scale in (("p95_ttft_ns", 1e6), ("mean_tpot_ns", 1e6), ("tokens_per_second", 1)):
                value = row.get(metric)
                values.append("—" if value is None else f"{value / scale:.3f}")
            lines.append("| " + " | ".join(self.cell(value) for value in values) + " |")
        lines.extend(("", "逐目标证据、采集用量和各指标的并列排序见 result.json。", ""))
        return "\n".join(lines)
