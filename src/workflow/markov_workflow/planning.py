"""Select an initial native observation per source domain and publish its concrete input."""

import json

from markov_adapters.sglang.configuration import ServingInput
from markov_collection.artifacts import write_json
from markov_workflow.adapters.sglang import SourceDomain
from markov_workflow.adapters.sglang_local_sources import LocalSources
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
                    "id": len(domains), "structure": requested, "configurations": [], "required_local_coverage": [],
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

        LocalSources(self.source["model"], self.workloads, self.configurations).extend(domains, self.capture_inputs)

        # The Cartesian target set stays factored; auxiliary sources do not become target configurations.
        return {
            "framework": self.source["framework"], "domains": domains,
            "targets": {"configurations": assignments, "workloads": list(self.workloads),
                        "count": len(assignments) * len(self.workloads)},
            "qualification": "initial_representative_plan",
            "native_jobs": len(domains),
            "limits": ["input conditions are opportunities, not observed native branches",
                       "selected workloads run sequentially after all-worker native idle; cache persists between groups",
                       "probe lengths preserve prefix, page/chunk remainders and route transitions; padding is unchanged",
                       "fixed arrival gaps keep original lengths; other shortening must preserve declared input conditions",
                       "allocator path opportunities use a cold-input page bound, not predicted batches or branch coverage",
                       "cached KV pressure and PD page reuse retain role-local demand; sharing/admission still decide eviction or sorting",
                       "shared-prefix probes run consecutively after completion; actual cache hits still require native evidence",
                       "expert-route variation outside selected workloads is not covered",
                       "ordinary and PD local coverage is checked on actual observations; missing batch/model coverage stops acquisition without supplementation",
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
            "", "PD 两端作为一个完整来源域，不从不同原生任务拼接；闭合的 merge/subset 可从兼容普通来源导入。worker 数是协议参与者数量，代表采集只减少观察和导出。",
            "按PD各参与者的局部需求合并：存在后续Decode时，两端均声明merge/subset需求，同一批次数组实施域由一个普通来源提供，不按TP/PP/EP度重复采集；优先复用已需采集的模型几何来源。实际执行模型且允许多请求批次的多TP参与者另需各自并行域的单/多成员模型调用。全部请求仅输出首token时不要求Decode模型来源，Prefill需求独立保留。已有普通来源须已声明覆盖所需动作，否则初次规划增加有限辅助来源。",
            "新辅助来源每个PP槽位两对请求，批次周转时输出1/2 token，只有模型几何需求时均输出1 token；模型几何来源先执行一个独立单请求组。Prefill预算依据分页准入，EP只截取已有路由，KV容量不扩大。",
            "单/多成员检查只证明模型调用覆盖，不证明所有局部guard完整；局部模板必须仍满足实施域、接口、别名和依赖合同才能复用。",
            "已有普通来源可能仍未触发所需动作；实际覆盖不足时停止采集任务并明确缺口，不补采、不启动缺库预测。该检查不代替核心对闭合叶动作和实际实施域的导入检查。",
            "", "sources 下的输入可直接交给 capture；请求/前缀 ID 编码为 [workload ID, 原 ID]，防止跨 workload 意外共享。",
            "输入条件只表示触发机会，例如共享前缀不保证实际缓存命中、不同输出长度不保证实际发生 subset。",
            "每组请求全部返回且所有 worker 到达原生空闲后，两端共同开始下一组；原生继续负责组批、清理和缓存，组间不重置缓存。",
            "多到达时刻的 workload 保留原长度；其他 workload 仅在输入条件保持时缩短，并保留共享前缀、页/chunk 余数、输出分组与路由转折，padding 不改写。",
            "同时到达的多 Decode、不同 Decode 长度及累计页需求分别参与选择。排队由原生控制推进，不以请求总数超过槽位数要求独立模板；具体组批、分配和资源动作仍需来源覆盖。Prefill只计prompt，其他角色还计Decode增长；单请求可容纳而累计需求超池时，保留缓存压力或PD延迟回收页再使用的机会，后者不要求启用缓存。未扣前缀复用、不推演准入，不等于实际淘汰或排序。",
            "分页 Prefill 根据同到达组、槽位、页容量及 prefill/chunk 预算计算冷输入页数上界；同一调用可能容纳第二成员时另保留重复分配机会，不能用跨组的多请求替代。上界不证明实际成批、缓存状态或分支覆盖。",
            "独立请求探针使用已有路由和长度，不延长请求；它会增加短 workload 的采集量，表中列出实际输入成本。",
            "不会保证原组批、准入压力或缓存命中；EP 补齐条件不同的 workload 不合并，目标输入保持原样。",
            "acquire-sources 负责冻结镜像部署和累计额度；仍有未包含条件时拒绝启动，不在运行中追加采集。", "",
        ])
        (output / "plan.md").write_text("\n".join(lines))

        # Publish the index only after every referenced capture input exists.
        write_json(output / "plan.json", plan, indent=2, replace=False)
