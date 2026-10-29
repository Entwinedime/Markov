# S8-B 全量重验证报告

日期：2026-10-28。结论：**S8未通过，继续保留全部失败和未覆盖项。** 本报告覆盖本轮114个根案例及210个来源预测子项，另包含用户要求补回的存储关联任务与一次有限时间额度恢复。原先独立报告保持原样，本报告不把后来的结果回写为早先已通过。

## 1. 先看结果

| 环节 | 范围 | 本轮结果 | 怎样理解 |
| --- | ---: | --- | --- |
| 来源任务 | 36 | 24 complete、11 partial、1 error | 35域发布库；partial表示仍有目标不能展开 |
| 来源内部配置×请求预测 | 210 | 165产生预测、39拒绝、6未执行 | 这些是来源任务内部子项，不再加进114的分母 |
| 原生目标 | 41 | 26 matched、9预测拒绝、5 differences、1 blocked | 40项完成真实原生观察；9项预测拒绝没有逐rank结构比较，page256目标未执行 |
| 条件展开 | 30 | 15 complete、9按预期拒绝、5 error、1 blocked | 条件结果采用给定节点耗时，不是原生性能或结构对照 |
| 生命周期 | 7 | 6 complete、1 error | 中断探针自身报错，业务中断/恢复资格尚未获得 |

实际逐rank结构报告为 **61 matched、23 differences**，来自有预测且进入比较的目标。原生内部路由/批次检查的matched，不等于预测结构matched；预测拒绝的9例不能借原生完成算通过。26个匹配目标覆盖七个模型各至少一个限定拓扑，仍不能授予任意模型、任意拓扑或完整组合资格。

调查程序最终仍退出2，具名待项为`native/qwen3-page256/small/unit-capacity`和`conditional/C-equal-pages-256`。完整清单有记录与完整实际覆盖是两件事。

证据入口：[冻结输入](../../../data/20261027T081658.000889Z_s8-revalidation/inputs/matrix.json)、[完整结果与历史](../../../data/20261027T081658.000889Z_s8-revalidation/results/outcomes.json)、[逐文件审查及诊断](../../../data/20261027T081658.000889Z_s8-revalidation/results/code-review.json)、[资源恢复前后记录](../../../data/20261027T081658.000889Z_s8-revalidation/results/storage-amendment.json)。下方清单逐一对应原114项，没有因失败删除行。原始data在本机保留、不随Git提交；本报告保存可独立阅读的摘要，原始图复核仍依赖对应数据。

## 2. 冻结范围与验证方法

核心与评价冻结`0daa08f37b9080055820b0f34ee0bcdfb938333b`，适配冻结`375f22f0adca36150281d6753ad6ffe61b05c81b`；冻结登记时间为`2026-10-27T11:30:21.809034+00:00`。本轮没有修改src、example、Docker、scripts、third_party、静态设计或构建约束。原生SGLang为官方`0460f277b770f8b5a404da2e50e7a9573ef8db40`，未为本轮模型/组合加框架patch。

| 环境 | 固定镜像 |
| --- | --- |
| SGLang/Ascend | `sha256:bc4f703df47873e328e9a758db88dcd4ef557adb1cf513c997e5b1d238976a63` |
| C++23/Python建模 | `sha256:e066a5196c3b34b2f46079227e4b5c2a1a811c9d06defca329175954101ca7ca` |

源码、构建SDK及实际模型配置还保留在各任务的`inputs/environment.json`与原始采集目录中。运行完整原生模型架构，使用dummy权重和预先给定的EP路由/padding；不声称真实权重路由、生成文本或数值正确性。来源与目标都按统一预热开始，初始化、编译和预热计入采集费用，不作为预测服务耗时。

流程是：先运行来源并建库，完成预声明配置×请求的展开，再观察预选原生目标，最后运行条件与生命周期清单。模板、规则与评价没有按目标结果修改；资源恢复只复用原始来源和原输入。请求seed为`271031`，备用seed为`271047, 271063`，备用实际请求已预先保留；已用于诊断/修复的目标后续转为回归。

原生比较要求同时到达请求，检查已配对流程的操作/公开几何、可用CPU与KV状态、NPU工作、提交/等待/事件关系，并检查可比较窗口。条件案例另外覆盖错峰、容量、成员数及给定不同成本。不能把内部执行完成、局部模型主体匹配、默认/明细一致性直接当成整窗口原生等价。

### 与首轮覆盖的对应

| 观察 | 首轮 | 本轮 |
| --- | ---: | ---: |
| 发布来源库 | 18域 | 35域 |
| 来源预测产生结果 | 112/208 | 165/210 |
| 来源预测未执行 | 90 | 6 |
| 预选原生目标完成观察 | 22 | 40 |

两轮有31个共同来源任务、36个同名原生目标；移除Phi3 TP1/TP2和OLMoE EP2/EP4/EP2×PP3，新增Llama TP1/TP2、GLM TP1/TP2及OLMoE EP1，原失败保留。模型替换按用户决定和原生可运行前提，相关EP/PP机制仍由其他合格模型承担。

36个同名原生目标中，27个的请求/EP路由数据已变化，9个固定边界输入保持：page1/16/64/256的unit-capacity、TP1的members-32/33/128与long-outputs、TP2的kv262144/context-boundaries。后者保守标为固定输入重测/回归，不冒充新输入泛化；另有5个新模型域目标。全部实际目标重新观察，没有拿旧trace阅读代替新运行。两轮代码、镜像、模型资格和部分判据不同，不能用上述数量直接计算性能精度或泛化提升百分比。

## 3. 模型与来源清单

模型、TP/PP/EP/PD、dtype和页布局的各实现域使用自身来源库。容量、请求成员和请求序列等域内变化由已有模板变换与动态控制展开；本轮没有证明一个库跨全部实现域复用。

| 模型ID | 原生模型配置来源 |
| --- | --- |
| `qwen3-8b` | `Qwen/Qwen3-8B`，revision `b968826d9c46dd6066d109eabc6255188de91218` |
| `mistral-7b` | `mistralai/Mistral-7B-v0.3`，revision `caa1feb0e54d415e2df31207e5f4e273e33509b1` |
| `qwen15-moe` | `Qwen/Qwen1.5-MoE-A2.7B`，revision `1a758c50ecb6350748b9ce0a99d2352fd9fc11c9` |
| `qwen3-moe` | `Qwen/Qwen3-30B-A3B`，revision `ad44e777bcd18fa416d9da3bd8f70d33ebb85d39` |
| `llama31-8b` | `NousResearch/Meta-Llama-3.1-8B-Instruct`，revision `d10aef7999a2b5ba950ab3974312feeedbfe0b77` |
| `olmoe` | `allenai/OLMoE-1B-7B-0924`，revision `6d84c48581ece794365f2b8e9cfb043c68ade9c5` |
| `glm4-9b-chat` | `zai-org/glm-4-9b-chat`，revision `bd8234fe5e0c09c48637a92abb0c797cb5fa0e73` |

Llama使用固定公开配置镜像；GLM是原版GLM-4-9B-Chat。Phi-3已按用户决定退出；OLMoE只验证已确认原生可运行的TP1/EP1，不为原生不支持的多卡/PP组合修改框架。

下面每行对应一个来源根案例。成功/拒绝/未执行统计该任务的配置×请求子项，库只按发布目录计算，不将每rank或每目标展开图另算成库。

| 来源任务 | 模型 | 根状态 | 成功/拒绝/未执行 | 已发布库 |
| --- | --- | --- | ---: | ---: |
| `qwen3-tp1` | qwen3-8b | complete | 30/0/0 | 1 |
| `qwen3-tp2` | qwen3-8b | complete | 12/0/0 | 1 |
| `qwen3-tp4` | qwen3-8b | complete | 4/0/0 | 1 |
| `qwen3-tp8` | qwen3-8b | complete | 4/0/0 | 1 |
| `qwen3-tp1-pp3` | qwen3-8b | partial | 3/3/0 | 1 |
| `qwen3-tp1-pp4` | qwen3-8b | partial | 3/3/0 | 1 |
| `qwen3-tp2-pp3` | qwen3-8b | partial | 3/3/0 | 1 |
| `qwen3-tp2-pp4` | qwen3-8b | partial | 3/3/0 | 1 |
| `mistral-tp1-pp1` | mistral-7b | complete | 4/0/0 | 1 |
| `mistral-tp4-pp1` | mistral-7b | complete | 4/0/0 | 1 |
| `mistral-tp1-pp3` | mistral-7b | partial | 3/3/0 | 1 |
| `mistral-tp1-pp4` | mistral-7b | partial | 3/3/0 | 1 |
| `qwen15-moe-ep1` | qwen15-moe | complete | 4/0/0 | 1 |
| `qwen15-moe-ep2` | qwen15-moe | complete | 4/0/0 | 1 |
| `qwen15-moe-ep4` | qwen15-moe | complete | 4/0/0 | 1 |
| `qwen3-moe-ep2` | qwen3-moe | complete | 4/0/0 | 1 |
| `qwen3-moe-ep4` | qwen3-moe | complete | 4/0/0 | 1 |
| `qwen3-moe-ep8` | qwen3-moe | complete | 4/0/0 | 1 |
| `qwen3-moe-ep2-pp3` | qwen3-moe | partial | 2/4/0 | 1 |
| `qwen3-moe-pd2-2` | qwen3-moe | partial | 0/4/0 | 1 |
| `qwen3-pd4-2` | qwen3-8b | complete | 4/0/0 | 1 |
| `qwen3-pd2-4` | qwen3-8b | complete | 4/0/0 | 1 |
| `qwen3-moe-pd-pp3` | qwen3-moe | partial | 0/4/0 | 1 |
| `llama31-tp1` | llama31-8b | complete | 6/0/0 | 1 |
| `llama31-tp2` | llama31-8b | complete | 6/0/0 | 1 |
| `glm4-tp1` | glm4-9b-chat | complete | 6/0/0 | 1 |
| `glm4-tp2` | glm4-9b-chat | complete | 6/0/0 | 1 |
| `olmoe-ep1` | olmoe | complete | 4/0/0 | 1 |
| `qwen3-fp16` | qwen3-8b | complete | 4/0/0 | 1 |
| `qwen3-page1` | qwen3-8b | partial | 0/6/0 | 1 |
| `qwen3-page16` | qwen3-8b | complete | 6/0/0 | 1 |
| `qwen3-page64` | qwen3-8b | complete | 6/0/0 | 1 |
| `qwen3-page256` | qwen3-8b | error | 0/0/6 | 0 |
| `qwen3-page64-pp3` | qwen3-8b | partial | 3/3/0 | 1 |
| `qwen3-moe-page64-ep2` | qwen3-moe | complete | 4/0/0 | 1 |
| `qwen3-page64-pd1-1` | qwen3-8b | complete | 4/0/0 | 1 |

39个来源预测拒绝分为：七个Dense PP域的21个同类selection 1拒绝（部分源角色已确认上下文不一致），MoE PP/P/D的12个选择/几何条件缺口，以及page1的6个`released_pages`缺口。page256的6个预测子项因来源预热失败未执行。它们是有效配置下的当前能力缺口，不能全归为用户输入错误；归组不等于每个角色的唯一根因均已证明。

## 4. 全部原生目标

M/D分别是逐rank报告的matched/differences数量。`原生完成`只说明观察运行正常完成；没有预测的行不具备rank结构资格。表内目标名完整保留配置与workload，特别是TP2×PP4使用full-micro/boundaries，Mistral PP3使用small/boundaries。

| 目标 | 整例状态 | 原生完成 | rank M/D | 首个拒绝或差异范围 |
| --- | --- | --- | ---: | --- |
| `qwen3-tp1/small/boundaries` | matched | 是 | 1/0 | — |
| `qwen3-tp1/r32/members-32` | matched | 是 | 1/0 | — |
| `qwen3-tp1/r33/members-33` | matched | 是 | 1/0 | — |
| `qwen3-tp1/r128/members-128` | matched | 是 | 1/0 | — |
| `qwen3-tp1/wide/long-outputs` | matched | 是 | 1/0 | — |
| `qwen3-tp2/wide/boundaries` | matched | 是 | 2/0 | — |
| `qwen3-tp2/kv262144/context-boundaries` | matched | 是 | 2/0 | — |
| `qwen3-tp4/wide/boundaries` | matched | 是 | 4/0 | — |
| `qwen3-tp8/wide/boundaries` | matched | 是 | 8/0 | — |
| `qwen3-tp1-pp3/wide/boundaries` | error | 是 | 0/0 | selection 1 |
| `qwen3-tp1-pp4/wide/boundaries` | error | 是 | 0/0 | selection 1 |
| `qwen3-tp2-pp3/wide/boundaries` | error | 是 | 0/0 | selection 1 |
| `qwen3-tp2-pp4/full-micro/boundaries` | differences | 是 | 2/6 | 见问题5：窗口/批次/状态及任务归属 |
| `mistral-tp1-pp1/wide/boundaries` | matched | 是 | 1/0 | — |
| `mistral-tp4-pp1/wide/boundaries` | matched | 是 | 4/0 | — |
| `mistral-tp1-pp3/small/boundaries` | differences | 是 | 0/3 | 见问题5：窗口/批次/状态及任务归属 |
| `mistral-tp1-pp4/wide/boundaries` | error | 是 | 0/0 | selection 1 |
| `qwen15-moe-ep1/wide/varying` | matched | 是 | 1/0 | — |
| `qwen15-moe-ep2/wide/varying` | matched | 是 | 2/0 | — |
| `qwen15-moe-ep4/wide/varying` | matched | 是 | 4/0 | — |
| `qwen3-moe-ep2/wide/varying` | matched | 是 | 2/0 | — |
| `qwen3-moe-ep4/wide/varying` | matched | 是 | 4/0 | — |
| `qwen3-moe-ep8/wide/varying` | matched | 是 | 8/0 | — |
| `qwen3-moe-ep2-pp3/wide/varying` | error | 是 | 0/0 | selection 1, selection 13 |
| `qwen3-moe-pd2-2/wide/varying` | error | 是 | 0/0 | selection 2 |
| `qwen3-pd4-2/wide/boundaries` | differences | 是 | 0/6 | 见问题5：窗口/批次/状态及任务归属 |
| `qwen3-pd2-4/wide/boundaries` | differences | 是 | 0/6 | 见问题5：窗口/批次/状态及任务归属 |
| `qwen3-moe-pd-pp3/wide/varying` | error | 是 | 0/0 | selection 2 |
| `llama31-tp1/wide/boundaries` | matched | 是 | 1/0 | — |
| `llama31-tp2/wide/boundaries` | matched | 是 | 2/0 | — |
| `glm4-tp1/wide/boundaries` | matched | 是 | 1/0 | — |
| `glm4-tp2/wide/boundaries` | matched | 是 | 2/0 | — |
| `olmoe-ep1/wide/varying` | matched | 是 | 1/0 | — |
| `qwen3-fp16/wide/boundaries` | matched | 是 | 1/0 | — |
| `qwen3-page1/small/unit-capacity` | error | 是 | 0/0 | missing parameter: released_pages |
| `qwen3-page16/small/unit-capacity` | matched | 是 | 1/0 | — |
| `qwen3-page64/small/unit-capacity` | matched | 是 | 1/0 | — |
| `qwen3-page256/small/unit-capacity` | blocked | 未获完成证据 | 0/0 | 缺来源，未执行下游 |
| `qwen3-page64-pp3/wide/boundaries` | error | 是 | 0/0 | selection 1 |
| `qwen3-moe-page64-ep2/wide/varying` | matched | 是 | 2/0 | — |
| `qwen3-page64-pd1-1/wide/boundaries` | differences | 是 | 0/2 | 见问题5：窗口/批次/状态及任务归属 |

### 采前代表复用的已获证据

| 来源域 | 来源实际观察 | 派生角色 | 目标结果与资格 |
| --- | --- | --- | --- |
| Qwen3 Dense TP4 | 0/1 | 2/3 | 四个原生rank全部matched |
| Qwen3 Dense TP8 | 0/1 | 2–7 | 八个原生rank全部matched |
| Qwen3 MoE EP4 | 0/1 | 2/3 | 四个原生rank全部matched |
| Qwen3 MoE EP8 | 0/1 | 2–7 | 八个原生rank全部matched |
| Qwen3 TP2×PP4 | 0/1/2/3/6/7 | 4/5 | 48个非空模型主体通过；整例differences，不能授予完整角色复用资格 |

复用规划发生在采集前，派生角色具有自身rank/词表等绑定；目标真实观察全部rank。EP协议仍需真实原生参与者，观察2个代表不意味着用2张卡运行8-rank协议，也不将减少观察量等同减少全部启动/通信成本。不同TP/EP实现域仍独立采来源。

## 5. 全部条件与生命周期案例

条件案例的complete检查给定节点成本下的完成、状态及执行关系；9个rejected都符合预先声明的拒绝预期。它们不提供新的原生性能准确性证明。

| 条件案例 | 预期 | 结果 | 说明 |
| --- | --- | --- | --- |
| `D-running-1` | complete | complete | — |
| `D-running-2` | complete | complete | — |
| `D-running-31` | complete | complete | — |
| `D-running-64` | complete | complete | — |
| `D-running-101` | complete | complete | — |
| `G-seed-64` | complete | complete | — |
| `G-seed-128` | complete | complete | — |
| `G-seed-256` | complete | complete | — |
| `K-qwen3-tp4-host` | complete | complete | — |
| `K-qwen3-tp4-device` | complete | complete | — |
| `K-qwen3-tp4-communication` | complete | complete | — |
| `K-qwen3-tp4-transfer` | complete | complete | — |
| `K-qwen3-moe-pd-pp3-host` | complete | error | selection 2 |
| `K-qwen3-moe-pd-pp3-device` | complete | error | selection 2 |
| `K-qwen3-moe-pd-pp3-communication` | complete | error | selection 2 |
| `K-qwen3-moe-pd-pp3-transfer` | complete | error | selection 2 |
| `E-request-exceeds-pool` | rejected | rejected | command failed (1): markov: request exceeds the native input length limit: too-large  |
| `A-zero-prompt` | rejected | rejected | command failed (1): markov: request needs nonnegative arrival and positive token lengths: request-0  |
| `A-type` | rejected | rejected | command failed (1): markov: serving fields must be integers  |
| `A-duplicate-request` | rejected | rejected | command failed (1): markov: request IDs must be nonempty and unique  |
| `C-wrong-page-asset` | rejected | rejected | command failed (1): markov: no region layout matches request_cache_release/prefill layout 111 condition "relea |
| `A-wrong-topology-asset` | rejected | rejected | command failed (1): markov: target PP differs from its source topology  |
| `C-equal-pages-1` | complete | error | missing parameter: released_pages |
| `C-equal-pages-16` | complete | complete | — |
| `C-equal-pages-64` | complete | complete | — |
| `C-equal-pages-128` | complete | complete | — |
| `C-equal-pages-256` | complete | blocked | 缺来源，未执行下游 |
| `A-missing-routes` | rejected | rejected | command failed (1): markov: [json.exception.out_of_range.403] key 'ep_routes' not found  |
| `A-duplicate-expert` | rejected | rejected | command failed (1): markov: one token cannot select the same expert twice  |
| `A-route-gap` | rejected | rejected | command failed (1): markov: incomplete route for request-0, layer 0  |

| 生命周期案例 | 原定行为 | 结果 | 说明 |
| --- | --- | --- | --- |
| `L-cli-invalid` | reject-task | complete | — |
| `L-help-after-error` | help | complete | — |
| `L-resume-completed` | resume-completed | complete | — |
| `L-budget-stop` | budget | complete | — |
| `L-interrupt-resume` | interrupt | error | 探针inspect容器时遇No such object；预定中断/恢复未验证 |
| `L-bind-input` | missing-input | complete | — |
| `L-protect-input` | protect-input | complete | — |

四个MoE P/D+PP3成本类别条件已实际尝试，均在Decode selection 2拒绝；page1条件缺released_pages，page256条件缺库。不能把这些错误算作成本类别或目标结构已经验证。

## 6. 问题、归属和证据

### 1. PP辅助采样与模型执行上下文不一致（B-O1，必须修复）

确认的适配采集缺口。来源中同一语义API的模型调用和辅助原型使用不同执行资源上下文，核心保留隔离后找不到目标变体；涉及七个Dense PP域的21个来源预测和五个预选原生目标。部分源角色已逐条核对同语义选择键、不同上下文，不能靠放宽guard宣告等价。

位置：[hooks.py](../../../src/collection/markov_collection/adapters/frameworks/sglang/hooks.py)的model_stream及辅助采集，[materialization.py](../../../src/collection/markov_collection/adapters/runtimes/ascend/materialization.py)。证据为`code-review.json`的B-O1及对应源catalog/执行变体。应统一原生来源上下文的交接，再重建受影响来源；不为五个目标各补一条规则。

### 2. MoE物化/索引/填充条件仍有选择缺口（B-O2，必须闭合，根因未定）

确认来源与目标展开存在缺口，尚未确认全部责任在核心或适配。MoE EP2×PP3涉及Prefill selection 1/13及small Decode selection 15；MoE P/D两域的Decode selection 2持续拒绝，包含floor_divide的填充行条件和索引operand布局等来源证据。12个来源预测、三个预选原生目标及四个成本条件受到影响。

位置：[expert_geometry.py](../../../src/collection/markov_collection/adapters/frameworks/sglang/expert_geometry.py)、[shapes.py](../../../src/collection/markov_collection/adapters/runtimes/pytorch/shapes.py)及共享变体/几何交接。证据为B-O2的source_selection_evidence和pd_source_extensions。应沿真实调用的参数、布局、填充语义和声明作用域判断所有者；不能按失败名称硬编码shape或无限扩大采样网格。

### 3. Page1未覆盖实际分配入口，且出现参数作用域缺口（B-O4，必须修复）

确认框架适配只覆盖alloc_extend/alloc_decode，而该原生路径走alloc；source serving分配声明缺失。六个来源预测、一个原生目标及一个条件均缺released_pages。已确认入口缺口，但它是否解释全部变量传播问题尚未证明。

位置：[preparation.py](../../../src/collection/markov_collection/adapters/frameworks/sglang/preparation.py)的分配包装、[allocation.py](../../../src/collection/markov_collection/adapters/frameworks/sglang/allocation.py)及模板参数交接。source与原生目标证据均保留在B-O4。先在adapter按真实分配生命周期补齐共同事实，再查共享参数边界；不提供默认released_pages掩盖错误。

### 4. Page256来源预热失败（B-O5，原生资格待澄清）

来源在SelfAttentionOperation预热失败，未进入有效采集或发布库；原生目标和对应条件因此未执行。退出信息不能单独证明OOM、框架不支持或核心出错。

证据：[page256原日志](../../../data/20261027T081658.000889Z_s8-revalidation/runs/qwen3-page256/logs/run.log)、B-O5。应使用同模型/配置、无Markov观察的有限原生资格运行区分原生支持和观察影响；不以修改原框架或换掉原配置冒充该格通过。

### 5. PP/P/D窗口、批次和状态未获得一致性资格（B-O6/B-O7及PD组，必须闭合，根因未定）

| 观察 | 已证明 | 尚未证明 |
| --- | --- | --- |
| Qwen3 TP2×PP4 full-micro | 8rank的48个非空模型主体和可用逐批检查通过；批次顺序相同，rank6/7整例matched | rank0–5整窗口错序；序列不匹配后整窗口依赖/端口检查未得到完整结论 |
| Mistral PP3 small | 3rank的66个非空模型主体通过，批次集合可配对 | 批次顺序、部分空闲/释放页及公开端口不同，不能认定与上一项同因 |
| Dense P4/D2、P2/D4、page64 P/D1/1 | 已配对的非空模型主体分别12/18/5个通过，完成及内部执行检查通过 | 大量批次未配对，P端存在未归属原生物理任务；可用逐批操作/拷贝/页状态亦有差异 |

P/D每个P rank仅配对1个流程，尚有1个原生、7个目标流程未配对；每个D rank配对5个流程，尚有5个原生、14个目标流程未配对。未归属原生任务：P4/D2每个P rank 295072；P2/D4两个P rank为590144/581960；page64 P/D1/1的P rank为1312。它们是原生观察中的任务，不是模拟目标节点。先按统一流程确认范围：范围外按约定忽略，范围内承担必要工作或因果的事件应归属；不要求为全部trace任务建立模型，也不以删掉计数掩盖缺口。

位置：[ServingStructure](../../../src/markov/adapters/frameworks/sglang/structure.cpp)、[BatchViews](../../../src/markov/adapters/frameworks/sglang/batches.cpp)、原生`event_loop_pp`以及[StructureComparison](../../../example/automatic_graph/structure.py)。证据为B-O6/B-O7和pd_native_group的逐rank报告。应先明确控制决策、观察覆盖、流程配对和必要因果的责任，再修具体实现或评价方法；不能以“局部模型一样”直接放宽整图判据，也不能把全部差异先归为核心错误。这些目标已经用于诊断，修复后转为回归，独立证据使用预冻结备用请求。

### 6. 中断探针存在容器发现竞争（B-O8，验证编排需修复）

确认example编排问题。docker ps给出短生命周期容器ID后，未经处理的inspect遇No such object并抛CalledProcessError，预定SIGINT/恢复检查中断；这不是业务恢复失败的直接证据。

位置：[interrupt_resume](../../../example/automatic_graph/investigation.py)约409行。证据为调查日志86行、L-interrupt-resume记录及其遗留state。应在验证编排内处理正确的容器生命周期交接并保留真正Docker错误，重新验证完整中断/恢复。不要改业务状态、清计费或为探针另造核心路径。

### 已有修复的新增资格：R4

Page1保留历史6个预测错误，恰等于全部组合；相同输入的当前check_workloads返回0个invalid，观察器确实第二次构建，原生目标完成。结合[prepare](https://github.com/Entwinedime/Markov/blob/0daa08f37b9080055820b0f34ee0bcdfb938333b/src/collection/markov_collection/workflow/exploration.py)约198行，R4从仅静态证据补充为该历史错误场景的限定运行证据。未将它扩大为全部失败/恢复路径均正确，page1业务参数错误仍保留。

## 7. 存储恢复与实际费用

原临时/保留总额120/96GiB调整为512/384GiB；36个来源任务统一临时64GiB、保留48GiB。两项8GiB来源停止复用原采集恢复，新增native为0，processing合计25.353154680s。来源、两个原生目标和四个条件均已补执行：两个原生观察完成但Decode selection 2拒绝，四个条件同样拒绝。**存储取消已补回，不代表这些预测通过。**

MoE P/D+PP3首次目标受原任务1300s总额限制，原生窗口489s，原失败及约948MiB部分数据保留；只追加一次尝试，任务总额2100s、单域native次数上限2→3，整轮93次/24000s不变。新attempt-3原生阶段完成，该任务native累计1800.357286880s；未重新采来源、降低rank数或修改请求/路由。

| 资源 | 累计计费/次数 | 本轮上限 | 口径 |
| --- | ---: | ---: | --- |
| native | 83次；18658.623322040s | 93次；24000s | 包含失败、来源及原生目标，保留全部实际已结算消耗 |
| processing | 18865.792770440s | 28800s | 包含中断探针未结算300s保守预留 |

处理中断探针的state仍为running/pending=processing，native为0；它的300s是启动前预占，启动器被终止后无法按正常返回结算。不可全部称作已测实际耗时，也不手工退款。其余已结算processing计费18565.792770440s；未知中断步的实际耗时不编造。最终调查退出2的待项只有page256原生目标和条件，时间额度目标已完成有限补执行。

## 8. 代码审查、下一步和交付边界

本轮逐文件阅读自有src下125份C++/Python、20651物理行，以及automatic_graph的12份Python、3558行；行数含空行/注释，排除外部、deprecated、build和data。结合原生入口、观察/解释、模板生产、变换、控制、事件执行和外围编排检查交接。读取全量不等于证明全部行为；当前代码没有因新场景继续增加分支、适配或拟合常量。

建议按以下顺序推进，并保持同类事实只有一个维护位置：

1. 先用有限原生资格证据澄清page256边界，并保留原失败格；修复容器探针的实际编排竞争，恢复可信的生命周期检查。
2. 统一补齐来源上下文、分配入口及MoE几何/条件的交接。先判断事实在原生采集、adapter解释还是共享模板规则中缺失，再在所有者处修改，不逐目标补规则。
3. 对PP/P/D分别核对控制策略、观察/配对范围与必需数据/通信因果。只有证明差异合法或定位错误后才能调整评价或装配，不把相同kernel/节点数当成结构等价。
4. 修复后审查完整关联代码、目录和产物，分层提交推送再冻结。核心/评价变化返回A并重新完整B；适配变化按有依据的受影响域重建与复验。已暴露目标作回归，备用请求作独立证据，不以挑选容易案例关闭缺口。

S8尚未验收，S9–S12未开始，性能层建设仍不属于本窗口。当前不能声明所有目标执行结构已经正确，也没有性能精度或MAPE结论。
