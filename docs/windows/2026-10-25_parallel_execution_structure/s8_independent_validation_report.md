# S8 全流程独立验证与代码审查报告

日期：2026-10-27。状态：本轮调查完成；S8 未通过验收，未交付。按用户要求，汇报完成后暂停 goal；本报告不启动修复、补采或后续阶段。

## 1. 结论先说清楚

原计划的 114 项均有终态，没有尚在运行、预算暂停或遗漏的根案例。调查完成与能力验收是两件事：本轮取得了 TP、部分 EP 和代表 worker 复用的局部独立证据，同时暴露了共同采集准备、实际设备实现选择及验证工具的缺陷，不能宣称执行结构已全面正确。

主要结论：

- 36 个来源任务中，18 个发布模板库，另 18 个未发布；失败中 16 个被设备占用预检阻断，Phi3 遇到固定原生内核限制，OLMoE 遇到本项目适配缺陷。
- 208 个正式配置×请求组合中，112 个得到条件预测结果，6 个因实现变体缺失失败，90 个因没有来源库而未执行。请求完成不等于原生结构匹配。
- 41 个预选原生目标中，22 个真正完成原生运行，1 个在设备预检受阻，18 个因来源未发布而阻断。22 个完成运行的案例中，14 个的全部 rank 局部报告 matched；完整案例仍受其他判据影响，原状态不改写为通过。
- 逐 rank 比较共 60 条：37 matched、22 differences、1 error。另一个完成八 rank 原生运行的 TP2×PP4 案例因预测先失败，没有逐 rank 预测对照，不能把这八个观察冒充比较通过。
- 确认问题共 9 项：7 项 Must Fix、2 项 Should Fix。多数失败属于少数共同机制，不能按失败案例数当成几十个独立缺陷。原生不兼容、外部设备受阻和未定性的调度/初态差异分别列出。
- 七个生命周期案例均完成；一项给定 host 耗时条件通过全部现有时间线检查，P/D 案例也通过八次 handoff 的现有执行检查。检查器在其他路径有缺陷，这些局部通过不能替代全面因果证明。

冻结期间没有修改业务代码、adapter、镜像、来源配方、目标输入、模板或比较判据，没有现场补模板、重试失败来源、替换模型或覆盖失败。不同来源域按预定配方进行首次采集，目标观察仅用于对照。
## 2. 冻结范围、运行方式和结果口径

代码基线为 d49f57adcb9a7b8388729d390c524ef12a16ad6c；冻结时间 2026-10-26T22:04:01.548261+00:00。冻结目录包括 src、example、docker、scripts、third_party 及正式构建/项目配置；调查结束后与该基线比较无差异。阶段前置文档提交不改变这份代码基线。

| 镜像 | 固定 image ID |
| --- | --- |
| markov-next-sglang:ubuntu22.04 | sha256:cf828cd2eb64c665e0f2afe63ade4bbf18ab33db569a7b830c6870bf1b721b4e |
| markov-next-modeling:ubuntu24.04 | sha256:e066a5196c3b34b2f46079227e4b5c2a1a811c9d06defca329175954101ca7ca |

原生为固定 SGLang/Ascend 栈、8 张 910B3，CANN 9.0、Torch/Torch NPU 2.10；原生源码固定到 0460f277b770f8b5a404da2e50e7a9573ef8db40。采集运行完整官方模型结构的 dummy 权重，不裁层、不缩专家，不评价文本或自然路由精度。EP 使用事先给定的逐拓扑 workload 路由并核对实际消费；未把 dummy 权重的自然路由当成真实路由预测。

正式入口依次执行来源域 → 预选原生目标 → 边界/耗时条件 → 生命周期。源和目标共享原生 serving 底层与状态生命周期；目标采用原生合法组批，轻量控制器不承诺复制原生组批顺序。采集保留语义记录及 Torch NPU/CANN 完整事实，离线解释后生成模板；目标观察不参与建库。

权威原始结果为 [逐例结果](../../../data/20261026T192126.898346Z_s8-independent-validation/results/outcomes.json)，运行次序及最后的 investigation_complete=true、pending=[] 见 [运行日志](../../../data/20261026T192126.898346Z_s8-independent-validation/logs/investigation.log)；[冻结清单](../../../data/20261026T192126.898346Z_s8-independent-validation/inputs/matrix.json)保存具名案例和输入。源码审查事实及定位汇总在 [审查记录](../../../data/20261026T192126.898346Z_s8-independent-validation/results/code-review.json)。这几份文件中的原始结果保持，不用本文解释覆盖原状态。

### 状态含义

| 状态 | 本轮数量 | 含义 |
| --- | ---: | --- |
| complete | 20 | 相应入口及其判据完成；含 12 个正式任务、1 个条件案例、7 个生命周期案例 |
| partial | 6 | 来源已发布，但该正式任务有一个预测组合失败 |
| error | 54 | 已执行并报错；须继续区分工具、项目、原生及设备预检原因 |
| blocked | 26 | 共享来源未发布，18 个原生目标及 8 个条件案例没有执行 |
| rejected | 8 | 负例被按预定错误入口拒绝 |

以上数量按 114 个根案例统计。208 个正式配置×请求组合是来源任务内部的另一个统计层次，不能与 114 相加为独立原生运行数。每个根案例均有结果，但被依赖阻断的下游没有得到执行证据；这不是完成了所有计划配置的覆盖。

## 3. 模型、配置与请求覆盖

### 六个模型

| 模型 | 官方配置 revision | 完整结构 | 本轮结果 |
| --- | --- | --- | --- |
| Qwen/Qwen3-8B | b968826d9c46dd6066d109eabc6255188de91218 | 36 层 | TP1/2/4/8、PP3/4 及 TP2×PP 已建库；部分大微批次拒绝。page64 P/D 已建库并运行目标，但观察/比较有缺口。 |
| mistralai/Mistral-7B-v0.3 | caa1feb0e54d415e2df31207e5f4e273e33509b1 | 32 层 | TP1/4 局部对照匹配；PP 不均匀层区间主体对应，但窗口/资源快照存在差异。 |
| Qwen/Qwen1.5-MoE-A2.7B | 1a758c50ecb6350748b9ce0a99d2352fd9fc11c9 | 24 层、60 专家、Top-K4 | EP1/2/4 建库；EP2/4 局部对照及路由匹配，EP1 出现 addmm 实现差异。 |
| Qwen/Qwen3-30B-A3B | ad44e777bcd18fa416d9da3bd8f70d33ebb85d39 | 48 层、128 专家、Top-K8 | EP2/4 局部对照及路由匹配；EP8、PP/PD 组合未取得本轮来源。 |
| microsoft/Phi-3-mini-4k-instruct | f39ac1d28e925b323eae81227eaba4464caced4e | 32 层 | 首次 TP1 在原生预热中失败；TP2 被设备预检阻断。新 Dense 泛化资格未取得。 |
| allenai/OLMoE-1B-7B-0924 | 6d84c48581ece794365f2b8e9cfb043c68ade9c5 | 16 层、64 专家、Top-K8 | EP2/4 被设备预检阻断，EP2×PP3 在本项目初始化适配时失败。新 MoE 泛化资格未取得。 |

Phi3 与 OLMoE 是本轮两个未参与前期开发的新模型，二者均未取得完整来源到独立目标的资格；不能用其他四个既有开发模型的成功替代该要求。没有在本轮现场为新模型增加专用 adapter。

### 实验轴、选点与实际覆盖

使用单因素边界、成对交互和少量高风险组合，不做六模型×全部参数的笛卡尔积。配置集合与 workload 按来源域组织，既验证模板内变换，也验证需要重新采集的新模型/TP/dtype/page size。下表同时列出已运行事实及未证明部分。

| 方向 | 预定覆盖 | 实际结果与边界 |
| --- | --- | --- |
| A 输入及来源规划 | 多配置/请求、类型、重复身份、路由、错误资产 | 八个负例按预期拒绝；页资产负例实际拒绝但摘要判据误记 error，见 R5；非法 CLI 后其他案例继续 |
| B 模型/dtype | 六模型，Dense BF16/FP16，新 Dense 与新 MoE | 四个既有模型取得来源；两个新模型未通过；FP16 来源被设备预检阻断，不能宣称 dtype 泛化完成 |
| C 页布局 | 1/16/64/128/256；等 token、等页容量与 PP/EP/PD 交互 | page128 多域取得证据；page64 P/D 条件预测完成并取得原生观察，但存在 R9 和比较差异；其他新页布局及交互被来源失败阻断，未判断其原生可行性 |
| D 集合与人数 | 1/2/3、31/32/33、64、101/128 | 原生实际达到 32/33/128 成员；128 是 Decode，两轮各 128，Prefill 为四轮各 32；其余条件检查多受 R2 阻碍，不把容量上限当实际人数 |
| E KV 容量/背压 | 4,096/16,384/65,536，选定域 262,144 token | 条件预测及最终页回收有结果；TP2 的 262,144 token 与上下文边界原生局部匹配；超池负例先触发原生输入长度上限，未单独证明仅 KV 不足的拒绝路径 |
| F 请求几何 | 页两侧、多页、1/2/32/64 输出、2047/2048/2049 等上下文点 | TP1 最长输出 64，63 轮 Decode 的局部结构匹配；TP2 上下文边界匹配；未验证所有连续长度、滑窗模型或任意长上下文 |
| G 到达与长序列 | 同步/错峰、突发/空闲、固定种子 64/128/256 请求 | 正式条件预测及独立案例已执行；G 组后置检查先遇 R2，R1 的 control 身份问题尚未获得单独运行复现 |
| H TP/PP/代表复用 | TP1/2/4/8、PP3/4、TP2×PP3/4，不均匀层区间 | TP 及未采集 worker 的局部匹配成立；PP 大微批次遇 R3，宽配置窗口/预热及小池状态仍有差异 |
| I EP/路由 | EP2/4/8、PP×EP、均衡/热点/变化/零接收/padding | Qwen1.5 与 Qwen3 EP2/4 实际 ID、专家量及 split 匹配；EP1 设备实现错；EP8 与新组合来源受阻，不授予其泛化资格 |
| J PD 组合 | 对称/非对称 TP、PP/EP 联合布局，八卡内 | 本轮组合来源多被设备预检阻断；page64 PD1→1 完成原生和 8 次 handoff 的模拟执行检查，但 rank0 观察失败、rank1 窗口不匹配 |
| K 必要因果 | 分别改变 host/device/communication/transfer 耗时，PP/PD 压力 | TP4 host 条件的 219,776 节点、6 动作检查匹配；另三类遇 R2；PD 组合耗时案例被来源失败阻断，不以空 handoff 集合的真值证明 PD |
| L 生命周期 | 失败后继续、恢复、中断、预算、纯预测隔离、输出保护 | 七项具名案例均 complete；中断后恢复未采新来源、费用不倒退；纯预测仅挂程序/输入/资产且 network=none；不等于验证所有崩溃和并发环境 |

未选笛卡尔组合由上述具名机制案例提供有限相关证据，并未获得逐配置运行资格。已选但受阻项完整保留在第 8 节，不能按“机制相同”将它们补写成通过。

## 4. 已取得的执行结构证据与尚未解释的差异

### 4.1 TP 与代表 worker

Qwen TP1 的 32、33、128 成员、最长 64 输出，TP2 的普通及大 KV 上下文，TP4/8 的全部实际 rank，共八个目标、二十份 rank 报告 matched。Mistral TP1/4 再有五份 matched。对应的是公开几何、操作/端口、可观察执行关系和命名接口范围，不是时间预测或任意分布式协议的证明。

Qwen TP4 和 TP8 来源均只观察 rank0/1；目标分别实际运行四/八 worker，未观察过的 rank2/3 或 rank2–7 从 peer 代表绑定后与实际观察匹配。目标 trace 没有参与角色分类或建库。观察减少发生在启动规划阶段，真实目标仍运行全部协议参与者。

### 4.2 EP 的条件路由与设备工作

| 来源域 | 目标 rank 数 | 专家调用 | 路由行 | 其中 padding 行 | 路由差异 | 逐 rank 结构 |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| qwen15-moe-ep1 | 1 | 144 | 25,392 | 0 | 0 | 有 differences |
| qwen15-moe-ep2 | 2 | 288 | 25,440 | 48 | 0 | 全部 matched |
| qwen15-moe-ep4 | 4 | 576 | 25,632 | 240 | 0 | 全部 matched |
| qwen3-moe-ep2 | 2 | 576 | 50,880 | 96 | 0 | 全部 matched |
| qwen3-moe-ep4 | 4 | 1,152 | 51,264 | 480 | 0 | 全部 matched |

这些调用由固定 workload 的请求、层、逻辑 token 和补齐位置解释，核对实际原生消费的专家 ID、专家计数及逐 peer split。路由相同不等于设备实现相同：Qwen1.5 EP1 首 Prefill 的 24 个带 bias 矩阵计算实际选择另一套内核，预测仍保留旧模式，见 R8。

### 4.3 PP 的模型工作、控制顺序与预热

四个宽配置 PP 目标（Qwen TP1×PP3/PP4、TP2×PP3、Mistral TP1×PP4）共 17 rank、221 个已配对 flow 的检查为真；整窗口仍各多两个 batch_filter(1→0) 接口，并有跨 flow 编排差异。它们是接口节点，不应误说成两个额外 Tensor empty；也未证明其时间影响可忽略。

Mistral PP3 小池的层区间为 [0,10)、[10,21)、[21,32)，66 个配对模型主体检查为真，但每 rank 22 flow 仅 10 个全部检查为真。其余有池前后状态、公开端口、释放页号和 slot 差异，批次顺序也不同。给定耗时与真实耗时、两种控制策略都会改变全局状态发生的位置，因此尚不能从同一语义 flow 的资源快照不同推出 KV 所有权错误；也不能忽略它们宣称 serving 等价。

TP2×PP4 full-micro 的八 rank 原生运行完成，实际首 Prefill 八成员、1044 token；预测却报没有匹配的 selection 2，证明它是本项目能力缺口，而非该原生配置不能运行。

统一预热还有独立的确定缺陷：Qwen TP1×PP3/PP4、Mistral TP1×PP4 的模型 stream 在固定 warmup 后为 190,840,832 字节，目标 baseline 后变为 197,132,288，增加 6 MiB；另一 stream 379,584,512 不变。来源对应模型 stream 没有发生这次增长。TP2×PP3 在该目标下未增长，不能说每个 PP 都必然扩容。R7 说明当前“统一预热起点”没有覆盖实际模型执行上下文，不能从 policy.kind 字符串推定已经满足。

### 4.4 P/D 与非 128 页布局

page64 的 PD1→1 来源发布及四个正式预测均完成；独立目标两服务实际运行完成，默认/明细一致，请求及页回收检查为真。执行证据包含 117,541 节点、50 动作、8 次 handoff，现有依赖/资源/交接检查 matched，具有实际非空 handoff。

原生 Prefill 可一次容纳多请求，观察出现 591 行矩阵，但查询合同只有 193 行上界，导致 rank0 的观察图产生 144 个 gap，见 R9。Decode rank1 有四个配对 flow，另有5个原生批次、14个目标flow未配对；窗口操作数3801对5951且组批顺序不同。配对结果中一项操作序列在batch_accept/batch_filter处分歧，另一项batch_filter的历史输入成员数为原生1、目标3，公开端口及相关复制/执行关系不一致；还有一项外部前沿为partial。不能在Prefill观察缺失、历史状态不同的情况下直接归因于KV或模板变换错误，也不能将它们略去后授予跨实例等价。

## 5. 确认问题清单与修复建议

以下是审查结论，修复尚未实施。运行确认、静态确认及未定性风险分别注明。按 Must Fix、Should Fix 排序；本轮没有单列 Nice to Have。

### R1 · Must Fix · 执行证据检查器把空闲控制工作当成有action的请求动作

**确认程度：** 静态确认；实际G组检查先被R2阻断，未单独运行到此错误。

**位置：** [example/automatic_graph/evidence.py:48](../../../example/automatic_graph/evidence.py#L48)；关联 [src/markov/adapters/frameworks/sglang/structure.cpp:244](../../../src/markov/adapters/frameworks/sglang/structure.cpp#L244)、[src/markov/adapters/frameworks/sglang/batches.cpp:65](../../../src/markov/adapters/frameworks/sglang/batches.cpp#L65)、[src/markov/workflow/serving.cpp:94](../../../src/markov/workflow/serving.cpp#L94)、[example/automatic_graph/investigation.py:303](../../../example/automatic_graph/investigation.py#L303)。

**触发与影响：** 错峰请求使控制器在两次到达之间清理已结束批次，并用--details输出该次合法执行。 ServingStructure::idle给新增区域写control身份，不写action；ServingWorkflow把它们记入controls。ExecutionEvidence逐节点直接读取region['action']，因此会在合法的空闲清理区域抛出KeyError。 条件预测本身可能已成功，后置证据检查却失败；该案例被记录为error，无法检查后续依赖/输出不变量。问题位于example验证层，不能据此判定核心展开失败。

**证据：** 现有五个 TP1 staggered 条件结果各有三个 control 决策；idle 区域只写 control，检查器却无条件索引 action。冻结 G 组先遇 R2，未运行到这一行，因此本项是静态确认。 具体案例及原始位置见本节后的逐例表和审查记录中同名问题。

**建议修复：** 整轮报告完成后，在验证层用明确的action/control身份解释节点归属；共同检查时间线、资源和依赖，分别核对decisions与controls的完成边界。保留核心已有的两种身份，不增加框架或workload特判。

**必要复核：** 复用冻结后的真实错峰结果核对检查器能覆盖control节点，并确认缺依赖、重复输出和提前完成仍会失败；不补模板。

### R2 · Must Fix · 未完成时间线节点让证据检查器提前抛出缺键错误

**确认程度：** 运行确认；前期开发与本轮冻结矩阵均实际触发。

**位置：** [example/automatic_graph/evidence.py:29](../../../example/automatic_graph/evidence.py#L29)；关联 [example/automatic_graph/evidence.py:24](../../../example/automatic_graph/evidence.py#L24)、[src/markov/execution/executor.cpp:213](../../../src/markov/execution/executor.cpp#L213)、[src/markov/workflow/serving.cpp:126](../../../src/markov/workflow/serving.cpp#L126)。

**触发与影响：** 读取已有PP4完整预测：control_status=complete，但最后的设备空闲页拼接仍有一个running和一个waiting节点。 check同时构造all_nodes_completed_once和nonnegative_intervals；尚未检查前者就对全部节点读取started_ns/completed_ns。running节点只有expected_ns，waiting节点没有起止时间，因而直接KeyError，无法输出已准备好的失败判据。 原生对照仍会运行，但案例的执行证据被记录为工具错误。检查器也混淆了控制端口完成与所有异步工作排空，需要先澄清再评价PP结果。

**证据：** 冻结目标及 12 个条件案例实际出现 completed_ns 缺键。已有 PP4 明细在控制终止 291633ns 时仍有合法 running/waiting 的设备清理尾部；运行态只含 expected_ns，等待态无起止时间。 具体案例及原始位置见本节后的逐例表和审查记录中同名问题。

**建议修复：** 整轮报告后先明确控制完成端口、异步尾部和最终窗口结束的合同。检查器应先处理实际节点状态并输出可解释结果，再核对必要端口和依赖；不要按PP或kernel名字跳过，也不要给每个动作强加全局屏障。

**必要复核：** 用保留的PP4完整图及冻结矩阵PP结果确认未完成节点会产生结构化证据；同时核对异步动作允许的尾部、真正必要完成端口及默认/明细一致性。

### R3 · Must Fix · PP模型的矩阵算法选择与具体stream身份耦合，已有原型无法供目标调用使用

**确认程度：** 运行确认；已有原型及原生目标均有事实，预测确实无法消费相应分支。

**位置：** [src/markov/templates/library.cpp:802](../../../src/markov/templates/library.cpp#L802)；关联 [src/markov/templates/asset.cpp:543](../../../src/markov/templates/asset.cpp#L543)、[src/collection/markov_collection/adapters/runtimes/ascend/matrices.py:103](../../../src/collection/markov_collection/adapters/runtimes/ascend/matrices.py#L103)。

**触发与影响：** 冻结案例qwen3-tp1-pp3/full-micro/boundaries：八成员Prefill合计1044 token、microbatch_requests=8。 此矩阵的SDK条件在matrix_rows>=1024时选择另一算法。PP模型调用的selection2只包含False分支；同一库的selection23已经包含True原型，但因具体stream绑定不同而被分在另一个选择池。 合法配置的条件展开被拒绝，六组合中一例失败。已有来源和算法元数据仍不能统一供PP模型调用消费；不能将其解释为必须补采新shape或用户填写模板。

**证据：** 同一库 definition28/operation0 的 selection2 使用 context [0,3,4]，仅有 False；selection23 的 context [0,1,3] 已有 True 原型。False 模式相同，具体 stream 绑定不同；八 rank 原生 full-micro 可运行而预测拒绝。 具体案例及原始位置见本节后的逐例表和审查记录中同名问题。

**建议修复：** 整轮报告后优先统一获取原生实际执行上下文，使必要辅助原型在对应调用上下文取得，和R7共用准备机制。保留C++对未证实资源迁移的隔离；不按PP标签加fallback，不按目标失败逐shape补采，也不把未知外部事件映射到任意资源。

**必要复核：** 保留旧库中“原型存在但不在可用上下文”的拒绝事实；统一准备机制后，以新的具名来源核对实际模型stream、依赖、端口及独立几何/组合，不按失败目标逐项补采或覆盖旧失败。

### R7 · Must Fix · PP的workspace预留没有覆盖实际模型stream，目标基线又改变了验证初态

**确认程度：** 运行确认；来源与目标在固定预热后的实际初态不同。

**位置：** [src/collection/markov_collection/adapters/runtimes/ascend/workspaces.py:44](../../../src/collection/markov_collection/adapters/runtimes/ascend/workspaces.py#L44)；关联 [src/collection/markov_collection/adapters/frameworks/sglang/hooks.py:512](../../../src/collection/markov_collection/adapters/frameworks/sglang/hooks.py#L512)、[src/collection/markov_collection/adapters/frameworks/sglang/session.py:211](../../../src/collection/markov_collection/adapters/frameworks/sglang/session.py#L211)、[src/markov/adapters/frameworks/sglang/structure.cpp:30](../../../src/markov/adapters/frameworks/sglang/structure.cpp#L30)。

**触发与影响：** 冻结PP3 wide/boundaries；模型使用forward stream，flush预留使用另一current stream。 预留只保证调用当时的一条stream达到379584512字节。原生目标的另一条workspace在固定warmup后为190840832，随后目标相关baseline将其扩大到197132288，capture才从扩容后的状态开始。既有代表来源的warmup/baseline/source均保持190840832；同一“统一预热”说明实际对应了不同模型stream容量。 当前目标采集窗口隐去了固定预热之后发生的目标相关扩容；来源和目标的初态不一致。逐flow结构匹配不能证明固定预热起点的完整执行结构；尚未据此计算时延误差，也未把PP窗口的两项额外清理操作归因于workspace。

**证据：** PP3 三 rank、PP4 四 rank及 Mistral PP4 四 rank的目标在 baseline 各扩容 6 MiB，来源未发生。reserve 只作用于 flush 时的 current_stream，模型实际在 forward_stream_ctx 执行；snapshot 保留原始三边界。 具体案例及原始位置见本节后的逐例表和审查记录中同名问题。

**建议修复：** 整轮报告后从原生实际执行上下文统一安排workspace预留及辅助原型，保证公开的统一预热起点适用于模型真正使用的stream；区分固定预热和目标相关baseline，不用kind字符串或目标缓存状态替代这个合同。不要按PP/rank写stream编号表或新增一套状态对账器。

**必要复核：** 先用现有事实验证初态解释，再在修复后的源与目标采集中核对实际模型stream在固定warmup后已满足同一容量策略，baseline/source不再因同类目标改变起点；保留本轮原始不一致。

### R8 · Must Fix · 带bias的addmm没有实现选择合同，形状变换后静默沿用错误设备模式

**确认程度：** 运行确认；目标的实际设备实现与预测不同。

**位置：** [src/collection/markov_collection/adapters/runtimes/ascend/observation.py:40](../../../src/collection/markov_collection/adapters/runtimes/ascend/observation.py#L40)；关联 [src/collection/markov_collection/adapters/runtimes/ascend/matrices.py:110](../../../src/collection/markov_collection/adapters/runtimes/ascend/matrices.py#L110)、[src/collection/markov_collection/adapters/frameworks/sglang/primitives.py:246](../../../src/collection/markov_collection/adapters/frameworks/sglang/primitives.py#L246)、[src/collection/adapters/runtimes/ascend/matmul_selection.cpp:42](../../../src/collection/adapters/runtimes/ascend/matmul_selection.cpp#L42)。

**触发与影响：** 冻结qwen15-moe-ep1/wide/varying，Prefill八成员/1044 token，24层QKV投影为aten.addmm.default。 Ascend观察器只为mm/matmul建立dense_matrix及execution_guards，addmm直接走普通形状传播。辅助探针只执行torch.matmul，SDK查询不带bias。addmm在源和目标均没有matrix_selection/guards，于是目标M改变后仍沿用源V2设备模式。 请求完成、公开几何和逻辑操作都可以一致，但24个设备节点错误；不是缺成本、路由注入或单纯控制顺序差异。性能层不能通过调整时延修复错误执行模式。

**证据：** Qwen1.5 EP1 六 flow 中首 Prefill 缺 24 个 aclnnAddmm_MatMulV3Common_MatMulV3、多 24 个 aclnnAddmm_MatMulCommon_MatMulV2。公开 4768 个操作/端口/顺序相同，路由也相同；addmm 未带 matrix_selection 或实现 guard。 具体案例及原始位置见本节后的逐例表和审查记录中同名问题。

**建议修复：** 整轮报告后按实际原生矩阵调用族建立完整描述，保留mm/addmm、bias、布局及固定标量所需事实，用已验证的对应SDK选择依据和同类原生调用取得最小原型。复用现有矩阵机制并与R3/R7的上下文修复对齐，不按Qwen或1044长度加分支，不把无bias原型直接改名当addmm，也不把未知实现变化当形状可变即结构固定。

**必要复核：** 先保留并重现本轮差异，再用同一API族的独立几何/模型场景检查所选模式与真实内核；同时覆盖原mm/matmul与bias调用，不以只补这一Prefill为结束条件。

### R6 · Must Fix · 专家路由适配器把可缺失的模块字段当成所有模型必有的有效元数据

**确认程度：** 初始化失败已运行确认；后续同类字段读取问题经源码确认。

**位置：** [src/collection/markov_collection/adapters/frameworks/sglang/native_routes.py:43](../../../src/collection/markov_collection/adapters/frameworks/sglang/native_routes.py#L43)；关联 [src/collection/markov_collection/adapters/frameworks/sglang/native_routes.py:77](../../../src/collection/markov_collection/adapters/frameworks/sglang/native_routes.py#L77)、[src/collection/markov_collection/adapters/frameworks/sglang/experts.py:52](../../../src/collection/markov_collection/adapters/frameworks/sglang/experts.py#L52)、[src/collection/markov_collection/adapters/frameworks/sglang/experts.py:57](../../../src/collection/markov_collection/adapters/frameworks/sglang/experts.py#L57)、[src/collection/markov_collection/adapters/frameworks/sglang/experts.py:155](../../../src/collection/markov_collection/adapters/frameworks/sglang/experts.py#L155)。

**触发与影响：** 冻结新模型OLMoE-1B-7B-0924，EP2×PP3；官方配置E=64/top-k=8，代表路由为八个合法专家ID。 NativeExpertRoutes.configure从首个FusedMoE.top_k取得top-k。但OLMoE把8传给独立TopK模块，创建FusedMoE时不传可选top_k，因此该字段为None。合法八专家路由在初始化时被错误拒绝。相同假设还出现在语义绑定、属性及combine几何中；注入代码另假定TopK.layer_id必有值，而OLMoE该字段也未设置。 模型尚未完成初始化就被Markov适配器拒绝，不能取得来源库或执行独立目标对照。只修初始化校验会留下注入layer及后续模板绑定缺口；不能将其归类为原生框架不支持OLMoE。

**证据：** OLMoE 官方配置是 64 专家、Top-K8，路由合法；FusedMoE.top_k 默认为 None，而真实 TopK 配置为 8。OLMoE 的 TopK.layer_id 也可缺失，expert 模块已有 layer_id；相关五处读取不能只修首个报错。 具体案例及原始位置见本节后的逐例表和审查记录中同名问题。

**建议修复：** 整轮报告后统一专家调用边界的元数据来源：实际expert模块提供layer/专家部署，实际路由结果提供top-k几何。路由注入与语义观察共享此合同，注入先于观测消费并覆盖未观察worker；初始化校验使用明确模型配置或实际路由模块，不堆逐模型fallback。同步收紧全部五处读取，而非只绕过最先报错的校验。

**必要复核：** 保留当前失败，修复后核对OLMoE及原有Qwen MoE的路由注入、source_routing、expert_top_k、combine形状和原生split/专家计数；两侧均用真实调用事实，不改路由去迎合None。

### R9 · Must Fix · 原生PD采集的矩阵范围沿用了轻量控制器的单请求假设

**确认程度：** 运行确认；采集到的实际行数超出事先声明的查询范围。

**位置：** [src/collection/markov_collection/adapters/frameworks/sglang/acquisition.py:159](../../../src/collection/markov_collection/adapters/frameworks/sglang/acquisition.py#L159)；关联 [src/collection/markov_collection/adapters/frameworks/sglang/acquisition.py:208](../../../src/collection/markov_collection/adapters/frameworks/sglang/acquisition.py#L208)、[example/automatic_graph/validation.py:165](../../../example/automatic_graph/validation.py#L165)、[src/collection/markov_collection/adapters/runtimes/ascend/matrices.py:171](../../../src/collection/markov_collection/adapters/runtimes/ascend/matrices.py#L171)。

**触发与影响：** 冻结native/qwen3-page64-pd1-1/wide/boundaries：原生Prefill允许多请求组批，实际aten.matmul的matrix_rows=591。 row_limits对PD不使用原生Prefill token预算，而按仿真P一次一个prompt取得M上界；NativeValidation复用source request并改为目标原生配置/请求，仍保留M=193的矩阵查询范围。离线声明对实际M=591的原生调用写入expected=true的matrix_rows<=193条件，SourceGraph合法地拒绝该自相矛盾输入。 原生两服务实际完成，默认/明细与全局执行证据通过，但Prefill观察图有144个合同gap，不能完成独立比较。不能把这一工具/采集计划边界问题归为page64原生不支持或当前目标预测结构已错。

**证据：** page64 P/D capture.json 的 M 上界为 193，原生 Prefill 配置允许 64 成员及 4096 token。rank0 operation1589 实际 left[591,4096]，却写入 matrix_rows<=193 的真条件，观察图被拒绝。 具体案例及原始位置见本节后的逐例表和审查记录中同名问题。

**建议修复：** 统一按实际原生采集配置和请求范围推导query/原型的有限域，区分原生允许的组批和轻量控制策略，不在采集范围计算中使用P一次一请求的假设。source和validation共用这一计算，覆盖整请求软预算越界与实际padding；不增PD目标标签分支、不改既定workload、不直接放宽SourceGraph合同检查。

**必要复核：** 保留本轮失败；后续以原生允许多请求的PD及普通/PP对照核对所有实际矩阵行均在预先查询范围内，观察图可解释，模拟器仍维护自己的组批规则；不以强制原生串行来隐藏缺口。

### R4 · Should Fix · 环境准备把历史预测错误误判为全部输入校验失败

**确认程度：** 静态确认；本轮未另行构造“所有预测失败”的单组合任务运行复现。

**位置：** [src/collection/markov_collection/workflow/exploration.py:195](../../../src/collection/markov_collection/workflow/exploration.py#L195)；关联 [example/automatic_graph/validation.py:155](../../../example/automatic_graph/validation.py#L155)、[src/collection/markov_collection/workflow/exploration.py:464](../../../src/collection/markov_collection/workflow/exploration.py#L464)。

**触发与影响：** 有效单配置×单workload任务已发布模板，但预测失败；随后按公开validate入口采原生目标。例如只提交本轮已确认失败的PP3 full-micro/boundaries组合。 NativeValidation在任务原有failures仍在时调用task.prepare。prepare将check_workloads的新结果合入历史failures，再以总错误数等于组合数判断无需准备SDK；这个总数也包含C++预测错误，输入合法时同样会提前返回。 正式任务结束已删除work/observer-build，后续原生验证因此缺少必要观察库和selector，可能启动失败或丢失所需观察；破坏“预测失败仍采预选原生事实”的验证边界。

**证据：** 合法输入、资产已发布、所有预测失败时，prepare 将历史预测错误与本次输入校验合并，再按错误总数提前返回；正式任务已删除 observer-build，预选原生验证可能没有观察库。此完整触发组合未另跑，跨模块逻辑静态确认。 具体案例及原始位置见本节后的逐例表和审查记录中同名问题。

**建议修复：** 仅以本次check_workloads得到的输入校验结果决定是否准备，历史预测错误继续保留用于报告；不要清空历史或新增一套持久状态账本。

**必要复核：** 报告后利用真实失败任务或等价正式单组合输入，核对无历史丢失且SDK准备完成，预测失败仍能执行原先安排的原生验证。

### R5 · Should Fix · 子进程摘要只保留末行，掩盖真实失败原因

**确认程度：** 运行确认；冻结矩阵的多类错误均有完整日志。

**位置：** [src/collection/markov_collection/workflow/exploration.py:136](../../../src/collection/markov_collection/workflow/exploration.py#L136)；关联 [example/automatic_graph/investigation.py:208](../../../example/automatic_graph/investigation.py#L208)、[example/automatic_graph/investigation.py:43](../../../example/automatic_graph/investigation.py#L43)。

**触发与影响：** 多行失败日志以空行或次生诊断结束；本轮设备预检及Phi3预热失败已触发。 Exploration.step只读取run.log最后一行。设备占用输出的空末行变成空CommandFailed信息；Phi3原生QKV断言则被随后CUDA coredump提示替代。 精简结果不能准确区分资源预检受阻和原生kernel不兼容，需人工回看完整日志；原始事实与费用仍完整，未将失败写成成功。

**证据：** 16 个来源设备预检摘要变空；Phi3 断言被后续 coredump 提示掩盖。页资产负例实际以 markov: no region layout matches 开头拒绝，末行只剩 layout60 条件，负例判据丢失前缀后误记 error。 具体案例及原始位置见本节后的逐例表和审查记录中同名问题。

**建议修复：** 整轮报告后在现有step错误交接中保留有界的非空日志上下文及完整日志位置；返回码和费用不变。不按设备/kernel具体报错新增业务分支。

**必要复核：** 用已经保留的真实空行日志检查精简错误含失败上下文，且正式流程的错误返回码、费用和失败后继续行为不变；不为此新增原生采集。

## 6. 原生限制、外部受阻及尚不能判定的问题

### 6.1 Phi3：固定原生融合内核限制

Phi3-mini 官方 hidden_size=3072、32 attention heads，head dimension=96。固定 sgl_kernel_npu 的 split_qkv_rmsnorm_rope.py:394 要求 next_power_of_2(head_dim)==head_dim，96 对应 128，实际预热抛出 AssertionError；本项目在没有观察 scope 时直接调用原生函数，来源尚未产出。

固定 Llama/Phi3 原生调用方在 extend 使用 forward_prepare_native，Decode 在满足 rotary 接口条件时走 forward_prepare_npu。存在分拆函数并不证明替代 Decode 路径已可运行；本轮没有改源码、换维度或无插桩重跑。应在恢复后评估通用原生能力路径或已有开关，保持官方完整结构；不能直接归为本项目模板问题，也不能写成所有 SGLang 后端都不支持 Phi3。

### 6.2 设备预检受阻：本轮没有获得对应覆盖

16 个来源任务和 TP1 small/boundaries 目标在启动 Engine 前发现所选设备占用或占用情况不能确认。原始 npu-smi 输出保留了设备 0 的进程及内存事实；相关 PID 不在本次任务容器中，不能确认具体所有者，不杀进程、不争用设备。来源受阻任务 native_attempts=0；后续资源恢复也没有自动重试和覆盖原失败。

这些来源牵连 18 个原生目标和 8 个条件案例，阻断关系见逐例表。EP8、FP16、多种页布局、非对称 PD 和组合拓扑等重要方向仍需后续独立证据；不能把资源拒绝写成框架不支持、算法错误或配置通过。

### 6.3 未定性及未充分覆盖项

- 异步尾部：控制端口完成、用户输出、状态提交与设备清理不一定同刻；观察到尾部不自动等于提前释放。R2 的检查器确有缺陷，最终窗口边界和结果描述仍须对齐共同合同。
- PP/P/D 编排：同一语义工作可配对，完整窗口的操作及资源状态仍可能不同。不能强迫原生复制轻量控制顺序，也不能先忽略差异再宣称正确；应分别核对状态不变量及必要因果。
- 接口范围：rank-local 匹配覆盖可观察工作、公开几何和指定 API/端口，opaque 内部几何、未观察输入内容、跨 rank 协议进度及全部驱动事件没有因此全部获证。同步 opaque 接口与内部 SDMA 包逐一归属是不同问题。
- 内容依赖：dummy 模型和给定路由只定义条件执行；没有推导真实文本下的自然专家选择、采样内容或真实权重分支。
- KV/资源依赖：整 storage 危害边界有保守性，CPU 访问端口存在已声明 shortcut；静态审查未把这些限制伪报为本轮已确认的内存漏洞。
- 错误路径：现有生命周期覆盖成功，但不是对驱动崩溃、所有并发竞争或任意第三方后台线程的穷尽证明。R4 只作静态确认；R1 被 R2 遮挡，不能补写运行通过。

## 7. 全流程实现与维护边界审查

审查从正式输入/部署经 Python 原生观察、跨语言交接、C++ 图生产和归一化、目标变换/组合、控制及执行，追到 CLI、示例验证、构建和容器；沿相关生产者和消费者检查未修改文件，不只检查 Git diff。冻结前开发审查与本轮只读诊断分开，本文问题未在调查中修复。

| 链路 | 已核对内容 | 审查判断与限制 |
| --- | --- | --- |
| 输入、来源规划、workflow | 来源域、边界校验、发布/恢复、预算、日志与错误继续 | R4/R5/R9 说明环境准备、历史错误和原生范围的职责仍需收紧 |
| 公共插桩与框架/runtime | Scope 的 ContextVar 父级恢复、模块/接口 wrapper、原生生命周期、专家条件注入 | 共同 wrapper 不导入具体框架；R6 的元数据归属与 R8 的调用族描述仍不完整 |
| 原生桥与事件归属 | 对应 Torch/CANN ABI、Tensor/动态库 RAII、append 记录、ctypes 前保留事件身份、时钟/线程归属 | 未发现新的已确认桥资源泄漏；不声明上游驱动及全部线程安全已获证 |
| 来源投影/模板 | 公共接口外部读写和主机读回、共享定义/布局/绑定、参数作用域及集合归约 | 没有为每个请求长度生成类；R3/R7/R8 表明正确的局部设备实现仍是关键边界 |
| 组合与通信 | 组/调用身份、P2P 配对、all-to-all split、PP stage 端口、PD KV/layout 交接 | 原生角色解释在 adapter；部分组合因来源受阻而没有本轮独立进度证据 |
| 控制与 KV | 请求进度、单池唯一页/槽位所有权、lane 预留、P/D 两池及交接后释放 | 没有为模板复制请求状态机；不同控制顺序的快照差异尚未等价于所有权缺陷 |
| 执行与回收 | 就绪后资源服务、同刻反馈、任务身份、在途图/存储前沿、借用对象生命周期 | rank 构造器由 unique_ptr 保持地址，catalog 生命周期覆盖借用；异步尾部及工具判据待 R1/R2 收口 |
| 验证与产物 | 独立目标不建库、默认/明细一致、只读预测挂载、输出保护、错误/费用保留 | 编排在 example，核心没有窗口案例标签；R1/R2/R5/R9 导致部分证据无法判定，不能靠调断言改成绿灯 |

代码层次整体仍对应 workflow → adapters → core，控制/执行算法未按模型、kernel 名或特殊 rank 添加案例分支。本轮没有发现需要以新框架/新控制器体系替代整个流程的证据；优先修正边界上的共同事实，而不是扩张 wrapper、目标黑名单或第二套状态账本。

工程表达核对包括 C++23、头源同目录、独立逻辑空行及注释间距、RAII/借用期、错误路径与删除机会。冻结前必要 Release 构建、原生桥构建、Ruff、Python 3.10 语法/注释间距、C++ 格式及空白检查已有记录；本轮冻结后没有改代码，因此没有为报告重新构建或增加 test 工程，.clang-format 保持不变。静态检查通过不代替上述内容审阅及真实结构证据。

冻结时 src 的 C++/Python 为 125 文件、20,577 行，相对 S7 净增 391 行；另有 18 行安装 Shell，合计 126 文件、20,595 行。example 的 Python/Shell 为 13 个文件、3,487 行，净增 1,819 行。编排增长主要在验证侧；恢复修复时应合并范围推导、共享元数据及重复错误处理，避免把本轮新判据搬进核心。统计为上述语言的物理行数，包含空行与注释，不含构建产物和第三方代码。

## 8. 完整逐例结果与证据索引

所有路径基于本轮 data 目录；每个表格行保留原始状态。blocked 不表示已运行，error 不表示根因必在核心。详细输入及完整图不复制到本文。

### 8.1 来源任务：36 项，内部计划 208 个配置×请求组合

| 来源任务 | 原状态 | 条件预测结果 | 说明 | 证据 |
| --- | --- | --- | --- | --- |
| qwen3-tp1 | complete | 30/30，预测失败 0 | 已发布来源；条件预测完成，非独立验收 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-tp1/results/summary.md) |
| qwen3-tp2 | complete | 12/12，预测失败 0 | 已发布来源；条件预测完成，非独立验收 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-tp2/results/summary.md) |
| qwen3-tp4 | complete | 4/4，预测失败 0 | 已发布来源；条件预测完成，非独立验收 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-tp4/results/summary.md) |
| qwen3-tp8 | complete | 4/4，预测失败 0 | 已发布来源；条件预测完成，非独立验收 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-tp8/results/summary.md) |
| qwen3-tp1-pp3 | partial | 5/6，预测失败 1 | 已发布来源；full-micro 因 R3 失败 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-tp1-pp3/results/summary.md) |
| qwen3-tp1-pp4 | partial | 5/6，预测失败 1 | 已发布来源；full-micro 因 R3 失败 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-tp1-pp4/results/summary.md) |
| qwen3-tp2-pp3 | partial | 5/6，预测失败 1 | 已发布来源；full-micro 因 R3 失败 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-tp2-pp3/results/summary.md) |
| qwen3-tp2-pp4 | partial | 5/6，预测失败 1 | 已发布来源；full-micro 因 R3 失败 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-tp2-pp4/results/summary.md) |
| mistral-tp1-pp1 | complete | 4/4，预测失败 0 | 已发布来源；条件预测完成，非独立验收 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/mistral-tp1-pp1/results/summary.md) |
| mistral-tp4-pp1 | complete | 4/4，预测失败 0 | 已发布来源；条件预测完成，非独立验收 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/mistral-tp4-pp1/results/summary.md) |
| mistral-tp1-pp3 | partial | 5/6，预测失败 1 | 已发布来源；full-micro 因 R3 失败 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/mistral-tp1-pp3/results/summary.md) |
| mistral-tp1-pp4 | partial | 5/6，预测失败 1 | 已发布来源；full-micro 因 R3 失败 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/mistral-tp1-pp4/results/summary.md) |
| qwen15-moe-ep1 | complete | 4/4，预测失败 0 | 已发布来源；条件预测完成，非独立验收 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen15-moe-ep1/results/summary.md) |
| qwen15-moe-ep2 | complete | 4/4，预测失败 0 | 已发布来源；条件预测完成，非独立验收 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen15-moe-ep2/results/summary.md) |
| qwen15-moe-ep4 | complete | 4/4，预测失败 0 | 已发布来源；条件预测完成，非独立验收 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen15-moe-ep4/results/summary.md) |
| qwen3-moe-ep2 | complete | 4/4，预测失败 0 | 已发布来源；条件预测完成，非独立验收 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-moe-ep2/results/summary.md) |
| qwen3-moe-ep4 | complete | 4/4，预测失败 0 | 已发布来源；条件预测完成，非独立验收 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-moe-ep4/results/summary.md) |
| qwen3-moe-ep8 | error | 0/4，未执行 | 设备预检受阻；未启动 Engine | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-moe-ep8/logs/run.log) |
| qwen3-moe-ep2-pp3 | error | 0/6，未执行 | 设备预检受阻；未启动 Engine | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-moe-ep2-pp3/logs/run.log) |
| qwen3-moe-pd2-2 | error | 0/4，未执行 | 设备预检受阻；未启动 Engine | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-moe-pd2-2/logs/run.log) |
| qwen3-pd4-2 | error | 0/4，未执行 | 设备预检受阻；未启动 Engine | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-pd4-2/logs/run.log) |
| qwen3-pd2-4 | error | 0/4，未执行 | 设备预检受阻；未启动 Engine | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-pd2-4/logs/run.log) |
| qwen3-moe-pd-pp3 | error | 0/4，未执行 | 设备预检受阻；未启动 Engine | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-moe-pd-pp3/logs/run.log) |
| phi3-tp1 | error | 0/6，未执行 | 原生 head_dim=96 融合内核断言 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/phi3-tp1/logs/run.log) |
| phi3-tp2 | error | 0/6，未执行 | 设备预检受阻；未启动 Engine | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/phi3-tp2/logs/run.log) |
| olmoe-ep2 | error | 0/4，未执行 | 设备预检受阻；未启动 Engine | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/olmoe-ep2/logs/run.log) |
| olmoe-ep4 | error | 0/4，未执行 | 设备预检受阻；未启动 Engine | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/olmoe-ep4/logs/run.log) |
| olmoe-ep2-pp3 | error | 0/6，未执行 | OLMoE 元数据适配错误 R6 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/olmoe-ep2-pp3/logs/run.log) |
| qwen3-fp16 | error | 0/4，未执行 | 设备预检受阻；未启动 Engine | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-fp16/logs/run.log) |
| qwen3-page1 | error | 0/6，未执行 | 设备预检受阻；未启动 Engine | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-page1/logs/run.log) |
| qwen3-page16 | error | 0/6，未执行 | 设备预检受阻；未启动 Engine | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-page16/logs/run.log) |
| qwen3-page64 | error | 0/6，未执行 | 设备预检受阻；未启动 Engine | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-page64/logs/run.log) |
| qwen3-page256 | error | 0/6，未执行 | 设备预检受阻；未启动 Engine | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-page256/logs/run.log) |
| qwen3-page64-pp3 | error | 0/6，未执行 | 设备预检受阻；未启动 Engine | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-page64-pp3/logs/run.log) |
| qwen3-moe-page64-ep2 | error | 0/4，未执行 | 设备预检受阻；未启动 Engine | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-moe-page64-ep2/logs/run.log) |
| qwen3-page64-pd1-1 | complete | 4/4，预测失败 0 | 已发布来源；条件预测完成，非独立验收 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-page64-pd1-1/results/summary.md) |

### 8.2 预选原生目标：41 项

rank 数值只描述本轮局部比较报告。每例原生是否实际运行单列，避免将依赖受阻算成实测。

| 目标案例（域/配置/请求） | 原状态 | 原生完成 | 局部 rank 比较及主要原因 | 证据 |
| --- | --- | --- | --- | --- |
| qwen3-tp1/small/boundaries | error | 否：设备预检受阻 | 没有逐 rank 对照；R2 阻碍完整执行检查 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-tp1/results/validation/small/boundaries/validation-small-boundaries.json) |
| qwen3-tp1/r32/members-32 | error | 是 | matched×1；R2 阻碍完整执行检查 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-tp1/results/validation/r32/members-32/validation-r32-members-32.json) |
| qwen3-tp1/r33/members-33 | error | 是 | matched×1；R2 阻碍完整执行检查 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-tp1/results/validation/r33/members-33/validation-r33-members-33.json) |
| qwen3-tp1/r128/members-128 | error | 是 | matched×1；R2 阻碍完整执行检查 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-tp1/results/validation/r128/members-128/validation-r128-members-128.json) |
| qwen3-tp1/wide/long-outputs | error | 是 | matched×1；R2 阻碍完整执行检查 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-tp1/results/validation/wide/long-outputs/validation-wide-long-outputs.json) |
| qwen3-tp2/wide/boundaries | error | 是 | matched×2；R2 阻碍完整执行检查 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-tp2/results/validation/wide/boundaries/validation-wide-boundaries.json) |
| qwen3-tp2/kv262144/context-boundaries | error | 是 | matched×2；R2 阻碍完整执行检查 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-tp2/results/validation/kv262144/context-boundaries/validation-kv262144-context-boundaries.json) |
| qwen3-tp4/wide/boundaries | error | 是 | matched×4；R2 阻碍完整执行检查 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-tp4/results/validation/wide/boundaries/validation-wide-boundaries.json) |
| qwen3-tp8/wide/boundaries | error | 是 | matched×8；R2 阻碍完整执行检查 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-tp8/results/validation/wide/boundaries/validation-wide-boundaries.json) |
| qwen3-tp1-pp3/wide/boundaries | error | 是 | differences×3；R2 阻碍完整执行检查 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-tp1-pp3/results/validation/wide/boundaries/validation-wide-boundaries.json) |
| qwen3-tp1-pp4/wide/boundaries | error | 是 | differences×4；R2 阻碍完整执行检查 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-tp1-pp4/results/validation/wide/boundaries/validation-wide-boundaries.json) |
| qwen3-tp2-pp3/wide/boundaries | error | 是 | differences×6；R2 阻碍完整执行检查 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-tp2-pp3/results/validation/wide/boundaries/validation-wide-boundaries.json) |
| qwen3-tp2-pp4/full-micro/boundaries | error | 是 | 没有逐 rank 对照；预测先遇 R3 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-tp2-pp4/results/validation/full-micro/boundaries/validation-full-micro-boundaries.json) |
| mistral-tp1-pp1/wide/boundaries | error | 是 | matched×1；R2 阻碍完整执行检查 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/mistral-tp1-pp1/results/validation/wide/boundaries/validation-wide-boundaries.json) |
| mistral-tp4-pp1/wide/boundaries | error | 是 | matched×4；R2 阻碍完整执行检查 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/mistral-tp4-pp1/results/validation/wide/boundaries/validation-wide-boundaries.json) |
| mistral-tp1-pp3/small/boundaries | error | 是 | differences×3；R2 阻碍完整执行检查 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/mistral-tp1-pp3/results/validation/small/boundaries/validation-small-boundaries.json) |
| mistral-tp1-pp4/wide/boundaries | error | 是 | differences×4；R2 阻碍完整执行检查 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/mistral-tp1-pp4/results/validation/wide/boundaries/validation-wide-boundaries.json) |
| qwen15-moe-ep1/wide/varying | error | 是 | differences×1；R2 阻碍完整执行检查；R8 设备模式不同 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen15-moe-ep1/results/validation/wide/varying/validation-wide-varying.json) |
| qwen15-moe-ep2/wide/varying | error | 是 | matched×2；R2 阻碍完整执行检查 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen15-moe-ep2/results/validation/wide/varying/validation-wide-varying.json) |
| qwen15-moe-ep4/wide/varying | error | 是 | matched×4；R2 阻碍完整执行检查 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen15-moe-ep4/results/validation/wide/varying/validation-wide-varying.json) |
| qwen3-moe-ep2/wide/varying | error | 是 | matched×2；R2 阻碍完整执行检查 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-moe-ep2/results/validation/wide/varying/validation-wide-varying.json) |
| qwen3-moe-ep4/wide/varying | error | 是 | matched×4；R2 阻碍完整执行检查 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-moe-ep4/results/validation/wide/varying/validation-wide-varying.json) |
| qwen3-moe-ep8/wide/varying | blocked | 未运行 | 来源未发布；依赖 workflow/qwen3-moe-ep8 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/results/outcomes.json) |
| qwen3-moe-ep2-pp3/wide/varying | blocked | 未运行 | 来源未发布；依赖 workflow/qwen3-moe-ep2-pp3 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/results/outcomes.json) |
| qwen3-moe-pd2-2/wide/varying | blocked | 未运行 | 来源未发布；依赖 workflow/qwen3-moe-pd2-2 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/results/outcomes.json) |
| qwen3-pd4-2/wide/boundaries | blocked | 未运行 | 来源未发布；依赖 workflow/qwen3-pd4-2 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/results/outcomes.json) |
| qwen3-pd2-4/wide/boundaries | blocked | 未运行 | 来源未发布；依赖 workflow/qwen3-pd2-4 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/results/outcomes.json) |
| qwen3-moe-pd-pp3/wide/varying | blocked | 未运行 | 来源未发布；依赖 workflow/qwen3-moe-pd-pp3 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/results/outcomes.json) |
| phi3-tp1/wide/boundaries | blocked | 未运行 | 来源未发布；依赖 workflow/phi3-tp1 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/results/outcomes.json) |
| phi3-tp2/wide/boundaries | blocked | 未运行 | 来源未发布；依赖 workflow/phi3-tp2 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/results/outcomes.json) |
| olmoe-ep2/wide/varying | blocked | 未运行 | 来源未发布；依赖 workflow/olmoe-ep2 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/results/outcomes.json) |
| olmoe-ep4/wide/varying | blocked | 未运行 | 来源未发布；依赖 workflow/olmoe-ep4 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/results/outcomes.json) |
| olmoe-ep2-pp3/wide/varying | blocked | 未运行 | 来源未发布；依赖 workflow/olmoe-ep2-pp3 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/results/outcomes.json) |
| qwen3-fp16/wide/boundaries | blocked | 未运行 | 来源未发布；依赖 workflow/qwen3-fp16 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/results/outcomes.json) |
| qwen3-page1/small/unit-capacity | blocked | 未运行 | 来源未发布；依赖 workflow/qwen3-page1 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/results/outcomes.json) |
| qwen3-page16/small/unit-capacity | blocked | 未运行 | 来源未发布；依赖 workflow/qwen3-page16 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/results/outcomes.json) |
| qwen3-page64/small/unit-capacity | blocked | 未运行 | 来源未发布；依赖 workflow/qwen3-page64 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/results/outcomes.json) |
| qwen3-page256/small/unit-capacity | blocked | 未运行 | 来源未发布；依赖 workflow/qwen3-page256 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/results/outcomes.json) |
| qwen3-page64-pp3/wide/boundaries | blocked | 未运行 | 来源未发布；依赖 workflow/qwen3-page64-pp3 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/results/outcomes.json) |
| qwen3-moe-page64-ep2/wide/varying | blocked | 未运行 | 来源未发布；依赖 workflow/qwen3-moe-page64-ep2 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/results/outcomes.json) |
| qwen3-page64-pd1-1/wide/boundaries | error | 是 | error×1、differences×1；Prefill R9；时间线检查 matched | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-page64-pd1-1/results/validation/wide/boundaries/validation-wide-boundaries.json) |

### 8.3 边界和给定耗时：30 项

| 案例 | 预期 | 原状态 | 实际解释 | 证据 |
| --- | --- | --- | --- | --- |
| D-running-1 | complete | error | 后置执行检查器 R2；不据此判定核心结果通过或失败 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-tp1/logs/run.log) |
| D-running-2 | complete | error | 后置执行检查器 R2；不据此判定核心结果通过或失败 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-tp1/logs/run.log) |
| D-running-31 | complete | error | 后置执行检查器 R2；不据此判定核心结果通过或失败 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-tp1/logs/run.log) |
| D-running-64 | complete | error | 后置执行检查器 R2；不据此判定核心结果通过或失败 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-tp1/logs/run.log) |
| D-running-101 | complete | error | 后置执行检查器 R2；不据此判定核心结果通过或失败 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-tp1/logs/run.log) |
| G-seed-64 | complete | error | 后置执行检查器 R2；不据此判定核心结果通过或失败 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-tp1/logs/run.log) |
| G-seed-128 | complete | error | 后置执行检查器 R2；不据此判定核心结果通过或失败 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-tp1/logs/run.log) |
| G-seed-256 | complete | error | 后置执行检查器 R2；不据此判定核心结果通过或失败 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-tp1/logs/run.log) |
| K-qwen3-tp4-host | complete | complete | 219,776 节点、6 动作的时间线检查 matched | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/cases/K-qwen3-tp4-host/result.json) |
| K-qwen3-tp4-device | complete | error | 后置执行检查器 R2；不据此判定核心结果通过或失败 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-tp4/logs/run.log) |
| K-qwen3-tp4-communication | complete | error | 后置执行检查器 R2；不据此判定核心结果通过或失败 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-tp4/logs/run.log) |
| K-qwen3-tp4-transfer | complete | error | 后置执行检查器 R2；不据此判定核心结果通过或失败 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-tp4/logs/run.log) |
| K-qwen3-moe-pd-pp3-host | complete | blocked | 共享来源未发布；未执行 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/results/outcomes.json) |
| K-qwen3-moe-pd-pp3-device | complete | blocked | 共享来源未发布；未执行 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/results/outcomes.json) |
| K-qwen3-moe-pd-pp3-communication | complete | blocked | 共享来源未发布；未执行 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/results/outcomes.json) |
| K-qwen3-moe-pd-pp3-transfer | complete | blocked | 共享来源未发布；未执行 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/results/outcomes.json) |
| E-request-exceeds-pool | rejected | rejected | command failed (1): markov: request exceeds the native input length limit: too-large | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-tp1/logs/run.log) |
| A-zero-prompt | rejected | rejected | command failed (1): markov: request needs nonnegative arrival and positive token lengths: request-0 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-tp1/logs/run.log) |
| A-type | rejected | rejected | command failed (1): markov: serving fields must be integers | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-tp1/logs/run.log) |
| A-duplicate-request | rejected | rejected | command failed (1): markov: request IDs must be nonempty and unique | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-tp1/logs/run.log) |
| C-wrong-page-asset | rejected | error | 实际拒绝，但末行错误摘要使负例识别失败（R5） | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-tp1/logs/run.log) |
| A-wrong-topology-asset | rejected | rejected | command failed (1): markov: target PP differs from its source topology | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-tp1/logs/run.log) |
| C-equal-pages-1 | complete | blocked | 共享来源未发布；未执行 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/results/outcomes.json) |
| C-equal-pages-16 | complete | blocked | 共享来源未发布；未执行 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/results/outcomes.json) |
| C-equal-pages-64 | complete | blocked | 共享来源未发布；未执行 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/results/outcomes.json) |
| C-equal-pages-128 | complete | error | 后置执行检查器 R2；不据此判定核心结果通过或失败 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-tp1/logs/run.log) |
| C-equal-pages-256 | complete | blocked | 共享来源未发布；未执行 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/results/outcomes.json) |
| A-missing-routes | rejected | rejected | command failed (1): markov: [json.exception.out_of_range.403] key 'ep_routes' not found | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-moe-ep2/logs/run.log) |
| A-duplicate-expert | rejected | rejected | command failed (1): markov: one token cannot select the same expert twice | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-moe-ep2/logs/run.log) |
| A-route-gap | rejected | rejected | command failed (1): markov: incomplete route for request-0, layer 0 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/runs/qwen3-moe-ep2/logs/run.log) |

E-request-exceeds-pool 虽被拒绝，实际先命中 native input length 上限；不将它单独作为 KV 容量不足的路径证明。K 的 host/device/communication/transfer 是给定耗时条件，不是性能采样或精度实验。

### 8.4 生命周期：7 项

| 案例 | 原状态 | 已核对事实 | 证据 |
| --- | --- | --- | --- |
| L-cli-invalid | complete | 非法任务非零退出，保留错误 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/cases/L-cli-invalid/run.log) |
| L-help-after-error | complete | 前例失败后公开 help 仍正常 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/cases/L-help-after-error/run.log) |
| L-resume-completed | complete | 完成任务恢复后状态保持，不新增采集 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/cases/L-resume-completed/run.log) |
| L-budget-stop | complete | 零 processing 预算停止；pending 清空、native 0 次、容器释放 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/cases/L-budget-stop/run.log) |
| L-interrupt-resume | complete | SIGINT 后状态 interrupted、pending 清空；恢复 complete，native 0 次，processing 费用从 1.234s 增至 74.416s | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/cases/L-interrupt-resume/run.log) |
| L-bind-input | complete | 缺失 bind 输入在启动前拒绝，不创建伪输入/输出目录 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/results/outcomes.json) |
| L-protect-input | complete | 输出覆盖输入非零拒绝，输入字节保持 | [证据](../../../data/20261026T192126.898346Z_s8-independent-validation/results/outcomes.json) |

## 9. 模板规模、观察复用与资源消耗

### 9.1 已发布的 18 个来源库

下表为每个库内部的共享定义/布局/执行模式，不能将各域数量相加后称为全项目唯一模板数，也不能把角色绑定数当模板类数。模型、TP、dtype、backend 或页布局变化仍由各来源域解释。

| 来源域 | 成功来源会话 | 观察角色/绑定角色 | 定义 | 布局 | 执行模式 |
| --- | ---: | --- | ---: | ---: | ---: |
| mistral-tp1-pp1 | 1 | 1/1 | 108 | 174 | 60 |
| mistral-tp1-pp3 | 1 | 3/3 | 153 | 294 | 64 |
| mistral-tp1-pp4 | 1 | 3/4 | 153 | 294 | 64 |
| mistral-tp4-pp1 | 2 | 2/4 | 113 | 184 | 64 |
| qwen15-moe-ep1 | 1 | 1/1 | 118 | 178 | 61 |
| qwen15-moe-ep2 | 1 | 2/2 | 186 | 321 | 78 |
| qwen15-moe-ep4 | 1 | 2/4 | 186 | 322 | 78 |
| qwen3-moe-ep2 | 1 | 2/2 | 178 | 309 | 78 |
| qwen3-moe-ep4 | 1 | 2/4 | 178 | 310 | 79 |
| qwen3-page64-pd1-1 | 1 | 2/2 | 135 | 239 | 64 |
| qwen3-tp1 | 1 | 1/1 | 111 | 183 | 61 |
| qwen3-tp1-pp3 | 1 | 3/3 | 159 | 318 | 64 |
| qwen3-tp1-pp4 | 1 | 3/4 | 159 | 318 | 64 |
| qwen3-tp2 | 2 | 2/2 | 116 | 193 | 64 |
| qwen3-tp2-pp3 | 1 | 6/6 | 193 | 383 | 68 |
| qwen3-tp2-pp4 | 1 | 6/8 | 193 | 383 | 68 |
| qwen3-tp4 | 2 | 2/4 | 116 | 193 | 64 |
| qwen3-tp8 | 2 | 2/8 | 116 | 193 | 65 |

18 库合计 22 个成功来源会话、63 个逻辑角色绑定，实际观察 45 个角色。TP4/TP8 各只采 0/1 是减少隔离模型会话的实际事实；PP/EP 代表复用主要减少观察与导出，协议所需全部 peer 仍运行。没有进行逐角色无复用的完整墙钟基线，不能据 45/63 推出同等比例的耗时或 HBM 节省。

每库定义 108–193、布局 174–383、执行模式 60–79；角色数增长不按比例复制全部定义。数量中含 serving 状态、接口及 runtime 有限实现，不只 decoder layer。它们证明有限范围的共享，不证明归一化已覆盖所有内容/后端分支；R8 正说明缺少调用族事实会产生错误复用。

来源规模与费用的中途快照见 source-coverage.json；其中费用注明可能包含当时已发生的目标验证，不能冒充纯来源成本。最终整体费用按全部 state 的 charged_ns 求和，均已无 pending 预留。

### 9.2 最终资源账目

| 项目 | 实际 | 本轮上限 |
| --- | ---: | ---: |
| 原生尝试 | 46 次 | 92 次 |
| native 时间 | 8,854.583643640 秒 | 24,000 秒 |
| processing 时间 | 7,440.108056980 秒 | 28,800 秒 |
| 最终目录物理文件大小 | 约 33.313 GiB | 保留 96 GiB；临时 120 GiB |
| 同时活动任务 | 1 | 1 |

46 次中包括 22 个成功来源会话、2 个新模型失败尝试、22 个完成的原生目标；设备预检不算 Engine 尝试，但消耗的处理时间保留。native 包含初始化、固定 warmup、baseline/capture 及相关原生工作，不是纯有效观测时间；processing 含构建、SDK/官方解析、图解释、预测与比较，也不是性能数据采样。手工审阅和文档编辑未连续计时，不声称以上覆盖全部人工墙钟时间。

33.313 GiB 是调查结束的目录大小，不是连续测得的整轮物理峰值。任务自身记录峰值及过程中存储检查均保留，不将逐任务峰值相加当成整轮峰值。源码输入、资产、来源原始事实、目标观察、失败日志及必要明细仍保留；本次不为压小报告数据而删除失败证据。

原生数据仍较大，官方 profiler 解析也显著占用 processing；本轮没有通过删原始事实或改变采集策略来掩盖开销。后续优化应先区分机器交接、可选明细和人类报告，保持独立证据可复核。

结束后所有 state.pending 均为空，按本轮 task label 查询无残留容器；没有清理其他工作区、系统服务或不能确认归属的设备进程。

## 10. 恢复后的推荐顺序与结束条件

用户已要求本次报告后暂停；以下工作保留为恢复后的安排，本次不执行。

1. **先修验证和错误交接：R1/R2，连同 R5。** 正确解释 action/control、真实节点状态、发布/完成端口及异步尾部；保留错误的有界上下文。页资产拒绝应能被准确记录，不能靠修改业务行为配合末行断言。优先复用现有事实，不为工具修复启动模型。
2. **统一原生准备事实：R3/R7/R9。** 从原生实际模型上下文安排辅助原型及 workspace，按实际原生配置推导有限矩阵域。保持 C++ 对未经证明的 context 迁移隔离，不直接删检查或强迫原生串行；来源与目标共享准备方法，控制器仍维护自己的策略。
3. **补全已有矩阵调用族：R8。** mm/matmul 与带 bias 的 addmm 保留实际 API、几何、布局及必要固定参数，依据对应原生选择逻辑得到有限实现原型；验证 SDK 的 bias 语义，不能未经核对直接改第三 Tensor 参数或重命名无 bias 内核。
4. **修正专家事实归属：R6。** 真实 expert 模块提供层及部署，真实路由结果提供 top-k 几何，注入在语义观察消费前完成且覆盖未观察 peer；使用共同调用边界，不写 OLMoE/Qwen 分支。同步处理 R4 的准备资格判断，历史预测错误不等于输入错误。
5. **分别处理原生资格及未完成覆盖。** 保留 Phi3 和 OLMoE 的首次失败；评估 Phi3 通用原生路径的可行性。设备可用后用新的具名记录补全本轮未运行的页布局、dtype、EP8、非对称/联合 PD 等必选方向，保留首次阻断及费用。不得静默换模型、降低覆盖或把修改后的重跑覆盖本轮独立结果。
6. **做有界回归与必要独立证据，再交付 S8。** 按共同机制评估影响域，选未参与修正规则的请求/几何验证新行为；分别核对模板工作、各自控制合法性、必要因果和资源生命周期。两个新模型仍需完整来源到独立目标；关键差异有依据解释或修正后，完成相关代码审查、文档/数据整理及分层提交推送，方可进入 S9。

恢复时优先减少重复职责链，新增抽象须有实际消费者；不为每个失败案例建 adapter、特殊阈值、拒绝表或额外采集循环。各次修改和回归保留新记录，本轮结果维持冻结历史。

本次仅交付完整调查报告与状态对齐。性能模型、准确率及 S9–S12 没有启动；S8 的必修缺陷、两个新模型及受阻组合资格仍未关闭。
