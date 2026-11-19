"""Publish a finite source plan and its concrete capture inputs."""

import json

from markov_adapters.artifacts import write_json
from markov_workflow.adapters.sglang_planning import SourcePlanning


class PlanningProcess:
    def __init__(self, arguments, *, prewarm=False):
        self.arguments = arguments
        self.prewarm = prewarm

    def run(self):
        source = json.loads(self.arguments.input.read_text())
        planner = SourcePlanning(source)
        plan = planner.document()
        output = self.arguments.output
        output.mkdir(parents=True)
        write_json(output / "input.json", source, indent=2, replace=False)
        (output / "sources").mkdir()
        lines = [
            "# 有限来源任务规划", "",
            f"{plan['targets']['count']} 个目标组合预先规划 {len(plan['sources'])} 个来源任务。",
            f"包含预先规划的局部动作来源，共 {plan['native_jobs']} 个原生任务；本命令只规划，不启动采集。",
            "同一实现域可由互补配置或不兼容padding的有限任务供证；每次预测仍使用一份完整worker模板，不拼接未经证明的物理流。",
            "每任务组合互补workload、缩短请求并加入必要生命周期；一个Engine逐组执行，命中和组批以原生记录为准。",
            "", "| 来源任务 | 代表配置 / workload | 请求数 / 提交组 | prompt / decode token | 原选中 token | 原生 worker |", "| --- | --- | ---: | ---: | ---: | ---: |",
        ]
        for job in plan["sources"]:
            selected = job["source"]
            cost = selected["input_cost"]
            capture = planner.capture_inputs[job["id"]]
            if self.prewarm:
                capture = {**capture, "warmup": capture["workload"]}
            write_json(output / selected["input"], capture, indent=2, replace=False)
            lines.append(f"| {job['id']} | {selected['configuration']} / {', '.join(selected['workloads'])} | {cost['requests']} / {selected['submission_group_count']} | "
                         f"{cost['prompt_tokens']} / {cost['decode_tokens']} | {selected['original_tokens']} | {job['native_workers']} |")
        for job in plan["sources"]:
            required = job["required_local_coverage"]
            if not required:
                continue
            consumers = ["本域目标"] if job["configurations"] else []
            if "local_consumers" in job:
                consumers.extend(f"完整来源 {item['source']}/{item['mode']}" for item in job["local_consumers"])
            lines.append(f"\n来源 {job['id']} 为 {', '.join(consumers)} 提供局部操作来源；"
                         f"发布前必须实际观察到 {', '.join(required)}。")
        lines.extend([
            "", "这是采集前的有限计划；实际批次、命中、分支和局部覆盖以原生观察为准。",
            "原生继续负责组批和生命周期；代表观察减少profiling与导出，不减少协议参与卡数。",
            "sources 中的输入供采集使用，完整选择依据和目标范围保留在plan.json。",
            "采集发现覆盖不足时保留缺口，不在运行中补采或借目标补库。", "",
        ])
        (output / "plan.md").write_text("\n".join(lines))

        # Publish the index only after every referenced capture input exists.
        write_json(output / "plan.json", plan, indent=2, replace=False)
