# S11-B 完整独立验证报告

日期：2026-11-01。调查已结束，**S11 未通过，包含未覆盖项**。本报告保留同一冻结基线的成功、局部一致、差异、失败及停止，不以完整登记代替完整验证。性能层不在本轮范围。

## 1. 结论

已确认模板能在多个独立来源域内随请求、成员和容量展开；在完成比较的70项后端案例、178份rank报告中，1492个已配对模型主体的当前检查全部通过。它们是“后端×rank×配对批次”的计数，不是1492个唯一模板或独立采集。

整体serving资格尚未成立：普通结果动作顺序、无模型协调工作、局部参数作用域及部分来源覆盖存在明确问题，PP×PD Decode拒绝尚未闭合，GLM和page1正式链路未完成。完整跨rank因果、部分PD输入条件及未知物理事件仍有证据边界。处理额度停止的案例继续算未覆盖。

本轮没有修改冻结业务、适配、评价或镜像，没有按目标差异补模板。下一步返回修复和维护阶段；修复后须重新冻结并重新执行完整独立矩阵，当前留出转为开发/回归材料。

## 2. 冻结依据与独立性

| 对象 | 本轮实际依据 |
| --- | --- |
| 核心、适配、评价和构建 | `f2bbd0ea9ee7d5fdb06ce792a6f262526ac7de35`；适配提交相同，期间无适配例外 |
| SGLang上游工作树 | `4e155ecc31af486c2878af4943a41c8378f69525`；未为模型/并行组合修改上游执行实现 |
| 推理镜像 | `markov-next-sglang:ubuntu22.04`，`c471b8a40ad9f8f5` |
| 原生控制镜像 | `markov-next-control:ubuntu22.04`，`b0df9e667190c9f0` |
| C++建模/评价镜像 | `markov-next-modeling:ubuntu24.04`，`e066a5196c3b34b2` |
| 目标主种子 | 311047；预定备用311063、311087未启用 |
| 输入及完整image ID | [冻结矩阵](../../../data/20261031T061440.793094Z_s11-independent-validation/inputs/matrix.json) |

模型保留官方完整层数/专家结构，使用原生dummy权重和可复现请求内容；MoE按给定逻辑路由及padding条件注入原生执行。没有真实权重文本质量、自然路由或KV数值等价结论。七个模型此前均参与过开发或兼容性探索，不能称为七个全新模型；独立性来自冻结后的目标运行及未用于修正规则的留出。

来源按固定代表配方建库；独立目标的原生记录不进入模板库。双后端共用一次真实目标观察，各自生成预测及报告。逐rank比较还使用共同的来源解释工具，原生事实独立不代表转换工具不存在共同盲点。

## 3. 清单、执行顺序与最终状态

先运行44个来源工作流，再执行51个真实目标的92项后端检查，随后270项条件和9项生命周期；最后按预定资源安排顺延硬件受阻项。共415个唯一案例、425条含恢复历史的记录，与清单逐项一致，无缺失或额外ID。全部任务已无pending预留。

| 类别 | 唯一案例数 | 最终结果 |
| --- | ---: | --- |
| workflow | 44 | 36 complete；6 partial；1 error；1 budget_exhausted |
| native | 92 | 23 partial；39 differences；8 matched；8 error；7 budget_exhausted；7 blocked |
| conditional | 270 | 203 complete；16 rejected；18 error；20 blocked；13 budget_exhausted |
| lifecycle | 9 | 9 complete |

`complete`表示该类流程完成，`matched`仅表示声明检查范围对应；`partial`表示有未知或覆盖不足，`differences`表示已有检查发现差异。`error`须按责任归因，不能一概算核心错误；`blocked`和`budget_exhausted`不算覆盖完成。

原完整驱动退出2，原因是仍有可恢复/未完成项。第一轮顺延于UTC 2026-11-01 10:30:57开始、13:58:43结束，脚本退出0仅表示完成这一轮调用。最终无`resource_blocked`，没有符合条件的第二轮硬件顺延；没有重启整套矩阵、扩额度或借用适配储备。六卡任务使用已登记的同尺寸物理重排，不缩小逻辑拓扑。[原始逐例结果与恢复历史](../../../data/20261031T061440.793094Z_s11-independent-validation/results/outcomes.json)、[完整415项机器摘要](../../../data/20261031T061440.793094Z_s11-independent-validation/results/summary.md)、[顺延日志](../../../data/20261031T061440.793094Z_s11-independent-validation/logs/deferred-hardware-1.log)、[设备重排](../../../data/20261031T061440.793094Z_s11-independent-validation/results/device-reassignment.json)。

### 3.1 来源工作流与模板库

七模型为Qwen3-8B、Mistral-7B-v0.3、Qwen1.5-MoE-A2.7B、Qwen3-30B-A3B、Meta Llama-3.1-8B-Instruct、OLMoE-1B-7B-0924和GLM-4-9B-Chat。GLM两个工作流在输入规范化处失败，本轮没有取得其正式来源/目标资格。Llama替代用户放弃的Phi-3；OLMoE保留EP1，原生不支持的多卡/PP组合由其他合格MoE承担机制覆盖，历史失败没有抹除。

252个配置×workload正式组合产生211份结果、32项预测/输入错误；另9项未发布为page1六项及PD小池域三项。下表错误数来自来源工作流记录，不混入后续负例或验证调用新增的任务失败。

| 来源任务 | workflow状态 | 发布库 | 结果/组合 | 错误 |
| --- | --- | ---: | ---: | ---: |
| qwen3-tp1 | complete | 1 | 30/30 | 0 |
| qwen3-tp2 | complete | 1 | 12/12 | 0 |
| qwen3-tp4 | complete | 1 | 4/4 | 0 |
| qwen3-tp8 | complete | 1 | 4/4 | 0 |
| qwen3-tp1-pp3 | complete | 1 | 6/6 | 0 |
| qwen3-tp1-pp4 | complete | 1 | 6/6 | 0 |
| qwen3-tp2-pp3 | partial | 1 | 0/6 | 6 |
| qwen3-tp2-pp4 | partial | 1 | 0/6 | 6 |
| mistral-tp1-pp1 | complete | 1 | 4/4 | 0 |
| mistral-tp4-pp1 | complete | 1 | 4/4 | 0 |
| mistral-tp1-pp3 | complete | 1 | 6/6 | 0 |
| mistral-tp1-pp4 | complete | 1 | 6/6 | 0 |
| qwen15-moe-ep1 | complete | 1 | 4/4 | 0 |
| qwen15-moe-ep2 | complete | 1 | 4/4 | 0 |
| qwen15-moe-ep4 | complete | 1 | 4/4 | 0 |
| qwen3-moe-ep2 | complete | 1 | 4/4 | 0 |
| qwen3-moe-ep4 | complete | 1 | 4/4 | 0 |
| qwen3-moe-ep8 | complete | 1 | 4/4 | 0 |
| qwen3-moe-ep2-pp3 | complete | 1 | 6/6 | 0 |
| qwen3-moe-pd2-2 | partial | 1 | 0/4 | 4 |
| qwen3-pd4-2 | complete | 1 | 4/4 | 0 |
| qwen3-pd2-4 | complete | 1 | 4/4 | 0 |
| qwen3-moe-pd-pp3 | complete | 1 | 4/4 | 0 |
| llama31-tp1 | complete | 1 | 6/6 | 0 |
| llama31-tp2 | complete | 1 | 6/6 | 0 |
| glm4-tp1 | partial | 0 | 0/6 | 6 |
| glm4-tp2 | partial | 0 | 0/6 | 6 |
| olmoe-ep1 | complete | 1 | 4/4 | 0 |
| qwen3-fp16 | complete | 1 | 4/4 | 0 |
| qwen3-page1 | error | 0 | 0/6 | 0 |
| qwen3-page16 | complete | 1 | 6/6 | 0 |
| qwen3-page64 | complete | 1 | 6/6 | 0 |
| qwen3-page256 | complete | 1 | 6/6 | 0 |
| qwen3-page64-pp3 | complete | 1 | 6/6 | 0 |
| qwen3-moe-page64-ep2 | complete | 1 | 4/4 | 0 |
| qwen3-page64-pd1-1 | complete | 1 | 4/4 | 0 |
| scheduling-single | complete | 1 | 4/4 | 0 |
| scheduling-tp2 | complete | 1 | 9/9 | 0 |
| scheduling-ep2 | complete | 1 | 4/4 | 0 |
| scheduling-pp3 | complete | 1 | 4/4 | 0 |
| scheduling-pd | budget_exhausted | 1 | 6/9 | 0 |
| scheduling-pd-tp | complete | 1 | 4/4 | 0 |
| scheduling-pd-ep | complete | 1 | 4/4 | 0 |
| scheduling-pd-pp | partial | 1 | 0/4 | 4 |

41份库绑定140个逻辑角色：110个观察、30个派生。库关联47份来源会话记录，共130次worker参与、110次角色观察；20次参与未作观察，节省的是相应观察/导出，不能推为20次完整Engine启动或等比例运行时间节省。这些计数仅含已发布库，不含失败/未发布来源及独立目标，也不等于本轮93次原生尝试。

库文件合计306302181字节；定义6937、布局13629、执行模式2876均为逐库求和，**不是全项目唯一模板种类**。模型、TP/PP/EP、dtype、backend、页布局及机制域分别建来源；没有证明同库任意跨这些维度变换。[逐库定义、布局、角色及会话依据](../../../data/20261031T061440.793094Z_s11-independent-validation/results/template-inventory.json)。

### 3.2 独立原生目标与两后端

44/51份目标取得完整观察与原生输入检查。92项后端中70项完成结构比较，结果为8 matched、23 partial、39 differences；其余8 error、7 budget_exhausted、7 blocked。

| 目标：任务/配置/workload | 原生输入观察 | 轻量控制 | 原生控制 |
| --- | --- | --- | --- |
| [qwen3-tp1/small/boundaries](../../../data/20261031T061440.793094Z_s11-independent-validation/runs/qwen3-tp1/results/validation/small/boundaries/lightweight/validation-small-boundaries.json) | 已核对 | partial | differences |
| [qwen3-tp1/r32/members-32](../../../data/20261031T061440.793094Z_s11-independent-validation/runs/qwen3-tp1/results/validation/r32/members-32/lightweight/validation-r32-members-32.json) | 已核对 | partial | matched |
| [qwen3-tp1/r33/members-33](../../../data/20261031T061440.793094Z_s11-independent-validation/runs/qwen3-tp1/results/validation/r33/members-33/lightweight/validation-r33-members-33.json) | 已核对 | partial | partial |
| [qwen3-tp1/r128/members-128](../../../data/20261031T061440.793094Z_s11-independent-validation/runs/qwen3-tp1/results/validation/r128/members-128/lightweight/validation-r128-members-128.json) | 已核对 | partial | partial |
| [qwen3-tp1/wide/long-outputs](../../../data/20261031T061440.793094Z_s11-independent-validation/runs/qwen3-tp1/results/validation/wide/long-outputs/lightweight/validation-wide-long-outputs.json) | 已核对 | partial | differences |
| [qwen3-tp2/wide/boundaries](../../../data/20261031T061440.793094Z_s11-independent-validation/runs/qwen3-tp2/results/validation/wide/boundaries/lightweight/validation-wide-boundaries.json) | 已核对 | partial | differences |
| [qwen3-tp2/kv262144/context-boundaries](../../../data/20261031T061440.793094Z_s11-independent-validation/runs/qwen3-tp2/results/validation/kv262144/context-boundaries/lightweight/validation-kv262144-context-boundaries.json) | 已核对 | partial | matched |
| [qwen3-tp4/wide/boundaries](../../../data/20261031T061440.793094Z_s11-independent-validation/runs/qwen3-tp4/results/validation/wide/boundaries/lightweight/validation-wide-boundaries.json) | 已核对 | partial | differences |
| [qwen3-tp8/wide/boundaries](../../../data/20261031T061440.793094Z_s11-independent-validation/runs/qwen3-tp8/results/validation/wide/boundaries/lightweight/validation-wide-boundaries.json) | 已核对 | partial | differences |
| [qwen3-tp1-pp3/wide/boundaries](../../../data/20261031T061440.793094Z_s11-independent-validation/runs/qwen3-tp1-pp3/results/validation/wide/boundaries/lightweight/validation-wide-boundaries.json) | 已核对 | differences | differences |
| [qwen3-tp1-pp4/wide/boundaries](../../../data/20261031T061440.793094Z_s11-independent-validation/runs/qwen3-tp1-pp4/results/validation/wide/boundaries/lightweight/validation-wide-boundaries.json) | 已核对 | differences | differences |
| [qwen3-tp2-pp3/wide/boundaries](../../../data/20261031T061440.793094Z_s11-independent-validation/runs/qwen3-tp2-pp3/results/validation/wide/boundaries/lightweight/validation-wide-boundaries.json) | 已核对 | error | error |
| [qwen3-tp2-pp4/full-micro/boundaries](../../../data/20261031T061440.793094Z_s11-independent-validation/runs/qwen3-tp2-pp4/results/validation/full-micro/boundaries/lightweight/validation-full-micro-boundaries.json) | 已核对 | error | error |
| [mistral-tp1-pp1/wide/boundaries](../../../data/20261031T061440.793094Z_s11-independent-validation/runs/mistral-tp1-pp1/results/validation/wide/boundaries/lightweight/validation-wide-boundaries.json) | 已核对 | partial | differences |
| [mistral-tp4-pp1/wide/boundaries](../../../data/20261031T061440.793094Z_s11-independent-validation/runs/mistral-tp4-pp1/results/validation/wide/boundaries/lightweight/validation-wide-boundaries.json) | 已核对 | partial | differences |
| [mistral-tp1-pp3/small/boundaries](../../../data/20261031T061440.793094Z_s11-independent-validation/runs/mistral-tp1-pp3/results/validation/small/boundaries/lightweight/validation-small-boundaries.json) | 已核对 | differences | matched |
| [mistral-tp1-pp4/wide/boundaries](../../../data/20261031T061440.793094Z_s11-independent-validation/runs/mistral-tp1-pp4/results/validation/wide/boundaries/lightweight/validation-wide-boundaries.json) | 已核对 | differences | differences |
| [qwen15-moe-ep1/wide/varying](../../../data/20261031T061440.793094Z_s11-independent-validation/runs/qwen15-moe-ep1/results/validation/wide/varying/lightweight/validation-wide-varying.json) | 已核对 | partial | differences |
| [qwen15-moe-ep2/wide/varying](../../../data/20261031T061440.793094Z_s11-independent-validation/runs/qwen15-moe-ep2/results/validation/wide/varying/lightweight/validation-wide-varying.json) | 已核对 | differences | differences |
| [qwen15-moe-ep4/wide/varying](../../../data/20261031T061440.793094Z_s11-independent-validation/runs/qwen15-moe-ep4/results/validation/wide/varying/lightweight/validation-wide-varying.json) | 已核对 | differences | differences |
| [qwen3-moe-ep2/wide/varying](../../../data/20261031T061440.793094Z_s11-independent-validation/runs/qwen3-moe-ep2/results/validation/wide/varying/lightweight/validation-wide-varying.json) | 已核对 | differences | differences |
| [qwen3-moe-ep4/wide/varying](../../../data/20261031T061440.793094Z_s11-independent-validation/runs/qwen3-moe-ep4/results/validation/wide/varying/lightweight/validation-wide-varying.json) | 已核对 | differences | budget_exhausted |
| [qwen3-moe-ep8/wide/varying](../../../data/20261031T061440.793094Z_s11-independent-validation/runs/qwen3-moe-ep8/results/validation/wide/varying/lightweight/validation-wide-varying.json) | 已核对 | budget_exhausted | budget_exhausted |
| [qwen3-moe-ep2-pp3/wide/varying](../../../data/20261031T061440.793094Z_s11-independent-validation/runs/qwen3-moe-ep2-pp3/results/validation/wide/varying/lightweight/validation-wide-varying.json) | 已核对 | differences | budget_exhausted |
| [qwen3-moe-pd2-2/wide/varying](../../../data/20261031T061440.793094Z_s11-independent-validation/runs/qwen3-moe-pd2-2/results/validation/wide/varying/lightweight/validation-wide-varying.json) | 已核对 | error | error |
| [qwen3-pd4-2/wide/boundaries](../../../data/20261031T061440.793094Z_s11-independent-validation/runs/qwen3-pd4-2/results/validation/wide/boundaries/lightweight/validation-wide-boundaries.json) | 已核对 | differences | differences |
| [qwen3-pd2-4/wide/boundaries](../../../data/20261031T061440.793094Z_s11-independent-validation/runs/qwen3-pd2-4/results/validation/wide/boundaries/lightweight/validation-wide-boundaries.json) | 已核对 | differences | differences |
| [qwen3-moe-pd-pp3/wide/varying](../../../data/20261031T061440.793094Z_s11-independent-validation/runs/qwen3-moe-pd-pp3/results/validation/wide/varying/lightweight/validation-wide-varying.json) | 未完整 | budget_exhausted | blocked |
| [llama31-tp1/wide/boundaries](../../../data/20261031T061440.793094Z_s11-independent-validation/runs/llama31-tp1/results/validation/wide/boundaries/lightweight/validation-wide-boundaries.json) | 已核对 | partial | differences |
| [llama31-tp2/wide/boundaries](../../../data/20261031T061440.793094Z_s11-independent-validation/runs/llama31-tp2/results/validation/wide/boundaries/lightweight/validation-wide-boundaries.json) | 已核对 | partial | differences |
| `glm4-tp1/wide/boundaries` | 未完整 | blocked | blocked |
| `glm4-tp2/wide/boundaries` | 未完整 | blocked | blocked |
| [olmoe-ep1/wide/varying](../../../data/20261031T061440.793094Z_s11-independent-validation/runs/olmoe-ep1/results/validation/wide/varying/lightweight/validation-wide-varying.json) | 已核对 | partial | differences |
| [qwen3-fp16/wide/boundaries](../../../data/20261031T061440.793094Z_s11-independent-validation/runs/qwen3-fp16/results/validation/wide/boundaries/lightweight/validation-wide-boundaries.json) | 已核对 | partial | differences |
| `qwen3-page1/small/unit-capacity` | 未完整 | blocked | blocked |
| [qwen3-page16/small/unit-capacity](../../../data/20261031T061440.793094Z_s11-independent-validation/runs/qwen3-page16/results/validation/small/unit-capacity/lightweight/validation-small-unit-capacity.json) | 已核对 | partial | matched |
| [qwen3-page64/small/unit-capacity](../../../data/20261031T061440.793094Z_s11-independent-validation/runs/qwen3-page64/results/validation/small/unit-capacity/lightweight/validation-small-unit-capacity.json) | 已核对 | partial | matched |
| [qwen3-page256/small/unit-capacity](../../../data/20261031T061440.793094Z_s11-independent-validation/runs/qwen3-page256/results/validation/small/unit-capacity/lightweight/validation-small-unit-capacity.json) | 已核对 | partial | matched |
| [qwen3-page64-pp3/wide/boundaries](../../../data/20261031T061440.793094Z_s11-independent-validation/runs/qwen3-page64-pp3/results/validation/wide/boundaries/lightweight/validation-wide-boundaries.json) | 已核对 | differences | differences |
| [qwen3-moe-page64-ep2/wide/varying](../../../data/20261031T061440.793094Z_s11-independent-validation/runs/qwen3-moe-page64-ep2/results/validation/wide/varying/lightweight/validation-wide-varying.json) | 已核对 | differences | differences |
| [qwen3-page64-pd1-1/wide/boundaries](../../../data/20261031T061440.793094Z_s11-independent-validation/runs/qwen3-page64-pd1-1/results/validation/wide/boundaries/lightweight/validation-wide-boundaries.json) | 已核对 | differences | differences |
| [scheduling-single/chunk-256/shared](../../../data/20261031T061440.793094Z_s11-independent-validation/runs/scheduling-single/results/validation/chunk-256/shared/native/validation-chunk-256-shared.json) | 已核对 | — | partial |
| [scheduling-tp2/chunk-256/shared](../../../data/20261031T061440.793094Z_s11-independent-validation/runs/scheduling-tp2/results/validation/chunk-256/shared/native/validation-chunk-256-shared.json) | 已核对 | — | matched |
| [scheduling-tp2/cache-pressure/cache-pressure](../../../data/20261031T061440.793094Z_s11-independent-validation/runs/scheduling-tp2/results/validation/cache-pressure/cache-pressure/native/validation-cache-pressure-cache-pressure.json) | 已核对 | — | partial |
| [scheduling-ep2/chunk-256/shared](../../../data/20261031T061440.793094Z_s11-independent-validation/runs/scheduling-ep2/results/validation/chunk-256/shared/native/validation-chunk-256-shared.json) | 已核对 | — | differences |
| [scheduling-pp3/chunk-256/shared](../../../data/20261031T061440.793094Z_s11-independent-validation/runs/scheduling-pp3/results/validation/chunk-256/shared/native/validation-chunk-256-shared.json) | 已核对 | — | matched |
| [scheduling-pd/chunk-256/shared](../../../data/20261031T061440.793094Z_s11-independent-validation/runs/scheduling-pd/results/validation/chunk-256/shared/native/validation-chunk-256-shared.json) | 未完整 | — | budget_exhausted |
| [scheduling-pd/cache-pressure/cache-pressure](../../../data/20261031T061440.793094Z_s11-independent-validation/runs/scheduling-pd/results/validation/cache-pressure/cache-pressure/native/validation-cache-pressure-cache-pressure.json) | 未完整 | — | error |
| [scheduling-pd-tp/chunk-256/shared](../../../data/20261031T061440.793094Z_s11-independent-validation/runs/scheduling-pd-tp/results/validation/chunk-256/shared/native/validation-chunk-256-shared.json) | 已核对 | — | differences |
| [scheduling-pd-ep/chunk-256/shared](../../../data/20261031T061440.793094Z_s11-independent-validation/runs/scheduling-pd-ep/results/validation/chunk-256/shared/native/validation-chunk-256-shared.json) | 未完整 | — | budget_exhausted |
| [scheduling-pd-pp/chunk-256/shared](../../../data/20261031T061440.793094Z_s11-independent-validation/runs/scheduling-pd-pp/results/validation/chunk-256/shared/native/validation-chunk-256-shared.json) | 已核对 | — | error |

“—”表示该后端不在该例清单；“原生输入观察已核对”不表示预测或整窗口通过。r33/r128的原生图报告本身matched，但Prefill实际仅32成员，矩阵按门槛降为partial；表中保留最终矩阵判定。

完成比较的178份rank报告、1492个配对模型主体全部通过当前主体检查。额度停止的EP4、EP8和EP×PP原生/轻量案例另保留5份完整rank报告、40个模型主体，它们也通过主体检查，但对应后端仍未完整比较；全体保留183份rank报告、1532个模型主体对照，不能据此升级预算停止状态。

六项完成比较但存在未配对批次的后端均来自Dense PD，包括两种非对称TP及page64 P1/D1。其余64项完成比较没有未配对批次，也不自动关闭外围操作或完成关系差异。

七份缺少完整观察的目标分别为：GLM TP1/TP2及page1因来源未发布受阻；MoE PP×PD及普通PD chunk/shared因processing停止；普通PD cache-pressure缺少正式目标输入；MoE PD chunk/shared因native额度停止。它们没有被移出清单。

### 3.3 条件及生命周期

270项条件为203 complete、16 rejected、18 error、20 blocked、13 budget_exhausted。前60项定向条件为40 complete、16 rejected、2 error、2 blocked；后210项另一后端的配置组合为163 complete、16 error、18 blocked、13 budget_exhausted。它们运行正式预测并做内部检查，**没有逐例再采原生目标**，也不是同一原生调用输入下的局部模板重放。

13项条件额度停止来自EP4四项、EP×PP六项和MoE PP×PD三项。18项error包括TP2×PP十二项、MoE PD四项及错误页资产负例两项，不能算18个新根因。GLM十二项、page1八项依赖受阻；包含等页数与错资产条件时按各自清单计数。

九项生命周期全部complete：非法入口后help、已完成恢复、额度停止、缺输入、两后端输入覆盖保护、两后端中断恢复。中断前费用继续累计，恢复后worker释放且pending清空；这些结果不关闭模型结构或分布式因果问题。

## 4. 实际取得的范围与限制

| 机制 | 本轮证据 | 保留边界 |
| --- | --- | --- |
| 请求/成员/KV容量 | 同域50项Dense TP条件通过内部检查；覆盖1/2/31/32/33/64/101/128并发设置、32/64步输出、最长2305-token上下文、最高262144-token池等具名点 | 并发上限不等于实际组批；33/128成员Prefill仅32，不能授予大Prefill资格 |
| 页布局及dtype | page16/64/256各自来源的small/unit-capacity原生后端窗口matched；FP16独立建库；另有page64×PP/EP/PD与等32页条件 | page1未建库；固定4096token的原生例与固定32页的条件分开；没有同库跨页/dtype或Decode跨页扩容独立证明 |
| 同等角色复用 | Dense TP8来源观察0/1、派生2–7，真实八rank两后端的96个模型主体对照通过；EP4派生2/3未增加差异类别；Mistral PP4中间stage及PD派生D角色有具名局部证据 | TP8整窗口仍partial/differences；EP8仅观察代表rank0完成结构比较；不能从元数据角色一致推定未比较角色正确 |
| 不均分PP | Mistral PP3实际10/11/11层，PP4每stage8层；20项条件通过；PP3小池原生窗口matched | 轻量控制同例仍有资源条件/窗口顺序差异；其他PP尾stage的跨批次存储顺序未闭合 |
| MoE路由 | 多模型EP1/2/4/8具名观察；EP8实际2304次专家调用、77568行路由含1488行padding核对一致 | 给定路由的条件结构；没有自然路由/真实权重资格；多例空协调工作缺失，整窗口不通过 |
| chunk/mixed/Decode预算/缓存 | TP2 chunk-256/shared两个rank各17621操作matched；PP3对应三stage整窗口matched并实际触发chunk、mixed及缓存命中；其他具名内部状态检查保留 | 大多数优化组合窗口仍partial/differences；cache-pressure的victim身份/LRU与实际调用输入未全部独立闭合 |
| 给定耗时反馈 | TP4与MoE PP×PD的16项内部案例改变host/device/communication/transfer耗时，仍合法完成；后者批次组成发生变化 | 全是合成耗时，不是实测成本或性能精度，也不证明所有争用/同步策略 |
| 资源与交接 | 页/槽位唯一归属、资源终态、已知设备消费者到释放以及各已登记PD生产完成端口有内部检查 | 已知消费者完整性、opaque host数组、实际KV载荷和完整跨rank端点/进展未获得独立证明 |

最后三项顺延事实需单独理解：

- **TP2×PP4：** 真实八worker、输入检查matched，实际Prefill8/Decode6；预测因私有参数缺失，零rank图比较。原生能跑与预测能展开是两件独立事实。
- **EP8：** 原生八rank输入及路由检查通过；轻量rank0六主体通过，但窗口28项原生操作未配对、缺6项设备工作。两后端均因原processing上限停止，其余结构范围未完成。
- **Dense PP3×PD：** 四请求、两池各4096token，Prefill分块256并启用缓存；原生六rank输入matched，实际Prefill/Decode各2成员、两批chunk、512token前缀复用。预测仍拒绝Decode入口，零rank图比较；P/D事实位于各自子目录，不能用单池顶层glob判断丢失。

## 5. 已确认问题及修复建议

以下按跨模块责任归因；同根因影响的多个案例不重复计作多个缺陷。取证记录保留完整日志及声明边界，修复不追溯修改本轮判定。

### F1 普通结果处理中的retain/release顺序与原生不符

- **级别/确认程度：** Must Fix；已确认。
- **责任与位置：** 原生控制桥接与核心结果动作连接。[src/control/markov_control/adapters/frameworks/sglang/resources.py](../../../src/control/markov_control/adapters/frameworks/sglang/resources.py)、[src/markov/adapters/frameworks/sglang/rank_structure.cpp](../../../src/markov/adapters/frameworks/sglang/rank_structure.cpp)、[src/markov/controller/native_group.cpp](../../../src/markov/controller/native_group.cpp)。
- **触发与影响：** 未启用前缀缓存，多个请求在同一批次完成结果处理。
- **证据：** Qwen1.5 EP2/4及Qwen3 EP2的已取证案例中，原生逐请求交错retain和release；桥接立即展开retain，却将普通release延迟到整批result，出现缺少caller完成关系和多出的stream关系。 [取证记录](../../../data/20261031T061440.793094Z_s11-independent-validation/results/result-order-scope-evidence.json)。
- **建议：** 在共同动作/完成接口保留实际调用顺序和唯一资源所有者，一次修复共享桥接；不要按模型或case补边。
- **边界：** 新TP8及其他类似差异尚未逐项映射，不能全部归于此因。

### F2 无模型批次的协调工作未进入目标窗口

- **级别/确认程度：** Must Fix；已确认。
- **责任与位置：** 框架观察、控制反馈与模板展开交接。[src/collection/markov_collection/adapters/frameworks/sglang/parallel.py](../../../src/collection/markov_collection/adapters/frameworks/sglang/parallel.py)、[src/control/markov_control/adapters/frameworks/sglang/scheduler.py](../../../src/control/markov_control/adapters/frameworks/sglang/scheduler.py)、[src/markov/adapters/frameworks/sglang/rank_structure.cpp](../../../src/markov/adapters/frameworks/sglang/rank_structure.cpp)、[src/markov/controller/native.cpp](../../../src/markov/controller/native.cpp)。
- **触发与影响：** 原生协调函数执行设备工作后返回None，包括末尾清理和非空批次之间的空轮次。
- **证据：** EP2/4及EP×PP已保存原生区域和操作归属，缺少的比较/非零查找/归约工作与未登记的空协调对应；EP8轻量rank0另有28项原生操作及6项设备工作缺口，种类数量相同但尚未逐项归因。 [取证记录](../../../data/20261031T061440.793094Z_s11-independent-validation/results/empty-coordination-evidence.json)。
- **建议：** 将有工作但无模型批次的原生动作按统一工作/完成合同交接，保留发生时机及参与组；不固定追加一次尾图，也不通过评价排除这些工作。
- **边界：** 空轮次不全部发生在尾部，完整跨rank完成责任仍需独立证据。

### F3 TP2×PP等待入口依赖未公开的局部参数

- **级别/确认程度：** Must Fix；已确认。
- **责任与位置：** 来源表达式作用域与公开输入交接。[src/collection/markov_collection/adapters/frameworks/sglang/pipeline.py](../../../src/collection/markov_collection/adapters/frameworks/sglang/pipeline.py)、[src/markov/adapters/frameworks/sglang/rank_structure.cpp](../../../src/markov/adapters/frameworks/sglang/rank_structure.cpp)、[src/markov/production/template_library.cpp](../../../src/markov/production/template_library.cpp)、[src/markov/templates/instance.cpp](../../../src/markov/templates/instance.cpp)。
- **触发与影响：** Qwen3 TP2×PP3/4目标实例化pipeline_wait_output_next_token_ids。
- **证据：** 目标缺proxy_residual_tokens；来源等待入口声明的是payload_0_axis_0，八个等待入口保存了私有参数引用。两拓扑均已取得真实原生多worker观察，但两后端没有生成目标图；十二项条件预测同样失败。 [取证记录](../../../data/20261031T061440.793094Z_s11-independent-validation/results/pipeline-wait-scope-evidence.json)。
- **建议：** 沿参数生产者、作用域和公共端口统一转换有来源依据的表达式，修复后联查所有消费者；不补零或按变量名增加专用默认值。
- **边界：** 另一个PP×PD Decode拒绝同时包含端口与条件问题，未证明属于同一根因。

### F4 page1来源的局部allocated_pages符号逃逸

- **级别/确认程度：** Must Fix；已确认。
- **责任与位置：** 框架/运行时来源几何交接。[src/collection/markov_collection/adapters/frameworks/sglang/state.py](../../../src/collection/markov_collection/adapters/frameworks/sglang/state.py)、[src/collection/markov_collection/adapters/runtimes/pytorch/indexing.py](../../../src/collection/markov_collection/adapters/runtimes/pytorch/indexing.py)、[src/collection/markov_collection/adapters/runtimes/ascend/layouts.py](../../../src/collection/markov_collection/adapters/runtimes/ascend/layouts.py)、[src/markov/graph/source_graph.cpp](../../../src/markov/graph/source_graph.cpp)。
- **触发与影响：** page_size=1来源生产，批次准备引用分配子调用中的局部表达式。
- **证据：** allocated_pages离开所属子作用域后被六处批次准备操作引用，正式来源验证拒绝，模板库未发布。不是原生page1无法运行的证明。 [取证记录](../../../data/20261031T061440.793094Z_s11-independent-validation/results/page-one-scope-evidence.json)。
- **建议：** 使用分配调用的公开结果几何及真实所属作用域交接；与其他符号逃逸按同一合同检查，不能为page1设置例外成功路径。
- **边界：** 页大小需要自身来源域，本轮没有证明同库跨page_size变换。

### F5 MoE PD来源仅覆盖空筛选，目标需要非空分支

- **级别/确认程度：** Must Fix；已确认覆盖缺口；附带内存风险未确认。
- **责任与位置：** 框架来源配方与批次筛选模板。[src/collection/markov_collection/adapters/frameworks/sglang/batches.py](../../../src/collection/markov_collection/adapters/frameworks/sglang/batches.py)、[src/collection/markov_collection/adapters/frameworks/sglang/preparation.py](../../../src/collection/markov_collection/adapters/frameworks/sglang/preparation.py)、[src/markov/adapters/frameworks/sglang/batches.cpp](../../../src/markov/adapters/frameworks/sglang/batches.cpp)。
- **触发与影响：** Qwen3 MoE P/D分离中的Prefill筛选after_members非零。
- **证据：** 来源P侧只有空筛选布局，目标非空时被eq(0,after_members)条件拒绝；两后端与四项条件均有相同拒绝。 [取证记录](../../../data/20261031T061440.793094Z_s11-independent-validation/results/pd-filter-coverage-evidence.json)。
- **建议：** 先核对统一筛选入口、局部原型和来源完整性，再由原生机制提供必要有限分支；不从目标图补模板，也不逐长度补采。
- **边界：** Release中保留行视图的生命周期风险尚未确认，不能把它写成这次失败原因。

### F6 Dense PP×PD的Decode入口无法选出布局

- **级别/确认程度：** Must Fix；来源工作流拒绝已确认；责任未闭合。
- **责任与位置：** 待判定：来源端口、归一化、目标绑定或控制条件。[src/markov/production/template_library.cpp](../../../src/markov/production/template_library.cpp)、[src/markov/templates/instance.cpp](../../../src/markov/templates/instance.cpp)、[src/markov/adapters/frameworks/sglang/rank_structure.cpp](../../../src/markov/adapters/frameworks/sglang/rank_structure.cpp)。
- **触发与影响：** PP3×PD来源发布后，chunk128/256与shared/arrivals四个正式目标展开。
- **证据：** 四例都拒绝Decode/decode，详细日志同时报告输入端口签名不同及layout438的or(eq(0,B),eq(1,B))条件不满足。 [取证记录](../../../data/20261031T061440.793094Z_s11-independent-validation/results/pipeline-wait-scope-evidence.json)。
- **建议：** 按实际来源调用与目标公开输入追溯两个拒绝条件，确定责任再作共享修复；不得添加B<=1支持限制、硬编码扩大B条件或归为框架不支持。
- **边界：** 独立目标六rank已取得规范观察，native_evidence为matched，实际Prefill/Decode均2成员、两批chunk及512token前缀复用；预测仍拒绝Decode入口，零rank图比较，不是原生框架不支持的证明。

### F7 GLM的词表元数据未规范化交接

- **级别/确认程度：** Must Fix；已确认。
- **责任与位置：** 模型配置与正式目标输入适配。[src/collection/markov_collection/adapters/frameworks/sglang/acquisition.py:31](../../../src/collection/markov_collection/adapters/frameworks/sglang/acquisition.py#L31)、[src/collection/markov_collection/adapters/frameworks/sglang/session.py:126](../../../src/collection/markov_collection/adapters/frameworks/sglang/session.py#L126)、[src/control/markov_control/adapters/frameworks/sglang/scheduler.py:123](../../../src/control/markov_control/adapters/frameworks/sglang/scheduler.py#L123)。
- **触发与影响：** GLM-4-9B-Chat TP1/TP2正式工作流生成目标输入。
- **证据：** 原始配置提供padded_vocab_size，当前路径读取vocab_size失败；十二个正式组合报错、两个来源库均未发布，当前两任务没有启动Engine。 [取证记录](../../../data/20261031T061440.793094Z_s11-independent-validation/results/outcomes.json)。
- **建议：** 查明现有规范模型元数据的所有者，由适配边界一次提供统一词表含义及来源，消除raw配置字段泄漏；不增加按GLM名称判断的模型分支。
- **边界：** 此前短请求原生兼容性记录不能代替当前矩阵的正式工作流或目标结构证明。

### F8 错误页资产负例依赖表达式打印顺序

- **级别/确认程度：** Must Fix；已确认。
- **责任与位置：** example评价工具。[example/automatic_graph/matrix.py:386](../../../example/automatic_graph/matrix.py#L386)、[example/automatic_graph/investigation.py:356](../../../example/automatic_graph/investigation.py#L356)。
- **触发与影响：** page64目标使用page128来源库，两种控制后端均执行负例。
- **证据：** 核心均返回码1并拒绝发布结果；实际条件是eq(128,page_size)，清单却要求eq(page_size,128)，原生异常摘要还只保留第一行，正式结果因此仍为error。 [取证记录](../../../data/20261031T061440.793094Z_s11-independent-validation/results/static-review.json)。
- **建议：** 按实际失败边界、退出状态、未发布结果及已有core.log判定预期拒绝；不只交换字符串，也不新增通用错误解析或兼容框架。
- **边界：** 保留本轮两个error，修复不能追溯把冻结结果改为通过。

### F9 评价重复运行、解析、扫描与依赖查询造成较大开销

- **级别/确认程度：** Should Fix；下一轮完整验证前必须收口；冗余已确认，耗时归因未测量。
- **责任与位置：** example评价与证据所有权。[example/automatic_graph/validation.py](../../../example/automatic_graph/validation.py)、[example/automatic_graph/structure.py](../../../example/automatic_graph/structure.py)、[example/automatic_graph/operations.py](../../../example/automatic_graph/operations.py)、[example/automatic_graph/execution.py](../../../example/automatic_graph/execution.py)、[example/automatic_graph/investigation.py](../../../example/automatic_graph/investigation.py)。
- **触发与影响：** 多rank大图、长请求、MoE/PP/PD及跨后端检查。
- **证据：** 已审阅的共同链路反复生产观察图、逐rank解析完整预测、逐批扫描和路径查询；30项重复内部检查与此前同输入报告深度一致。部分正式比较因原processing上限停止。 [取证记录](../../../data/20261031T061440.793094Z_s11-independent-validation/results/static-review.json)。
- **建议：** 用本轮保留的大图经正式入口作有界函数级测量；同一案例交接已有事实与检查结果，共享必要图索引，删除重复职责链，并在语义不变下比较结果、耗时与峰值后重新估算矩阵额度。
- **边界：** DependencyPaths已对到达的目标提前停止；不能把它描述为无条件遍历全图，也没有测出某函数占比或承诺加速倍数。

### F10 PD小池准入及大Prefill机制覆盖不足

- **级别/确认程度：** Must Fix验证覆盖；配方/交接事实已确认。
- **责任与位置：** 来源规划与example验证编排。[example/automatic_graph/matrix.py](../../../example/automatic_graph/matrix.py)、[example/automatic_graph/investigation.py](../../../example/automatic_graph/investigation.py)、[src/application/markov_application/exploration.py](../../../src/application/markov_application/exploration.py)。
- **触发与影响：** 1024-token Decode锚点接收527/545-token请求并要求512-token原生预留；大成员Prefill仍使用4096-token、128-token页预算。
- **证据：** PD所需1039/1057 token超过1024，来源额度停止后下游缺目标输入；大成员案例Decode实际达到33/128，Prefill只达到32，不能按配置上限宣称覆盖。 [取证记录](../../../data/20261031T061440.793094Z_s11-independent-validation/results/outcomes.json)。
- **建议：** 依据真实源/目标准入条件组织合法锚点及负例预期，不擅自扩大用户目标容量；修正下一轮机制配方和依赖交接，使必须验证的大Prefill实际触发，未发布输入不得继续伪装成原生运行错误。
- **边界：** 这些事实不证明模板在相应容量或人数上逻辑不可泛化，不能加业务黑名单回避。

## 6. 仍需验证的风险和证据缺口

| 项目 | 当前事实及所缺证明 | 后续处理 |
| --- | --- | --- |
| PP跨批次依赖 | 六卡尾stage的20对顺序要求为14对请求映射读/后写、6对输出缓冲赋值；报告同时检查逻辑与CPU返回路径，不同调度历史下是否必须保留相同存储顺序尚未证明 | 先对应调用输入和实际值版本，区分必要依赖与过强存储约束；[依据](../../../data/20261031T061440.793094Z_s11-independent-validation/results/pipeline-window-order-evidence.json) |
| PD筛选输入 | 原生/预测after_members及回收队列状态不同，部分批次未对应；270项条件不是相同原生调用条件重放 | 在同一公开调用条件下检查局部变换，控制历史与完整窗口另查；[依据](../../../data/20261031T061440.793094Z_s11-independent-validation/results/pd-batch-scope-evidence.json) |
| 跨rank通信 | 核心按flow、组、端点、通道/字段连接；现有报告主要逐rank，当前原生接口未另保留全部低层src/dst/group实参 | 依据真实端点/组事实独立核对连接及完成责任，不能用内部自洽替代；未确认核心配对错误；[依据](../../../data/20261031T061440.793094Z_s11-independent-validation/results/communication-scope-evidence.json) |
| 未归属设备事件 | page64 P1/D1的P窗口操作matched，但1312项SDMA_SQE无有效关联；其他PD亦有未知SDMA | 不按名称/时间强归属，不推定全为KV丢失；若影响必要端口/完成关系继续阻断结论 |
| 缓存与CPU值 | 已有页量/空闲变化和引用检查，不等于完整victim/LRU身份、opaque数组或KV浮点内容一致 | 只补正式公共状态/完成边界需要的事实，不扩成通用张量解释器；[依据](../../../data/20261031T061440.793094Z_s11-independent-validation/results/cache-pressure-scope-evidence.json) |
| 动态策略独立性 | 错峰/长序列主要为内部条件检查；同批次也可能有不同资源初态，给定耗时可能改变完成次序 | 原生策略比较须对应到达、资源、就绪与完成条件；不能要求两控制器始终相同，也不能忽略差异后宣称等价 |
| 历史时差 | 旧PP×PD的384ns差异缺完整旧图和源码 | 保留未解释，不以当前结果追认历史通过 |

## 7. 费用、规模与审查

| 本轮任务累计 | 实际值 | 注册上限 |
| --- | ---: | ---: |
| 原生尝试 | 93次 | 130次，含18次受控适配储备 |
| native时间 | 29991.170s | 101700s |
| processing时间 | 82441.475s | 249000s |
| 收口时文件量，含work | 168.813GiB | 640GiB临时上限 |
| 收口时保留口径，排除work | 163.481GiB | 480GiB保留上限 |

计费覆盖同一路径的来源、目标、失败、恢复、条件和生命周期，按47份去重任务状态求和；不是只算有效serving窗口，也不代表项目历史总费用。当前任务都已结算，未把活跃预留当耗时。文件量是收口快照，不是全轮瞬时存储峰值；逐任务峰值继续保存在state。全轮尚有余额不授权绕过已冻结的单域停止条件；本轮没有动用适配修改储备。

来源费用可以与后续验证分开理解：首次来源阶段47次尝试，16940.336s native、21151.554s processing；后续PP×PD来源另一次尝试，累计708.255s native、2240.569s processing，其中21.818s旧预检已包含在首次快照。扣重后来源阶段共48次尝试，约17648.6s native、23370.3s processing。这是依据当时已结算日志合并的近似值，包含启动/编译/预热/导出/建库及该正式工作流的预测，不是纯插桩或有效窗口耗时；已发布库关联的47份会话不含失败来源，不能与48次尝试混用。

首遍完整读取211个自有代码、构建及入口文件，零部分读取，跨模块审阅覆盖观察/符号、模板、控制/KV、执行、Python/C++交接、部署及评价；只读审阅发现的新增明确评价缺陷为F8，其他既有或动态暴露问题按上文分别保留。读取完整不代表所有风险关闭。[审阅范围与发现](../../../data/20261031T061440.793094Z_s11-independent-validation/results/static-review.json)。

| 当前自有源文件口径 | 文件数 | 物理行 | 非空行 |
| --- | ---: | ---: | ---: |
| src | 175 | 28073 | 24167 |
| example | 17 | 5448 | 5036 |
| docker中的源脚本 | 1 | 87 | 76 |
| scripts | 3 | 25 | 19 |

后者仅统计py/cpp/hpp/h/cc/c/sh，物理行含空行及注释，不能与211份审阅文件直接等同。B阶段业务及示例源码净增减均为0；没有为冻结调查增补测试工程、编译或新框架分支。报告汇总只读使用保留事实，最后一次完整元数据核对约6.811s，没有新Engine或仿真。

## 8. 推荐推进顺序

1. **收口评价与证据所有权。** 用保留大图作有界函数级测量，减少重复生产/解析/查询；使部分报告、覆盖降级与失败边界有可靠交接。修F8并按实测重新估算下一轮额度，不能删检查提速。
2. **修公共参数、端口和来源交接。** 统一追溯F3/F4，独立查明F5/F6，补齐规范词表输入及合法准入配方。模板条件来自原生机制，不能按case、模型名、B值或目标图兜底。
3. **闭合动作和完成关系。** 修F1/F2，核对普通、PP、PD和缓存上下文，补足独立跨rank事实；区别真正缺依赖与不同控制历史，确认Release视图生命周期风险后再定修法。
4. **完成整体维护交付。** 联查生产者/消费者和目录职责，删除替代路径、重复状态、无用输出；大范围修改后统一做必要Docker构建及有限公开入口回归。分别审查代码、文档、数据，阶段边界分层提交推送。
5. **重新完整独立验证。** 当前种子和目标转开发用途，按预定顺序启用未使用留出；保留全部模型/机制及负例义务，明确原生不支持边界，不只重跑失败例。本轮报告和失败不覆盖。新轮通过并完成维护交付后再进入多机初探及最终交接，性能层继续后置。
