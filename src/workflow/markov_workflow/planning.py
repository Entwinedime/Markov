"""Publish a finite source plan and its concrete capture inputs."""

import json

from markov_adapters.artifacts import write_json
from markov_workflow.adapters.sglang_planning import SourcePlanning


class PlanningProcess:
    def __init__(self, arguments):
        self.arguments = arguments

    def run(self):
        source = json.loads(self.arguments.input.read_text())
        planner = SourcePlanning(source)
        plan = planner.document()
        output = self.arguments.output
        output.mkdir(parents=True)
        write_json(output / "input.json", source, indent=2, replace=False)
        (output / "sources").mkdir()
        lines = [
            "# 代表来源域规划", "",
            f"{plan['targets']['count']} 个目标组合分为 {len(plan['domains'])} 个来源域。",
            f"包含预先规划的局部动作来源，共 {plan['native_jobs']} 个原生任务；本命令只规划，不启动采集。",
            "选择互补 workload，构造较短请求，按需加入独立生命周期及顺序前缀复用；同一 Engine 逐组执行，实际命中和组批以原生记录为准。",
            "", "| 来源域 | 代表配置 / workload | 请求数 / 提交组 | prompt / decode token | 原选中 token | 原生 worker |", "| --- | --- | ---: | ---: | ---: | ---: |",
        ]
        for domain in plan["domains"]:
            selected = domain["source"]
            cost = selected["input_cost"]
            write_json(output / selected["input"], planner.capture_inputs[domain["id"]], indent=2, replace=False)
            lines.append(f"| {domain['id']} | {selected['configuration']} / {', '.join(selected['workloads'])} | {cost['requests']} / {selected['submission_group_count']} | "
                         f"{cost['prompt_tokens']} / {cost['decode_tokens']} | {selected['original_tokens']} | {domain['native_workers']} |")
        for domain in plan["domains"]:
            required = domain["required_local_coverage"]
            if not required:
                continue
            consumers = ["本域目标"] if domain["configurations"] else []
            if "local_consumers" in domain:
                consumers.extend(f"PD {item['domain']}/{item['mode']}" for item in domain["local_consumers"])
            lines.append(f"\n域 {domain['id']} 为 {', '.join(consumers)} 提供局部操作来源；"
                         f"发布前必须实际观察到 {', '.join(required)}。")
        missing = [domain for domain in plan["domains"] if domain["source"]["unrepresented_conditions"]]
        if missing:
            lines.extend(["", "选定配置与兼容补齐条件下未包含的输入条件："])
            for domain in missing:
                lines.append(f"- 域 {domain['id']}：{', '.join(domain['source']['unrepresented_conditions'])}")
        lines.extend([
            "", "这是采集前的有限计划；实际批次、命中、分支和局部覆盖以原生观察为准。",
            "原生继续负责组批和生命周期；代表观察减少profiling与导出，不减少协议参与卡数。",
            "sources 中的输入供采集使用，完整选择依据、目标映射和未覆盖条件保留在plan.json。",
            "采集发现覆盖不足时保留缺口，不在运行中补采或借目标补库。", "",
        ])
        (output / "plan.md").write_text("\n".join(lines))

        # Publish the index only after every referenced capture input exists.
        write_json(output / "plan.json", plan, indent=2, replace=False)
