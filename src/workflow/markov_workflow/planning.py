"""Select an initial native observation per source domain and publish its concrete input."""

import json

from markov_adapters.sglang.configuration import ServingInput
from markov_collection.artifacts import write_json
from markov_workflow.adapters.sglang import SourceDomain
from markov_workflow.adapters.sglang_sources import RepresentativeSources


class SourcePlanning:
    def __init__(self, source):
        if source.keys() != {"framework", "model", "configurations", "workloads"}:
            raise ValueError("source planning needs framework, model, configurations and workloads")
        if source["framework"] != "sglang":
            raise ValueError("source planning currently requires the SGLang adapter")
        self.source = source
        self.configurations = self.named(source["configurations"], "configurations")
        self.workloads = self.named(source["workloads"], "workloads")
        self.workloads = {identity: ServingInput.workload_input(workload) for identity, workload in self.workloads.items()}
        self.representatives = RepresentativeSources(self.workloads)
        self.capture_inputs = {}
        for configuration in self.configurations.values():
            if configuration.keys() != {"id", "instances"}:
                raise ValueError("an exploration configuration needs id and complete serving instances")

    @staticmethod
    def named(values, kind):
        if not isinstance(values, list) or not values:
            raise ValueError(f"{kind} must be a nonempty array")
        result = {}
        for value in values:
            identity = value["id"]
            if not isinstance(identity, str) or not identity or identity in result:
                raise ValueError(f"{kind} IDs must be unique nonempty strings")
            result[identity] = value
        return result

    def document(self):
        domains = []
        candidates = []
        assignments = {}
        for identity, configuration in self.configurations.items():
            inputs = [(identity, name, ServingInput({
                "model": self.source["model"], "instances": configuration["instances"], "workload": workload,
            })) for name, workload in self.workloads.items()]
            serving = inputs[0][2]
            requested = SourceDomain.requested(serving)
            domain = next((item for item in domains if item["structure"] == requested), None)
            if domain is None:
                domain = {
                    "id": len(domains), "structure": requested, "configurations": [],
                    "native_workers": sum(instance.configuration.tp * instance.configuration.pp for instance in serving.instances),
                }
                domains.append(domain)
                candidates.append([])
            domain["configurations"].append(identity)
            candidates[domain["id"]].extend(inputs)
            assignments[identity] = domain["id"]

        for domain, inputs in zip(domains, candidates, strict=True):
            domain["source"], workload, groups = self.representatives.select(inputs)
            domain["source"]["input"] = f"sources/{domain['id']}.json"
            self.capture_inputs[domain["id"]] = {
                "model": self.source["model"],
                "instances": self.configurations[domain["source"]["configuration"]]["instances"],
                "workload": workload, "submission_groups": groups, "observe_all_workers": False,
            }

        # The Cartesian target set stays factored; no repeated request bodies or generated per-target task files.
        return {
            "framework": self.source["framework"], "domains": domains,
            "targets": {"configurations": assignments, "workloads": list(self.workloads),
                        "count": len(assignments) * len(self.workloads)},
            "qualification": "initial_representative_plan",
            "native_jobs": len(domains),
            "limits": ["input conditions are opportunities, not observed native branches",
                       "selected workloads run sequentially in one Engine; native cache state persists between groups",
                       "probe lengths preserve prefix, page/chunk remainders and route transitions; padding is unchanged",
                       "shared-prefix probes run consecutively after completion; actual cache hits still require native evidence",
                       "expert-route variation outside selected workloads is not covered",
                       "one initial source per domain; no automatic supplementation",
                       "shorter probes can change admission, cache and batching; lifecycle coverage still needs native evidence"],
        }


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
            f"每域选择一个初始来源，共 {plan['native_jobs']} 个原生任务；本命令只规划，不启动采集。",
            "选择互补 workload，构造较短请求，按需加入独立生命周期及顺序前缀复用；同一 Engine 逐组执行，实际命中和组批以原生记录为准。",
            "", "| 来源域 | 代表配置 / workload | 请求数 / 提交组 | prompt / decode token | 原选中 token | 原生 worker |", "| --- | --- | ---: | ---: | ---: | ---: |",
        ]
        for domain in plan["domains"]:
            selected = domain["source"]
            cost = selected["input_cost"]
            write_json(output / selected["input"], planner.capture_inputs[domain["id"]], indent=2, replace=False)
            lines.append(f"| {domain['id']} | {selected['configuration']} / {', '.join(selected['workloads'])} | {cost['requests']} / {selected['submission_group_count']} | "
                         f"{cost['prompt_tokens']} / {cost['decode_tokens']} | {selected['original_tokens']} | {domain['native_workers']} |")
        missing = [domain for domain in plan["domains"] if domain["source"]["unrepresented_conditions"]]
        if missing:
            lines.extend(["", "选定配置与兼容补齐条件下未包含的输入条件："])
            for domain in missing:
                lines.append(f"- 域 {domain['id']}：{', '.join(domain['source']['unrepresented_conditions'])}")
        lines.extend([
            "", "PD 两端作为一个完整来源域，不从不同原生任务拼接。worker 数是协议参与者数量，代表采集只减少观察和导出。",
            "", "sources 下的输入可直接交给 capture；请求/前缀 ID 编码为 [workload ID, 原 ID]，防止跨 workload 意外共享。",
            "输入条件只表示触发机会，例如共享前缀不保证实际缓存命中、不同输出长度不保证实际发生 subset。",
            "每组请求全部返回后，两端共同开始下一组；原生继续负责组批、清理和缓存，组间不重置缓存。",
            "保留组内到达、完整共享前缀、页/chunk 余数、输出长度分组与路由转折；只缩短可移除的重复区间，padding 不改写。",
            "独立请求探针使用已有路由和长度，不延长请求；它会增加短 workload 的采集量，表中列出实际输入成本。",
            "不会保证原组批、准入压力或缓存命中；EP 补齐条件不同的 workload 不合并，目标输入保持原样。",
            "acquire-sources 负责冻结镜像部署和累计额度；仍有未包含条件时拒绝启动，不在运行中追加采集。", "",
        ])
        (output / "plan.md").write_text("\n".join(lines))

        # Publish the index only after every referenced capture input exists.
        write_json(output / "plan.json", plan, indent=2, replace=False)
