# S11-B 冻结独立调查报告：2026-11-04

本轮已完成固定清单的执行和结果登记，**S11-B 未通过，S11 尚未交付为通过基线**。下一步返回共同机制诊断与修复；多机、轻量控制器推进和性能建模继续后置。

调查根为 `data/20261103T061548.320668Z_s11-independent-validation`。入口于 2026-11-04 12:44:37 UTC 已核实退出，退出码为 2；末尾列出的五个依赖受阻目标和一个额度耗尽案例均已有结果记录，不是六个遗漏任务。本轮没有自动恢复这些案例。

原始依据为[固定矩阵](../../../data/20261103T061548.320668Z_s11-independent-validation/inputs/matrix.json)、[全部结果](../../../data/20261103T061548.320668Z_s11-independent-validation/results/outcomes.json)及[机器汇总](../../../data/20261103T061548.320668Z_s11-independent-validation/results/summary.md)。以下路径除特别说明外均相对调查根；数据不随 Git 提交，复核需要保留本地材料。

## 1. 最终结果及含义

| 对象 | 固定清单 | 最终结果 |
| --- | --- | --- |
| 来源工作流 | 44 | 40 complete，4 error；发布 40 个来源库 |
| 配置 × workload 展开 | 252 | 231 项已发布；21 项因四个来源库未发布而未执行 |
| 真实目标 | 54 | 34 matched，6 partial，3 differences，6 error，5 blocked |
| 条件检查 | 31 | 20 complete，10 rejected，1 budget_exhausted |
| 生命周期 | 7 | 7 complete |
| 外层清单合计 | 136 | 136 项均有记录，无遗漏或额外案例 |

`complete` 工作流说明默认给定耗时下的正式展开和完成检查成立；它不替代真实目标对照。真实目标的 `matched` 只授予具名模型、配置、请求和观察范围的资格。`partial` 保留证据缺失，`differences` 保留实际不一致，`error` 包含预测失败，`blocked` 表示依赖未具备。

43 个实际生成预测并完成比较的真实案例，五项完成检查、内部结构/状态检查及默认与明细一致性均通过。它们仍有公开操作、设备归属、通信或输入投递上的差异，不能用内部不变量通过将整图改判为通过。其余六个预测失败和五个来源依赖受阻目标没有取得同等比较证据。

## 2. 冻结、生产身份与独立性

- 正式比较冻结于 2026-11-03 18:17:12.496744 UTC，核心与适配 Git 基线均为 `5713ed00dced950118b832c874cb3d9ecd4a1436`。本轮没有修改冻结源码、镜像、矩阵、模板规则或评价判据；收尾核对冻结路径无 Git 差异。
- 目标采集提前完成，54 份记录均由 `5fc920e9fa2db9564484c297d3f76a33a9d1c50239a4bf8be5c487e05efbacbe` 镜像生产；没有为正式比较重新运行目标。
- 51 份完整来源会话中，50 份由 `8fc4005539d3ebb234daee1829bb887b827b109f327a586f2a20abc8224eed93` 生产，`scheduling-pd-pp` 的一份来源由上述 `5fc920…` 生产。来源集合保留实际身份，没有跨镜像补角色或回写旧身份。
- 正式离线解释镜像为 `8fc400…`，控制镜像为 `94461194f31f9763d2ceec3d2046dde671ad4998531377d96694690297393bbf`，建模镜像为 `e066a5196c3b34b2f46079227e4b5c2a1a811c9d06defca329175954101ca7ca`。
- 比较冻结前的来源就绪性修正、一次有界重试、旧费用及保留目标的依据在 `results/source-readiness-change.json`；正式比较基线在 `results/comparison-freeze.json`。旧版本目标全部观察原生 worker，修正针对的未观察 worker 采集交接没有被用来改写目标事实。
- 模板从固定代表来源生成；目标只向旁路提供具名工作的边界时间。真实批次、资源分配和目标图没有作为控制或构图答案输入。

本轮没有使用备用请求种子；主种子为 311103，311129/311147 仍按预定用途保留。模型曾参与开发，不将本轮称为七个从未见过的模型；独立性来自目标用途、来源分离、规则冻结及没有用本轮差异现场修改实现。

模型使用固定完整结构及 dummy 权重。MoE 使用共同给定路由条件，路由与 padding 的计数/传输核对不代表真实权重下的自然路由或数值正确性。

| 模型 | 结构 | 固定 revision 前缀 |
| --- | --- | --- |
| Qwen3-8B | Dense，36 层 | b968826d9c46 |
| Mistral-7B-v0.3 | Dense，32 层 | caa1feb0e54d |
| Qwen1.5-MoE-A2.7B | 24 层，60 专家，Top-K 4 | 1a758c50ecb6 |
| Qwen3-30B-A3B | 48 层，128 专家，Top-K 8 | ad44e777bcd1 |
| Meta Llama-3.1-8B-Instruct | Dense，32 层；配置来自 NousResearch 镜像 | d10aef7999a2 |
| OLMoE-1B-7B-0924 | 16 层，64 专家，Top-K 8；本轮仅 TP1/EP1 | 6d84c48581ec |
| GLM-4-9B-Chat | Dense，40 层 | bd8234fe5e0c |

完整仓库来源、revision 和路径在固定矩阵中。BF16 为主，Qwen3-8B 另有 FP16 来源；attention backend、原生依赖及框架版本没有作为本轮变化轴。

## 3. 本轮流程和职责

正式任务读取配置与请求集合、划分来源域并管理累计额度。框架/runtime 适配观察真实调用、张量和执行关系；离线解释与 C++ 模板生产分开交接。模板库提供共享定义、布局、执行模式和角色绑定，原生控制器通过共同动作接口请求展开，核心资源服务维护槽位、页与前缀状态，执行器或真实时间旁路返回完成边界。

矩阵编排、真实对照及内部检查位于 `example/automatic_graph`，没有进入核心状态机。这里只检查统一流程及必要因果，不以还原所有 CPU 函数或后台事件为目标。CPU 返回、设备完成、通信与请求发布分别判定。

原始会话完成、解释完成、模板发布、默认预测完成和独立比较完成分别记账。四个建库失败域不发布空模板，后继目标按依赖受阻保留；纯离线失败不触发新的 Engine。

## 4. 全部清单

### 4.1 来源库与正式展开

库数值依次为 **定义 / 布局 / 执行模式 / 角色**；代表/派生来自采前角色计划。各域正式资产位于 `runs/<域>/assets/domain-0/templates.json`，状态在 `runs/<域>/state.json`，输入在相应 `inputs/task.json`。定义数不是物理节点数，也不能跨来源域相加当作全局唯一模板数。

| 来源域 | 工作流 | 展开/计划 | 库 | 代表/派生 |
| --- | --- | --- | --- | --- |
| qwen3-tp1 | complete | 30/30 | 130/230/63/1 | 1/0 |
| qwen3-tp2 | complete | 12/12 | 135/239/66/2 | 2/0 |
| qwen3-tp4 | complete | 4/4 | 135/239/66/4 | 2/2 |
| qwen3-tp8 | complete | 4/4 | 135/235/67/8 | 2/6 |
| qwen3-tp1-pp3 | complete | 6/6 | 177/393/66/3 | 3/0 |
| qwen3-tp1-pp4 | complete | 6/6 | 177/393/66/4 | 3/1 |
| qwen3-tp2-pp3 | complete | 6/6 | 208/510/70/6 | 6/0 |
| qwen3-tp2-pp4 | complete | 6/6 | 208/510/70/8 | 6/2 |
| mistral-tp1-pp1 | complete | 4/4 | 127/221/62/1 | 1/0 |
| mistral-tp4-pp1 | complete | 4/4 | 132/230/66/4 | 2/2 |
| mistral-tp1-pp3 | complete | 6/6 | 171/366/66/3 | 3/0 |
| mistral-tp1-pp4 | complete | 6/6 | 171/366/66/4 | 3/1 |
| qwen15-moe-ep1 | complete | 4/4 | 137/226/63/1 | 1/0 |
| qwen15-moe-ep2 | complete | 4/4 | 203/371/82/2 | 2/0 |
| qwen15-moe-ep4 | complete | 4/4 | 203/370/82/4 | 2/2 |
| qwen3-moe-ep2 | complete | 4/4 | 196/357/82/2 | 2/0 |
| qwen3-moe-ep4 | complete | 4/4 | 196/358/83/4 | 2/2 |
| qwen3-moe-ep8 | complete | 4/4 | 196/354/83/8 | 2/6 |
| qwen3-moe-ep2-pp3 | complete | 6/6 | 286/656/84/6 | 6/0 |
| qwen3-moe-pd2-2 | complete | 4/4 | 245/457/90/4 | 4/0 |
| qwen3-pd4-2 | complete | 4/4 | 195/384/74/6 | 4/2 |
| qwen3-pd2-4 | complete | 4/4 | 195/385/74/6 | 4/2 |
| qwen3-moe-pd-pp3 | complete | 4/4 | 329/689/94/8 | 8/0 |
| llama31-tp1 | complete | 6/6 | 127/221/62/1 | 1/0 |
| llama31-tp2 | complete | 6/6 | 132/230/66/2 | 2/0 |
| glm4-tp1 | complete | 6/6 | 124/208/59/1 | 1/0 |
| glm4-tp2 | complete | 6/6 | 129/217/63/2 | 2/0 |
| olmoe-ep1 | complete | 4/4 | 130/221/60/1 | 1/0 |
| qwen3-fp16 | complete | 4/4 | 130/230/62/1 | 1/0 |
| qwen3-page1 | complete | 6/6 | 115/209/52/1 | 1/0 |
| qwen3-page16 | complete | 6/6 | 130/230/62/1 | 1/0 |
| qwen3-page64 | complete | 6/6 | 130/230/62/1 | 1/0 |
| qwen3-page256 | complete | 6/6 | 130/230/62/1 | 1/0 |
| qwen3-page64-pp3 | complete | 6/6 | 177/393/66/3 | 3/0 |
| qwen3-moe-page64-ep2 | complete | 4/4 | 196/357/82/2 | 2/0 |
| qwen3-page64-pd1-1 | complete | 4/4 | 174/321/70/2 | 2/0 |
| scheduling-single | error | 0/4 | — | 1/0 |
| scheduling-tp2 | error | 0/9 | — | 2/0 |
| scheduling-ep2 | complete | 4/4 | 216/461/81/2 | 2/0 |
| scheduling-pp3 | error | 0/4 | — | 3/0 |
| scheduling-pd | complete | 9/9 | 200/403/69/2 | 2/0 |
| scheduling-pd-tp | complete | 4/4 | 222/501/73/6 | 4/2 |
| scheduling-pd-ep | error | 0/4 | — | 4/0 |
| scheduling-pd-pp | complete | 4/4 | 259/660/75/6 | 6/0 |

四个失败域的原生来源与解释事实均保留；它们停止于模板生产，不能归为原生配置不可运行。库缺失合计阻断 21 项展开和五个真实目标。

### 4.2 54 个真实目标

案例列为 `来源域/配置/workload`，后端统一为 native。逐例报告位于 `runs/<域>/results/validation/<配置>/<workload>/native/validation-<配置>-<workload>.json`，逐 rank 报告在同名子目录中；受阻案例以整轮 outcomes 为准。

“原生/配对”是公开窗口操作计数，“未归属设备/runtime”是两类未归属事件数量。破折号表示未取得相应比较证据。通信判定与本例总判定分别列出。

| 案例 | 判定 | 原生/配对 | 未归属设备/runtime | 通信 |
| --- | --- | --- | --- | --- |
| qwen3-tp1/small/boundaries | matched | 16393/16393 | 0/0 | matched |
| qwen3-tp1/r32/members-32 | matched | 7185/7185 | 0/0 | matched |
| qwen3-tp1/r33/members-33 | matched | 7190/7190 | 0/0 | matched |
| qwen3-tp1/r128/members-128 | matched | 7665/7665 | 0/0 | matched |
| qwen3-tp1/wide/long-outputs | partial | 67300/67300 | 1/0 | matched |
| qwen3-tp1/small/staggered | differences | 18128/18128 | 0/0 | matched |
| qwen3-tp2/wide/boundaries | matched | 15084/15084 | 0/0 | matched |
| qwen3-tp2/kv262144/context-boundaries | matched | 16560/16560 | 0/0 | matched |
| qwen3-tp4/wide/boundaries | error | — | — | — |
| qwen3-tp8/wide/boundaries | matched | 60048/60048 | 0/0 | matched |
| qwen3-tp1-pp3/wide/boundaries | matched | 18666/18666 | 0/0 | matched |
| qwen3-tp1-pp3/wide/staggered | differences | 18690/18690 | 0/0 | matched |
| qwen3-tp1-pp4/wide/boundaries | matched | 19968/19968 | 0/0 | matched |
| qwen3-tp2-pp3/wide/boundaries | matched | 39156/39156 | 0/0 | matched |
| qwen3-tp2-pp4/full-micro/boundaries | matched | 19100/19100 | 0/0 | matched |
| mistral-tp1-pp1/wide/boundaries | matched | 6092/6092 | 0/0 | matched |
| mistral-tp4-pp1/wide/boundaries | matched | 26024/26024 | 0/0 | matched |
| mistral-tp1-pp3/small/boundaries | matched | 28611/28611 | 0/0 | matched |
| mistral-tp1-pp4/wide/boundaries | matched | 17544/17544 | 0/0 | matched |
| qwen15-moe-ep1/wide/varying | matched | 6984/6984 | 0/0 | matched |
| qwen15-moe-ep2/wide/varying | matched | 25148/25148 | 0/0 | matched |
| qwen15-moe-ep4/wide/varying | matched | 50164/50164 | 0/0 | matched |
| qwen3-moe-ep2/wide/varying | matched | 40840/40840 | 0/0 | matched |
| qwen3-moe-ep4/wide/varying | matched | 81224/81224 | 0/0 | matched |
| qwen3-moe-ep8/wide/varying | matched | 162184/162184 | 0/0 | matched |
| qwen3-moe-ep2-pp3/wide/varying | matched | 98844/98844 | 0/0 | matched |
| qwen3-moe-pd2-2/wide/varying | differences | 78070/78054 | 3392/0 | differences |
| qwen3-pd4-2/wide/boundaries | error | — | — | — |
| qwen3-pd2-4/wide/boundaries | error | — | — | — |
| qwen3-moe-pd-pp3/wide/varying | error | — | — | — |
| llama31-tp1/wide/boundaries | matched | 6092/6092 | 0/0 | matched |
| llama31-tp2/wide/boundaries | matched | 13012/13012 | 0/0 | matched |
| glm4-tp1/wide/boundaries | matched | 8936/8936 | 0/0 | matched |
| glm4-tp2/wide/boundaries | matched | 18892/18892 | 0/0 | matched |
| olmoe-ep1/wide/varying | matched | 4168/4168 | 0/0 | matched |
| qwen3-fp16/wide/boundaries | matched | 7080/7080 | 0/0 | matched |
| qwen3-page1/small/unit-capacity | matched | 5947/5947 | 0/0 | matched |
| qwen3-page16/small/unit-capacity | matched | 5992/5992 | 0/0 | matched |
| qwen3-page64/small/unit-capacity | matched | 5992/5992 | 0/0 | matched |
| qwen3-page256/small/unit-capacity | matched | 5992/5992 | 0/0 | matched |
| qwen3-page64-pp3/wide/boundaries | matched | 18666/18666 | 0/0 | matched |
| qwen3-moe-page64-ep2/wide/varying | matched | 40840/40840 | 0/0 | matched |
| qwen3-page64-pd1-1/wide/boundaries | partial | 13390/13382 | 1312/0 | matched |
| scheduling-single/chunk-256/shared | blocked | — | — | — |
| scheduling-tp2/chunk-256/shared | blocked | — | — | — |
| scheduling-tp2/cache-pressure/cache-pressure | blocked | — | — | — |
| scheduling-ep2/chunk-256/shared | error | — | — | — |
| scheduling-pp3/chunk-256/shared | blocked | — | — | — |
| scheduling-pd/chunk-256/shared | partial | 24800/24780 | 1376/0 | matched |
| scheduling-pd/cache-pressure/cache-pressure | partial | 40789/40737 | 2136/0 | matched |
| scheduling-pd/chunk-256/arrivals | partial | 24759/24739 | 1376/0 | matched |
| scheduling-pd-tp/chunk-256/shared | error | — | — | — |
| scheduling-pd-ep/chunk-256/shared | blocked | — | — | — |
| scheduling-pd-pp/chunk-256/shared | partial | 41546/41546 | 1536/0 | matched |

### 4.3 条件检查

这些案例使用给定耗时检查结构与状态，不授予新的真实设备对照资格。正例的内部证据位于 `cases/<案例>/result.json`，负例与额度结果在整轮 outcomes；具体请求及成本条件在 `inputs/cases`。

| 案例 | 预期 | 实际 |
| --- | --- | --- |
| D-running-1-native | complete | complete |
| D-running-2-native | complete | complete |
| D-running-31-native | complete | complete |
| D-running-64-native | complete | complete |
| D-running-101-native | complete | complete |
| G-seed-64-native | complete | complete |
| G-seed-128-native | complete | complete |
| G-seed-256-native | complete | complete |
| K-qwen3-tp4-host-native | complete | complete |
| K-qwen3-tp4-device-native | complete | complete |
| K-qwen3-tp4-communication-native | complete | complete |
| K-qwen3-tp4-transfer-native | complete | complete |
| K-qwen3-moe-pd-pp3-host-native | complete | complete |
| K-qwen3-moe-pd-pp3-device-native | complete | complete |
| K-qwen3-moe-pd-pp3-communication-native | complete | complete |
| K-qwen3-moe-pd-pp3-transfer-native | complete | budget_exhausted |
| E-request-exceeds-pool-native | rejected | rejected |
| A-zero-prompt-native | rejected | rejected |
| A-type-native | rejected | rejected |
| A-duplicate-request-native | rejected | rejected |
| C-wrong-page-asset-native | rejected | rejected |
| A-wrong-topology-asset-native | rejected | rejected |
| C-equal-pages-1-native | complete | complete |
| C-equal-pages-16-native | complete | complete |
| C-equal-pages-64-native | complete | complete |
| C-equal-pages-128-native | complete | complete |
| C-equal-pages-256-native | complete | complete |
| A-missing-routes-native | rejected | rejected |
| A-duplicate-expert-native | rejected | rejected |
| A-route-gap-native | rejected | rejected |
| E-pd-decode-reservation-native | rejected | rejected |

MoE PP×PD 的 transfer 条件在约 2968.389 秒的本次可用处理额度内未完成，最终 `budget_exhausted`。该任务累计 processing 为 5400.834 秒，包含此前来源、正式比较及其他条件的处理；不能把累计值当作本次条件的独占耗时，也不把额度停止等同于结构已被证明错误。

### 4.4 生命周期

| 案例 | 检查 | 实际 |
| --- | --- | --- |
| L-cli-invalid | reject-task | complete |
| L-help-after-error | help | complete |
| L-resume-completed | resume-completed | complete |
| L-budget-stop | budget | complete |
| L-interrupt-resume | interrupt | complete |
| L-bind-input | missing-input | complete |
| L-protect-input | protect-input | complete |

中断案例确认 worker 已释放，状态从 `interrupted/pending=null` 恢复到 `complete/pending=null`，没有新增原生尝试；费用单调累计。输入保护检查覆盖结果覆盖输入、core 日志覆盖输入及 native 日志覆盖输入三种位置，三项均按边界错误拒绝且原文件不变。

七项通过不覆盖所有终止竞态。本轮 transfer 条件的超时路径另有 Docker 删除竞态，见后文。

## 5. 泛化证据与限制

| 机制 | 本轮取得的证据 | 保留边界 |
| --- | --- | --- |
| A 输入与来源 | 四十四域自动规划；十个负例按预期拒绝 | 四个库未发布，不能据其他域成功扩大其资格 |
| B 模型/dtype | 七模型各有具名基本配置 matched，Qwen3 FP16 也有真实对应 | dummy 权重、固定版本和 backend；不是任意模型或数值验证 |
| C 页布局 | page_size 1/16/64/128/256 分别建库；具名容量、尾页和 PP/EP 交互有真实对应 | 跨 page_size 共用旧库未实现；page64 PD 仍 partial |
| D 集合边界 | 真实 Prefill/Decode 达到 32、33、128 成员并 matched；1/2/31/64/101 等条件展开完成 | 容量上限与实际批次人数分别判断，不授予所有拓扑相同人数 |
| E KV容量 | 32 页来源到更大或更小池；page1 的 257-token 请求有实际 257 页需求证据；TP2 的 KV262144 目标 matched | KV262144 是容量，不表示实际填满；不证明 KV 数值正确 |
| F 请求几何 | 页边界、长输出和 2047/2048/2049 及 2305 上下文目标有具名证据 | 长输出 TP1 仍有一项设备任务未归属，不声明连续任意长度支持 |
| G 到达与序列 | 64/128/256 请求条件序列完成；单worker、PP、PD均有错峰真实案例 | 单worker和PP的提前发送检查失败；PD发送检查通过仍无调度器接收时刻 |
| H TP/PP | TP1/2/8、PP3/4、TP2×PP3/4具名目标 matched，包含不均匀层划分和未观察角色 | TP4真实旁路仍报错；通过某TP规模不证明所有规模 |
| I EP | EP1/2/4/8、TP2×PP3×EP2具名目标 matched，热点/变化路由与 padding 被实际核对 | 固定给定路由；原生不支持的 mixed/MoE 组合仍不纳入 |
| J PD | Dense PP×PD公开窗口及通信对应；具名普通PD、page64 PD有部分对应 | 非对称TP、MoE PP×PD、调度EP/PDTP有反馈错误；MoE PD有通信差异 |
| K 完成与资源 | 43例完成/内部/默认明细检查通过；七个耗时条件完成 | 第八个耗时条件额度耗尽；内部检查不能覆盖未知设备完成 |
| L 生命周期 | 七项完成，所有46份任务状态pending为空，退出后无本调查容器运行 | 超时删除竞态已触发，另有两项静态恢复/评价缺陷 |

### 5.1 角色复用的实际含义

44 个采前计划共有 144 个逻辑角色，选择 114 个代表、派生 30 个角色，减少约 20.8% 的角色观察/导出。联合来源中未记录的 worker 仍可能必须参加原生通信，因此这不是减少 30 次 Engine、30 个进程或 20.8% 墙钟开销的证明。

已有 8 个具名域、22 个派生角色取得目标真实 worker 的完整对应：`qwen3-tp8`、`qwen3-tp1-pp4`、`qwen3-tp2-pp4`、`mistral-tp4-pp1`、`mistral-tp1-pp4`、`qwen15-moe-ep4`、`qwen3-moe-ep4`、`qwen3-moe-ep8`。其余派生域仍受各自反馈错误限制，不把库已生成当作复用已通过。

四十个正式库合计覆盖 134 个角色，其中 104 个代表、30 个派生；失败域的十个代表只有来源事实，没有取得正式库资格。模型、TP/PP/EP、dtype、backend、page_size 和机制域分别维护来源，同库主要变换请求、成员、到达、并发及 KV 容量。

## 6. 旧问题的最终开发回归

以下是正式 B 开始前的开发证据，不计入本轮独立通过数。完整输入和回归结果保留在 `data/20261103T081205Z_s11-capture-decoupling` 及两个 `data/20261103T133644.873015Z_s11-*-original-case` 任务中。

| 旧问题 | 最终开发证据 | 限制 |
| --- | --- | --- |
| F1/F2 结果顺序与空协调 | 原 EP4 四rank共50164公开操作、24模型主体、739项通信/5912条完成关系 matched | 逐worker返回与通信组分开；只代表该原例 |
| F3 TP2×PP4参数交接 | 1502动作、11逻辑批次、八池，五项完成及19内部检查通过 | 给定耗时开发回归 |
| F4 page1局部作用域 | 原输入110动作、12逻辑批次完成；206页allocator原语另有证据 | 原短prompt回归不冒充大分配serving；本轮257页证据单列 |
| F5 MoE PD筛选 | 2080动作、13逻辑批次，实际14次非空成员减少，完成及内部检查通过 | 局部筛选证据不替代本轮PD整图资格 |
| F6 Dense PP×PD | 原例43097公开操作、149通信、950完成关系及432切片对应 | 仍有1536个设备任务未归属；与本轮41546操作的目标分别记录 |
| F7 GLM词表交接 | 原例110动作、12逻辑批次及完成检查通过 | 本轮GLM的TP1/TP2独立证据另列 |
| F8 错页拒绝 | 按稳定的no region layout matches语义拒绝 | 不再依赖表达式打印顺序 |
| F9 评价开销 | 已有函数测量、共享观察及分组索引收紧，最终两个原例完成全部rank评价 | 没有全域速度倍率；本轮transfer耗尽原因未测明 |
| F10 配方/准入 | 545+512>1024的请求被拒绝，128成员配方预算修正 | 本轮实际128成员对照和PD预留负例分别确认 |

A 的六个最终接口回归共 processing 224.862 秒；详细图检查、105 份 Python 的 3.10 语法/Ruff 核对共 98.427 秒，79 份矩阵输入重生成后保持原内容。A 的 Release 构建与安装依据保留；B 没有业务代码修改，没有为文档重建、追加测试或重跑这些开发回归。

## 7. 问题清单与推荐顺序

下面区分 **已发生的失败**、**源码确认的缺陷**和**尚未证实的风险/归因**。异常在核心抛出不等于根因必在核心；适配器提供事实，核心消费共同合同，两侧前提都要核对。

### Must Fix：取得S11资格前必须关闭的缺口

**M1：语义入口存在未命名的 attention mask 输入。严重程度 P1；失败已确认，责任归属待判。**

- 触发：`scheduling-single`、`scheduling-tp2`、`scheduling-pp3` 建库，分别报 region/value 为 874/2874、988/3098、1331/3487 的 unnamed input；阻断17项展开和四个真实目标。
- 证据：三份 `work/sources/domain-0/.../rank-0.json` 中，这些值均是无记录生产者的 NPU `torch.int8[2048,2048]`，作为 `npu.npu_fused_infer_attention_score.default` 的 `atten_mask`。编译后的 region 编号不能直接当原始 regions 数组下标。
- 位置：[模板输入检查](../../../src/markov/production/template_library.cpp)第769行；[SGLang入口资源声明](../../../src/collection/markov_collection/adapters/frameworks/sglang/source.py)第167–180行；完整错误在各域 `logs/run.log`。
- 建议：沿外部值的创建、持续状态登记、入口命名和区域边界统一核对资源交接。保留核心禁止匿名外部输入的合同；不按这三个case、2048尺寸或mask名称补白名单，也不把未知输入补零。

**M2：PD×EP调度来源建库遇到空JSON。严重程度 P1；失败已确认，具体空值与责任待判。**

- 触发：`scheduling-pd-ep`，P/D各TP2/EP2，四角色已采集和解释，`templates` 报 `[json.exception.type_error.304] cannot use at() with null`。
- 影响与证据：正式库不存在，四项展开和一个真实目标受阻；`runs/scheduling-pd-ep/logs/run.log`、对应 `work/sources` 及 `state.json` 保留原事实。
- 建议：定位第一个空值及其生产者，明确该事实的必需/可选含义，再修共享交接。当前没有定位到准确源码行，不能归为原生模型不可运行、给核心加空对象兜底或靠重采掩盖。

**M3：六个真实目标的控制边界对应失败。严重程度 P1；失败已确认，共同根因尚未成立。**

涉及 `qwen3-tp4` 的空成员select/running、`qwen3-pd4-2`、`qwen3-pd2-4`、`qwen3-moe-pd-pp3`、`scheduling-ep2` 和 `scheduling-pd-tp`。后五域分别在poll或发布边界停止，均没有完整rank/通信比较资格。

位置为 [ObservedFeedback](../../../src/markov/execution/observed_feedback.cpp) 的 submit/advance_until。第124行只检查“是否有发布”与生成程序 publication 端口存在性一致；第138行同时检查“下一事件早于当前时刻”和“对应工作尚未提交”，当前报错不能区分后二者。不能将发布存在性不符写成已证明的端口shape/名称错误，也不能把派生rank报错直接归为角色复制错误。

建议从真实边界记录生产、语义工作身份、动作提交、程序发布及事件推进的完整链路推导统一不变量，并定位每例第一处分歧。先判断共享机制与适配事实，允许整段重写职责；不按rank编号、批次序号或workload标签调整时间或塞入动作。

**M4：五个PD目标存在公开操作缺失，一例还有通信差异。严重程度 P1；差异已确认，原因待判。**

- `qwen3-moe-pd2-2` 有16项原生公开操作未对应；`qwen3-page64-pd1-1`为8项；`scheduling-pd`三例分别20、52、20项。问题集中于P侧，D侧对应不自动证明P侧正确。
- MoE PD通信有491项问题、3896项未验证；已有523项检查、624条完成关系和1536个传输切片。`native_peer_pairing` 是已见问题类型，不能推定491项全同根。判据位置包括 [CommunicationComparison](../../../example/automatic_graph/communication.py)第253行。
- 建议统一核对公开调用范围、重复调用身份、跨调用数据/通信端点及必要因果。先区分真实结构漏项、采集交接和评价配对问题；不按操作名删除差异，也不仅用总数对齐。

**M5：设备任务归属仍有未知。严重程度 P1；缺证据已确认，必要因果与具体来源待判。**

具名计数见4.2：TP1长输出1项，page64 PD为1312项，普通调度PD为1376/2136/1376项，MoE PD为3392项，PP×PD为1536项。最后一例三个P rank各512项、D侧全部归属；其41546项公开操作及142项通信/922条完成关系/432个切片全部对应，仍保留partial。

建议按统一流程和必要因果，将原生API、私有队列或opaque完成范围与观察任务建立来源明确的归属。若确属流程外工作，需要机制证据说明范围；已知SDK可能走私有队列，不是这些任务全部来自SDK的证明。不扩张为还原所有后台事件，也不添加任务名称忽略名单。

**M6：错峰输入的提前发送判据失败。严重程度 P2；失败已确认，采集时基或投递机制原因待判。**

`qwen3-tp1/small/staggered` 与 `qwen3-tp1-pp3/wide/staggered` 的 `not_submitted_early` 为false，后者最小延迟为−57480ns；其他公开窗口、通信和内部检查对应。判据在 [input_delivery](../../../example/automatic_graph/evidence.py)第388–411行，比较 submitted_ns 与 arrival_ns。

建议核对请求到达定义、共同时间原点和发送记录位置，不添加容差凑通过或更改输入到达。现有证据只覆盖sender release；即使PD错峰的三项发送检查为true，也没有观察调度器接收时刻，不据此声称两侧接收和组批时点已证明一致。

**M7：传输耗时条件未完成。严重程度 P2；额度不足结果已确认，计算慢的机制原因未定位。**

`conditional/K-qwen3-moe-pd-pp3-transfer-native`耗尽当前处理额度，未取得其预定正例资格。当前没有过程测量证明是哪一段造成长耗时，也不能从CPU使用率、最终内存快照或操作数量推断根因。后续先从控制推进、活动图生命周期与重复遍历推导规模，必要时才对保留事实做有界测量；不直接提高额度再跑同例，或将其从本轮清单删掉。

### Should Fix：已确认的收尾与维护缺陷

**S1：超时清理与Docker自动删除发生竞态。严重程度 P2；本轮实际触发。**

位置：[NativeExecution.connect/cleanup](../../../src/application/markov_application/native.py)第151、163、177行。transfer案例先出现TimeoutExpired，终止过程中收到SIGTERM，随后 `docker rm -f` 返回 `removal of container … is already in progress`，CalledProcessError成为最终清理异常；完整堆栈在该域 `logs/run.log` 第151行起。

建议收紧退出、容器删除和错误报告的所有权，保留首个失败，避免把已由生命周期负责删除的容器再次当作必须成功删除的对象。不能全局check=False吞掉真实清理失败。退出后未见本调查容器存活，只证明本轮容器结果，不证明不存在所有资源泄漏。

**S2：forward包装失败时恢复顺序不安全。严重程度 P2；源码确认，本轮未触发。**

位置：[ModuleObservation](../../../src/collection/markov_collection/adapters/runtimes/pytorch/modules.py)第64、66、90–97行。恢复记录在包装器构造和赋值前登记；若构造失败且原forward来自类，restore会删除尚未创建的实例属性，掩盖原异常并停止后续恢复。建议覆盖成功后才登记记录，不增加多层存在性检查。

**S3：生命周期负例重执行可能借用旧日志通过。严重程度 P2；源码确认的条件缺陷，本轮没有误通过证据。**

位置：[Investigation.public/lifecycle](../../../example/automatic_graph/investigation.py)第375、481、491行。public追加同一日志，拒绝与预算负例却搜索累计全文；当同一案例重执行且本次是其他非零错误时，旧ValueError/BudgetExceeded可能被误用。建议只读取当前调用的输出区间，保留历史日志。本轮七项生命周期仍按实际首次执行结果记录，不因静态风险将其改判。

### 风险：需要验证归因

**R1：图片段入口/尾部查询重复扫描历史依赖。** [ExecutionGraph](../../../src/markov/execution/protocol.cpp)第130/143行每次遍历全部dependencies后筛选，PD [展开调用方](../../../src/markov/adapters/frameworks/sglang/disaggregated.cpp)第242行逐动作查询terminal_nodes。明细保留历史时会累计重复扫描。

这是源码确认的重复工作及规模风险，尚无测量将其归为transfer额度耗尽的原因。后续优先让查询消费本次新增片段的依赖区间，保持原语义；不先增加另一套缓存、索引管理器或验证平台。

## 8. 开销、数据及维护边界

| 项目 | 累计实测/快照 | 口径 |
| --- | --- | --- |
| 原生尝试 | 107 / 上限133 | 任务入口尝试，含历史失败/中断；不是Engine或rank进程数量 |
| 原生计费 | 35234.516秒 / 上限111600秒 | 包含初始化、预热、采集导出及失败等，不是净模型执行 |
| 处理计费 | 50303.157秒 / 上限253500秒 | 包含解释、建库、预测、比较、生命周期及失败 |
| 正式比较冻结后的增量 | native15897.526秒；processing49136.252秒 | 由comparison-freeze启动快照与最终46份状态差额取得 |
| 当前总逻辑大小 | 61316829927字节，约57.11GiB | 按本轮存储口径计文件逻辑大小；上限640GiB |
| 当前保留区 | 37782355413字节，约35.19GiB | 排除work路径；上限480GiB |
| 最大已记账任务数据峰值 | 10938508833字节，约10.19GiB | scheduling-pd-ep；不是内存，也不是整轮连续峰值 |
| 代码规模 | src 31174行；example/automatic_graph 6410行 | 含空行/注释，第三方/deprecated/构建/数据不计；B无业务源码增减 |

51份来源和54份目标的完整会话摘要均存在；下表按会话摘要累加，不含未发布会话的完整阶段，不代替上面的累计计费。

| 完整会话 | 初始化/秒 | 预热/秒 | 采集及flush/秒 |
| --- | --- | --- | --- |
| 来源51份 | 3176.483 | 1512.645 | 11203.460 |
| 目标54份 | 3494.774 | 3350.480 | 7900.600 |

`capture_and_flush` 包含采集及导出，不能称为无采集扰动的有效推理时间；初始化和预热也不进入未来稳态性能预测。整轮总存储连续峰值没有单独记录，只有上限检查、当前大小和逐任务已记账峰值。

结束时46份相关任务状态pending均空，容器检查没有本调查仍存活的容器。来源、目标、正式库、失败日志、费用及诊断所需未发布事实保留；本次汇报没有额外清理这些资产，没有新增原生或验证案例，也没有为文档重编译。

本轮代码维护证据为最终A关联审查、冻结依赖复核，以及上述跨模块源码检查；没有将Ruff、编译或结果计数当作全部逻辑正确性的证明。Git工作区的reference迁移、忽略配置、独立example和用户截图不属于本调查交付，保持原状。

## 9. 后续修复顺序

1. 先关闭来源事实到语义入口的统一资源合同：M1与M2分别定位生产者和消费者，保留两种失败，不能假定同根。使用既有完整来源离线推导，避免再次启动Engine。
2. 沿共同动作/事件接口收口M3：统一工作身份、发布与CPU/设备/通信完成含义，逐例定位第一处分歧；同时确认控制、资源和执行各自唯一状态，不在adapter中复制调度器。
3. 按统一来源归属和端点规则处理M4/M5，再处理M6投递时基；每项明确核心、框架/runtime适配或评价责任。内部不变量与独立真实证据保持分开。
4. 集中处理S1–S3；对M7/R1先做规模推导，再决定最小测量或结构重写。删除旧职责链，避免逐case分支、默认空值或新的管理层。
5. 用本轮保留的ground truth进行开发回归，只重新运行必要的解释、仿真与比较。已用于修正规则的目标转为开发证据；需要独立资格时按固定备用留出和用途边界安排，不自动重新采集54个目标。
6. 共同机制修复、关联代码审查、数据/文档整理及阶段交付完成后，再开始完整独立验收。当前报告和失败保持历史事实，不以只重跑失败项或给定耗时通过代替整轮资格。

本轮停止于调查结果和修复交接，S11未通过，多机探索及性能层未启动。正确结构是性能预测的前提；真实成本、通信争用及CPU服务/等待仍需自己的建模与测量，当前没有MAPE或性能精度结论。
