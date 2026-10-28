# 并行执行结构窗口状态

更新日期：2026-10-28。**Goal运行中；S8尚未验收，S9–S12未开始，性能建设不属于本窗口。** 当前交付起点为已推送并核对远端的`f9cb7c8`。本批已补齐部分对应窗口的跨调用检查，32份回归结论不变；继续处理控制历史不同情况下的工作与依赖对应。

本文只保留当前资格、限制和恢复入口。[plan.md](plan.md)维护方法与验收条件，[log.md](log.md)保留实施、失败及交付经过。两份独立报告保持原样：后续修复不回写为当时已通过。

## 1. 阶段与当前资格

| 阶段 | 状态 | 已交付范围或剩余事项 |
| --- | --- | --- |
| S0–S3 环境、共同合同、TP与执行 | 已交付 | TP1/2/4/8、共享定义、动作/事件与就绪资源执行；不授予任意模型/TP迁移 |
| S4 PP | 已交付 | PP4、TP2×PP3的微批次和消息接续；不代表任意TP×PP组合 |
| S5 基础PD | 已交付 | 同机P TP1/D TP1、独立KV池、SDMA交接和多成员Decode |
| S6 条件EP | 已交付 | 完整Qwen1.5 MoE、EP1/2/4及按拓扑给定token/padding路由；不含真实权重自动补齐元数据 |
| S7 组合拓扑与采前复用 | 已交付 | TP/PP/EP及PD限定组合、代表角色规划与独立绑定；部分来源仍partial |
| S8-A 分层修复 | 后续修复与归因中 | 来源几何、KV发送、状态守恒和部分等待因果已获开发证据；不同组批前态及整窗口对应仍待收口 |
| S8-B 全量独立验证 | 上轮调查完成，未通过 | 存储关联任务已补执行；修复后的新基线尚未开始完整独立验证 |
| S9 调度与一级前缀缓存 | 未开始 | chunked prefill、Decode优先、混合组批、prefix caching |
| S10 单机整合 | 未开始 | 统一入口及边界审查 |
| S11 扩展探索 | 未开始 | 多机、原生调度器的有界探索 |
| S12 最终验收 | 未开始 | 新增调度/缓存及受影响结构的独立验证与交付 |

S6/S7的“已交付”只指当时限定开发范围；泛化和新增组合资格由S8判定。S7的27个开发目标、角色复用细节与历史失败保留在日志，不重复计入独立验收。

## 2. 独立调查与开发证据分开

### 固定的独立结果

[首轮报告](s8_independent_validation_report.md)：114个根案例均有终态，18域发布库；208个配置×请求子项中112个产生预测、6失败、90未执行；22个预选目标完成原生运行，逐rank为37 matched、22 differences、1 error。确认9项问题，原失败保留。

[重验证报告](s8_revalidation_report.md)：仍为114个根案例；核心/评价冻结`0daa08f`，适配冻结`375f22f`。下表是该轮最终事实，不包含之后的开发回归。

| 环节 | 结果 | 未闭合范围 |
| --- | --- | --- |
| 36个来源任务 | 24 complete、11 partial、1 error；35域发布库 | page256无合格来源 |
| 210个配置×请求子项 | 165产生预测、39拒绝、6未执行 | 不额外计入114个根案例 |
| 41个原生目标 | 26 matched、9预测拒绝、5 differences、1受阻 | 40项原生观察完成；page256目标未执行 |
| 30个条件展开 | 15 complete、9预期拒绝、5 error、1受阻 | 四个存储关联条件已尝试但Decode selection 2拒绝，另有page1错误和page256受阻 |
| 7个生命周期案例 | 6 complete、1 error | 中断探针遇容器发现/inspect竞争 |

存储关联来源、目标和条件均已补执行；最终驱动退出2源于page256两个下游缺来源，不能解释为仍有存储取消任务。旧8GiB停止及约948MiB部分采集保留；processing费用含300s未结算预留，不全部视为实测时间，也不清零。

### 后续开发已经证明什么

| 事项 | 当前证据 | 保留的限制 |
| --- | --- | --- |
| 完成合同、错误与恢复（R1/R2/R4/R5） | 真实输出/状态提交端口、control/action身份及有界错误上下文；TP/PP/PD限定回归和中断恢复通过 | 不代表所有失败路径或独立窗口均已通过 |
| 来源事实（R3/R6/R7/R8） | 真实模型stream、普通/补齐几何、动态model_tokens、expert可选元数据及mm/matmul/addmm事实；page1和PP3展开、三MoE/PP域16项控制展开完成 | 来源域间不自动互授资格；原八卡初始化停滞原因仍未知 |
| PP4执行关系 | 8个rank、48个模型主体及整窗口必要数据/端口/event/queue/stream关系通过 | 跨owner顺序可不同；证据是开发回归 |
| KV与请求状态 | 291个原生输入条件下的KV释放操作/端口/内部依赖一致；6轨迹568次预测状态变化、26rank的211次原生结果进度核对通过 | 177个局部释放因外部stream前沿仍partial；未独立核验原生每个物理页的所有权，也未证明策略等价 |
| P/D控制范围（R9） | 真实readiness边界声明后检查数据、设备/runtime和通信边界；原始图未删操作 | 只单列闭合的控制工作，不忽略模型TP或KV搬运 |
| P4/D2发送 | 原生调用处声明切片和当前槽位；42次发送调用正确，旧28处detach形状矛盾归零；4项展开完成，102次状态变化自洽 | 只适用于当前完整Prefill合同，其他P/D域尚未重建受影响来源 |
| 完整图等待 | 32份固定报告中，旧26份仍9 matched/17 differences；新Prefill四项从differences改为partial，Decode两项仍differences | 8条必要关系在原完整图间接成立；没有新matched，不授予外部工作或时延资格 |

Mistral PP3的66个模型主体通过；剩余差异涉及组批顺序、池前态及端口。已确认示例的原生空闲池23+3=26页、预测27+3=30页各自成立，不能要求不同历史下逐值相等。P/D原生Prefill可组1+7成员，当前轻量控制逐请求执行，比较须先确定真实调用条件；不能删batch_filter或强制相同池状态求匹配。

前批复用32份材料的288次过滤/合并调用，以153个输入条件作局部展开：277次操作/端口/依赖/执行一致，11次P端7→1过滤无来源分支。实际目标P rank均没有过滤调用，该缺口不扩大成当前轻量控制必需功能；错误与未验证范围保留。周期关联审查未确认新的业务修复需求；材料在`data/20261028T051510.641601Z_s8-a-batch-history/results/batch-history.md`及`review.json`。

本批比较器不再因窗口未完整配对而跳过已对应调用的关系。32份整例及逐flow结论均不变（9 matched、4 partial、19 differences），新增检查16个窗口范围：13个局部操作/端口/数据依赖一致，但外部执行关系仍partial；Mistral PP3三个rank的执行关系一致，另有135/136/174条原生数据依赖顺序未满足，全部跨批次，涉及页池、请求表和复用buffer等。它们尚未判定为核心漏边或合法控制差异，不自动剔除；7个没有操作对应的窗口仍无局部资格。证据在`data/20261028T053701.095788Z_s8-a-partial-window/results/partial-window.md`。

## 3. 模型与环境边界

本轮七模型为Qwen3-8B、Mistral-7B-v0.3、Qwen1.5-MoE-A2.7B、Qwen3-30B-A3B、Meta Llama-3.1-8B-Instruct、AllenAI OLMoE-1B-7B-0924、GLM-4-9B-Chat。固定官方完整架构及revision，运行dummy权重；不裁层/专家，不宣称真实文本或自然路由精度。

- Phi-3已按用户要求放弃，尝试性框架补丁撤回；不继续补采或改上游。OLMoE仅TP1/EP1有本轮资格，TP2与PP存在原生限制。当前Ascend不支持PP2，EP>1要求EP=TP。
- Llama、GLM无Markov/无profiler的TP1/TP2均完成8请求；各六组来源/展开完成，预选目标各3rank matched，不外推其他拓扑。配置位于`/root/models/markov-next/Meta-Llama-3.1-8B-Instruct`和`/root/models/markov-next/GLM-4-9B-Chat`，没有下载真实权重。
- page256在相同配置且关闭Markov观察/profiler后仍在原生SelfAttentionOperation失败；仅该配置无原生资格，不推导所有page256均不可用。
- 当前推理镜像`markov-next-sglang:ubuntu22.04`为`sha256:f2b139d2810d2760cbeb8c2a0614ecfb9b270b22011d806dfeaaac6368292337`。它在冻结B原生栈`bc4f703df478…`上仅重打包采集器；最近实采镜像`5201fea1a4a0…`与它只有经AST核对的排版差异。旧证据各自的镜像、包及revision保留在运行输入，不自动互授资格。

## 4. 必要资产入口

下面是已有材料索引，详细费用和失败以各任务state、输入及日志为准；不因此重新运行或清除旧证据。

| 运行目录（均在data下） | 内容与当前用途 | 已记录消耗 |
| --- | --- | --- |
| [20261026T192126.898346Z_s8-independent-validation](../../../data/20261026T192126.898346Z_s8-independent-validation) | 首轮冻结输入、完整调查及报告证据，保留历史 | native 46次/8854.583643640s；processing 7440.108056980s，约33.313GiB |
| [20261027T052222.352003Z_s8-a-completion-contract](../../../data/20261027T052222.352003Z_s8-a-completion-contract) | TP1错峰、PP4宽配置、page64 PD的完成合同回归；复用三个库 | native 0次；processing 30.918324510s，约106.72MiB |
| [20261027T055725.075155Z_s8-a-native-adapters](../../../data/20261027T055725.075155Z_s8-a-native-adapters) | 专家、矩阵、PD查询域及八卡上下文回归，本批结束 | native 12次/3072.877974340s；processing 1049.348708790s，约5.060GiB |
| [20261027T081658.000889Z_s8-revalidation](../../../data/20261027T081658.000889Z_s8-revalidation) | 正式B核心、评价和验证内容保持冻结；完整调查及有限资源补执行已汇总，仍未通过 | native83次/18658.623322040s；processing计费18865.792770440s（含300s预留）；上限native93次/24000s、processing28800s、临时512GiB/保留384GiB |
| [20261027T084920.584520Z_s8-a-head-geometry](../../../data/20261027T084920.584520Z_s8-a-head-geometry) | Phi3补丁镜像构建及撤回记录，批次已取消 | native 0次，未使用的额度不转为已验证结果 |
| [20261027T094225.476145Z_native-model-compatibility](../../../data/20261027T094225.476145Z_native-model-compatibility) | Llama原生TP1/TP2均完成；最早一次用户中断保留，无残留自有容器；不是模板采集 | 累计native 3次/336.958264120s，其中本次新增2次/244.343051650s，低于原800s总额 |
| [20261027T111111.818737Z_glm4-native-compatibility](../../../data/20261027T111111.818737Z_glm4-native-compatibility) | GLM原生TP1/TP2各完成8请求，无残留自有容器；不是模板采集 | native 2次/221.731706350s，低于800s总额 |
| [20261027T223827.749481Z_s8-a-followup](../../../data/20261027T223827.749481Z_s8-a-followup) | 中断恢复、page1及PP3限定回归，批次已交付 | native 5次/759.262550510s；processing 481.075479820s，含失败与中止费用 |
| [20261027T233148.469927Z_s8-a-source-reuse](../../../data/20261027T233148.469927Z_s8-a-source-reuse) | 三个既有MoE/PP域完整重处理，5/16展开成功，批次结束 | native 0次；processing 299.610587880s，原B/A来源及费用保留 |
| [20261027T235923.822557Z_s8-a-native-geometry](../../../data/20261027T235923.822557Z_s8-a-native-geometry) | 三域16项控制展开完成，P/D仅恢复已有来源的处理；原错误保留，所有pending为空 | native 3次/1290.994050240s；来源processing 1981.211293870s，打包6.036114980s；轮上限native 3次/2700s、processing6300s、临时256GiB/保留192GiB |
| [20261028T011258.712673Z_s8-a-window-causality](../../../data/20261028T011258.712673Z_s8-a-window-causality) | 六案例26rank完整开发核对，9rank窗口完整对应、17rank仍不完整；旧B未回写，初次导入错误保留 | native 0次；processing 452.908701610s；轮上限processing2000s、临时128GiB/保留96GiB；所有pending为空 |
| [20261028T014001.630772Z_s8-a-stream-bindings](../../../data/20261028T014001.630772Z_s8-a-stream-bindings) | 复用六案例26rank已有图，9 matched/17 differences；同owner内工作顺序与真实等待关系保留，其他状态/组批差异未放宽 | native 0次；processing 169.108643210s；轮上限processing1500s、临时128GiB/保留96GiB；所有pending为空 |
| [20261028T021745.846849Z_s8-a-native-state-contexts](../../../data/20261028T021745.846849Z_s8-a-native-state-contexts) | 291项原生条件下的KV局部逻辑一致，177项外部stream前沿仍partial；26份完整报告不变；临时展开图消费后移除 | native 0次；processing 441.948665880s，含初次26项回归路径错误；轮上限processing4000s、临时128GiB/保留96GiB；所有pending为空 |
| [20261028T032834.521224Z_s8-a-request-state](../../../data/20261028T032834.521224Z_s8-a-request-state) | 六预测状态自洽、26rank原生请求进度核对；新诊断不改变预测，消费后移除完整新图 | native 0次；processing 146.853466270s；轮上限processing4000s、临时128GiB/保留96GiB；所有pending为空 |
| 20261028T034804.960469Z_s8-a-control-scope | P4/D2原生控制声明、旧26报告回归和形状/因果诊断；新六rank仍differences | native 1次/276.658092160s；processing 655.566174340s，保留约3.218GiB；pending为空 |
| 20261028T041813.701342Z_s8-a-kv-send-geometry | P4/D2发送几何/槽位回归，4项展开及6rank观察完成；整例仍differences | native 2次/610.101543780s；processing 1057.184147800s；保留约6.648GiB，pending为空 |
| 20261028T050306.809923Z_s8-a-wait-causality | 32份固定图的等待因果复核；旧26状态不变，新Prefill四项partial、Decode两项differences | native 0次；processing 202.356130460s；保留约61.74MiB，pending为空 |
| 20261028T051510.641601Z_s8-a-batch-history | 批次前态及288次条件调用核对、周期链路审查；原整例结论不变 | native 0次；processing 174.116642650s；pending为空 |
| 20261028T053701.095788Z_s8-a-partial-window | 补查16个部分对应窗口并定位跨批次依赖差异；32份整例/逐flow结论不变 | native 0次；processing 216.715559830s；pending为空 |

## 5. 恢复工作顺序

1. 收口批次前态与工作对应：区分跨批次页池、请求表和复用buffer的状态关系与请求本身必须保持的因果；相同条件验证模板，各自轨迹核对状态与依赖。未知差异保留，不按依赖种类批量排除，也不自动改为通过。
2. 完成受影响来源域的有限回归、关联审查和代码/文档/数据维护交付；已用于修复的目标转为开发材料。
3. 再冻结核心、评价及输入，按预定备用请求执行完整新B。适配变更须说明来源域并重建复验，旧冻结报告不覆盖。
4. S8通过并交付后进入S9，再完成S10–S12；性能建设继续后置。
