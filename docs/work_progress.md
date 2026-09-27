# 当前工作进展

更新时间：2026-09-27

## 当前目标与未完成项

当前进行的是用户流程与代码清理重构，不是新一轮按误差调参。
按 09-27 最新要求，先跑通正式组的补采、构模和预测，推广到当前完整 60-cross，
再集中完成整体代码瘦身与保留能力回归。此前的规模、可读性及校准管理目标不变。
实施依据见[计划](tmp/user_workflow_calibration_refactor_plan_20260923.md)，批次证据见[日志](tmp/user_workflow_calibration_refactor_log_20260923.md)。

当前代码已统一公开准备与单独构模的观测准入，预测与 target 评分保持隔离。
预测任务只由显式选择的 source/target 组合决定，已删除按输入顺序截断前 N 格的开发参数及报告状态。
成本选择、缺口报告及模型发布现由同一构模函数负责，已删除 coverage 中转模块和成本字典的二次装拆。
成本先取 base，可辨识时直接外推；缺失服务可复用独立参数或安排组内共享补采。首次准备不再要求先采完整物理包。
部署几何准备现与物理采样参数分开：首次准备自动读取部署信息，无需额外声明；实际 I/O 补采仍要求独立页域、放置和预算。
仅声明 CPU 轻量回放预算时，现可从所选 base 自动规划缺失配对和 TP，无须重复手写 pairs；不影响已有成本复用。
CPU 回放 dry-run 现会读取当前 base 的请求规模并检查累计预算；入口直接显示单次回放需求和 CPU 准备停止原因。
缺 base 时也可使用普通单实验或显式 experiments，不再强制矩阵；显式 token plan 只规划回放，
实验覆盖项和普通 config/workload 标签贯穿选择、补采与 manifest 读取。

具体已接通：DMA 与新页成本的 base 辨识；DMA、预取、新旧页的逐服务补充；CPU 同源校正与共享控制采集。
物理采集已清退旧的一次采全分支，正式组流程与单项排查共用逐服务采样和报告合并；已有完整校准报告仍可读取。
token 记录不再采集无用 trace；控制原语不再回退到未校正历史 CPU；实验完成而缺项不变时停止，不原样重复。
执行补采中，缺少某项声明或适用采样器不再阻断其他独立需求；实际进程失败、无效产物或清理未完成仍停止后续采集。
预算按各项实验规模分别检查累计账本：大实验超限不再跳过同一预算内较小的后续实验或已有测量复用。
预取等待程序已按目标实际活跃分支请求；不再预扫描后强制补齐另一策略的等待成本，本地返回成本仍不能缺失。
正式构模已删除历史静态标量控制参数的估计链及专用样本导出；历史模型读取和静态回放保留，现行操作级 CPU 成本和同源校正不变。
预取控制补采已去掉旧写入验证用的重写尾段；代表输入三阶段从 42 请求减为 27 请求，C3 的等待补采已实机完成。
这些能力已有局部检查、资产构模和下述完整矩阵的执行证据；更广的自动缺口处理及瘦身后验收仍未完成。

09-27 普通释放补采闭环已实机跑通：修正校准 workload 的双插入问题后，两 rank 各采到四次
普通释放，完成完整/轻量配对校正和成本导出，prepare 自动恢复预测，三目标全部 EXECUTED。
C3、C4、C5 的预测 HTTP 时间分别为 11.627714、9.959905、8.422972 秒，均保留 partial 成本覆盖提示。
结果位于 `user_workflow_refactor_20260923/control_capture_group`；这不是新的 target 精度成绩。
当前完整 60 格已在 `full60_current_20260927` 完成：五组正式 prepare 均为 predicted、各 12/12，
没有失败或跳过。全部预测结束后，正式 evaluate-hicache 使用 30 份正常运行测量（15 个配置/workload
组合各两次）完成 60/60 独立评分；没有重预测、补采或 target DAG 提取。

完整 HTTP 正式窗口不排除 gap、未归属成本、Prefill 或 Decode：总体 WAPE **3.897%**、
P90 **8.252%**、最大误差 **12.158%**，46/60 格误差小于 6%。相比直接沿用正常 base 耗时的
22.740% WAPE 有改善，但未达到总体 3%、P90 5% 的门槛，且所有格仍有 partial 成本覆盖提示。
评分状态为 MODEL_LIMITATION，不能称为精度验收通过或 target 结构完全验证。

| Base | 完成格数 | 完整 E2E WAPE | P90 |
| --- | ---: | ---: | ---: |
| C1 | 12/12 | 4.342% | 8.252% |
| C2 | 12/12 | 3.260% | 6.329% |
| C3 | 12/12 | 4.741% | 9.101% |
| C4 | 12/12 | 4.355% | 7.514% |
| C5 | 12/12 | 2.798% | 5.116% |

最差格为 C3→C2/W3：预测 5.435305 秒，正常实测均值 4.846119 秒，误差 12.158%。
当前结果与逐格证据位于 `full60_current_20260927/normal_http_scores`。
这批结果作为接下来代码瘦身的对照，不作为之后改动的验收替代品，也不因这些误差重新拟合。

随后清理了只供测试调用的标量预取/通信回放及重试观测链，净减少 918 行；实际 DAG 执行、
历史静态回放及有效时序检查保留。当前源码已通过全新目录的 Release/validation 构建及两项
C++ 语义检查、Python Ruff/格式检查和 39 项相关行为检查。尚未重跑瘦身后的完整矩阵。
本次按依赖、采集、建模核心、工作流、评分、文档分层提交，不提交运行资产。

### 已有结果及其范围

以下路径均相对于 `data/modeling_runs/user_workflow_refactor_20260923/`；更早的逐批输出和检查记录见日志。

| 证据 | 结果 | 位置 |
| --- | --- | --- |
| 09-27 普通释放自动补采后 | C3/C4/C5 三格全部 EXECUTED；11.627714/9.959905/8.422972 秒 | `control_capture_group/predictions` |
| 09-26 正式三目标准备 | C3 11.627714 秒，C5 8.422972 秒；C4 失败 | `formal_prepare_audit_20260926` |
| 09-27 C4/C5 回归 | C5 8.422972 秒、partial；C4 缺普通释放成本、HTTP null | `prediction_result_cleanup_20260927` |
| 最近一次 C5 实际预测 | 8.422972 秒、partial；预取等待按需改造后预测值不变 | `demand_prefetch_control_20260927` |
| 历史控制标量隔离验证 | 清空 control_models 后仍为 8.422972 秒、partial | `static_control_isolation_20260927/prediction` |
| Decode 参数可缺省后的代表回归 | C5 8.422972 秒、partial；使用原完整参数结果不变 | `optional_decode_contract_20260927` |
| CPU 校准单次构图 | 与原 CPU 成本文件完全一致，640,098 个区间；不再二次构图 | `single_pass_cpu_20260927` |
| 缺少 Load 的正式组路径 | 执行报告 8 页、16 MiB/页；HTTP null，停止于未声明补采条件 | `demand_service_20260927/group` |
| 当前控制准入后的构模 | 模型及来源报告与此前逐字段一致；不是新的预测运行 | `corrected_control_admission_20260927` |
| 普通释放缺口定位 | ReleaseRegular：1152 tokens、页大小 64，未成功预测 | `release_regular_requirement_20260927` |
| 干净 Release 代表预测 | C1/W2→C5 为 8.422972 秒、partial；完整结果与此前对照一致 | `clean_backend_20260927/c5` |
| NodeScale 公开入口 | KTransformers hook smoke：默认/关闭为 15,226 µs，启用两倍缩放为 30,452 µs；不是模型推理验收 | `clean_backend_20260927/ktransformers_*` |

09-27 在全新目录 `build/modeling/trace_graph-goal-{release,validation}-20260927` 完成 Release/validation 构建，
时序和 HiCache 检查均通过；新 Release 二进制完成上述 C5 代表预测。公开 DAG 入口现可直接用
`--model-config` 启用 NodeScale，无须手写 runner 配置。Python 全量 173 文件 lint/format 通过。
这些结果不证明当前工作树完成全矩阵或 KTransformers 模型推理验收，也没有产生新的 target 精度成绩。
当前代表 base 的 DMA 仍需独立曲线，不能把新增的 base 参数辨识能力当作已证明的域外精度。

普通 DAG 默认路径现已跳过 HiCache 成本观测提取及 control/去 gap 分项回放。
`dag_path_cleanup_20260927` 中真实 C1/W2 前后对照的节点、边、完整仿真及 HTTP 耗时一致；
C1/W2→C5 执行式预测的完整摘要也与上述对照一致。KTransformers hook smoke 的普通路径及 NodeScale 仍通过。
这些是流程等价性证据，不是新的精度验收；历史静态及显式评分路径仍保留分项分析。

### 尚未完成

- 全项目深度清理与代码量目标：正式实现 64,120 行、测试 20,766 行，共 84,886 行，尚未达到 50,000 行上限或减半目标。
- 最小共享补采的完整闭环：I/O 服务及存储新旧页已按实际执行需求触发补采；CPU/phase 前置要求和写入/淘汰分支仍需收敛，真实多缺口闭环未完成。
- 更广的控制分支缺口自动补齐：普通设备释放已完成真实配对补采并恢复预测；写入片段缺口目前仍需显式提供已有独立校准，尚未全部自动处理。
- 未见配置、保留框架能力、中断恢复及整体瘦身后的正式 60-cross 最终验收；不能用本次瘦身前结果替代。

此前设备占用期间没有终止无关进程或重置预算；用户通知释放后，已完成上述共享补采。
后续确需补采时仍重新确认设备状态并串行执行，累计上限按用户授权调整，不再逐次询问。

SGLang、KTransformers、默认关闭的 NodeScale、历史静态 oracle-cost 回放及后续所需资产均保留。
当前执行式预测不支持历史静态回放方式。正式用法见 [modeling 文档](modeling_development.md)；
下方历史成绩按日期保留，不是当前重构版本的统一成绩。此处文档归并不删除任何数据资产。

## 9 月 23 日完整 60 格结果（重构前对照）

本轮已完成三类共享 CPU 校准的采集开销修正：对同一独立固定输入重采完整/轻量配对，在导出加载索引、加载提交和层等待成本前应用同源 CPU 修正，再统一重跑 60 格。没有按 target 误差调参，没有清零未知 gap。

该轮矩阵 `data/modeling_runs/hicache_calibration_cpu_correction_20260923/full60` **60/60 格预测、评分及执行一致性检查完成**。
完整 normal HTTP E2E WAPE **2.9460%**（此前 5.1984%），P90 **6.1857%**（此前 11.2721%），最大 **10.0601%**（此前 15.4552%）。
评分保留 gap、未归属成本和 Prefill/Decode，逐格真值与上一轮完全相同。51 格改善、8 格持平、1 格轻微退步：C1→C4/W3 增加 0.348 ms，误差增加 0.00752 个百分点。

五个 base 的 WAPE 分别为 C1 **3.8739%**、C2 **2.2751%**、C3 **2.7670%**、C4 **3.0508%**、C5 **2.7401%**。
当前最大误差为 C1→C2/W3，另一个明显偏差为 C3→C2/W3（9.3277%）。本轮到此收束，不继续调参。
这只剥离配对测量覆盖的开销，不代表所有采集扰动消失；其他控制校准及部分成本覆盖限制仍保留。
详见 [全部 60 格与前后对照](../data/modeling_runs/hicache_calibration_cpu_correction_20260923/results.md)、[实现与验证日志](tmp/hicache_calibration_cpu_correction_log_20260923.md)。

## 9 月 21 日的结构生成阶段（历史对照）

本阶段目标已完成：目标加载准入、提交、消费者等待、完成确认及分配失败后的淘汰/重试按目标执行状态生成；成本来自各自 base 和共享独立校准，不使用 target trace 补结构或按 cell 调参。

最新统一矩阵 `data/modeling_runs/hicache_semantic_generation_20260921/independent_full60` 已完成 **60/60格预测和评分**。完整 HTTP E2E WAPE **5.1984%**，P90绝对误差 **11.2721%**，最大 **15.4552%**；不排除 gap、未归属成本或 Prefill/Decode。源绑定、状态消费、层调用、分配准备和静态重放检查全部通过，Release代表格与验证构建完整 HTTP 对象一致。

详细结果见 [60格结果表](../data/modeling_runs/hicache_semantic_generation_20260921/independent_full60/results.md)，结构证据、支持范围与成本外推限制见 [验收记录](tmp/hicache_semantic_generation_review_20260921.md)。成本覆盖仍为partial，不宣称精度完备或任意配置均已验证。本阶段不设精度门槛，到此停止，不继续调参。旧 `full60` 的4.5024%使用过混入C5 base的共享 I/O比例且早于加载链路整体替换，仅为历史诊断，不是最终成绩。

## 历史过程记录

09-15 至 09-21 的逐轮分析、失败与运行状态统一见[完整 E2E 开发日志](tmp/hicache_full_e2e_development_log_20260915.md)。
主进展页不再重复这些流水；其中的进程、会话和“正在运行”都只描述当时状态，不能据此恢复任务或重复启动实验。

## 09-14 泛化结果（历史对照）

新 workload / 新 HiCache 配置 / TP=4 初步泛化实验已完成：C5 base × W4/W5 × G1/G2/G3，
TP=2→TP=2 与 TP=4→TP=4 各 6 格，合计 12/12 预测、target 采集和独立评分齐全。
不是跨 TP 扩图；新 workload 的 base profile 提供 source DAG/工作量，但不参与参数拟合。

| 当前新面板 | TP=2 六格 | TP=4 六格 | 总体十二格 |
| --- | ---: | ---: | ---: |
| 组合 scope WAPE | 0.276% | 0.389% | 0.317% |
| Scope p90 / 最大 APE | 0.935% / 0.935% | 1.453% / 1.453% | 0.935% / 1.453% |
| HiCache I/O/控制 WAPE | 12.950% | 16.209% | 14.772% |
| 不抵消 I/O 分项 WAPE | 17.463% | 20.486% | 19.153% |
| HiCache / phase 结构严格一致 | 4/6；5/6 | 4/6；5/6 | 8/12；10/12 |

组合 scope 主目标通过，但排除了 residual gap 和未归属成本，不是完整墙钟 E2E；最终仍为 `MODEL_LIMITATION`。
Prefill compute 总体 WAPE/p90 为 1.662%/8.634%，Decode 为 0.151%/0.476%；Prefill 与 I/O 分项仍未达标。
G2/W4 在两种 TP 均暴露完整前缀匹配边界缺陷（预测 Prefill 0，实际每 rank 128 tokens）；
G3 四格的等待/可见性投影尚未对齐，不能把严格结构失败直接说成预测 DAG 漏了等待。
这两项新问题尚未修复；未因新 target 成绩调参或追加校准，保留原始外推成绩。

本轮完成首次正式分配写回的归属修复、可选 NPU profiler 串行导出、TP4/W5 source 原始数据重导出恢复，
并通过机制测试、代表格与十二格正式原生回归。全部十九份普通 profile 请求成功，最终十二份 target 的
独立 phase/I/O 观测 ready，无无效事实、归属冲突或 token 范围错误。早期失败记录仍保留。
正式结果：`data/modeling_runs/hicache_generalization_20260914/generalization_summary.json`。
详细配置、逐 workload/单格结果、参数边界及后续问题见 [主验证文档第 10 节](validation/hicache_validation.md#10-09-14-新-workload--新配置--tp4-泛化结果)，
过程见 [泛化计划](tmp/hicache_generalization_plan_20260914.md) 与 [泛化日志](tmp/hicache_generalization_log_20260914.md)。

以下为 09-12 历史对照结果；没有在本轮首次分配归属修复后重跑旧 60-cross，不与新面板混算。

本阶段完成验证合同与控制边界修复、新 60-cross、独立评分与五格真实成本回放。
普通预测没有调参、补采或缩小评分范围；主要变化是消除旧验证工具造成的错误归因。
见 [本轮计划](tmp/hicache_gap_excluded_causal_plan_20260912.md) 与 [执行日志](tmp/hicache_gap_excluded_causal_log_20260912.md)。

## 1. 09-12 对照状态

“一个固定小型校准 + 一个 base 的 profiles，预测全部 HiCache target”主流程已经完成逻辑修复并执行新的
5-base/60-cross 验证。软件流程和信息边界可用，但严格模型验收没有全部通过：

- 60/60 预测 READY，60/60 位于当前固定校准记录的 I/O 域；
- 60/60 effect shape 与 phase 结构严格一致；原 40 格差异是 target 漏字段造成；
- 排除 residual CPU gap 的组合 scope：WAPE 0.921%、p90 1.981%，总体及五个 base 均通过；最大单格 APE 2.712%；
- Prefill+Decode combined：WAPE 0.694%、p90 2.316%，通过；phase delta 1.383%，54 个大变化方向全对；
- Prefill 单项 p90 3.436%，略高于 3% 门槛；
- HiCache I/O/控制：WAPE 5.186%、p90 32.518%、delta 8.121%；不抵消分项 WAPE 5.995%，失败；
- 最终状态为 `MODEL_LIMITATION`，不是工作流错误，也不是全 gate PASS。

09-12 对照正式结果：

```text
data/modeling_runs/hicache_gap_excluded_causal_20260912/final_60_evaluation/gate_summary.json
```

## 2. 09-12 逻辑修复

### 本次修复

- 成本回放不再清零正载荷 Prefetch 的 terminal control；删除过时的 outcome-only 分类，保留真实 CPU 工作。
- 控制节点由操作是否存在决定，不因 duration 为零而消失；避免后台 service 意外成为前台前驱。
- target shape 从同一 target trace 的 Prefetch I/O 完成记录传播 visibility 页数，不用默认 0 冒充观测。
- oracle 先检查相同成本回放恒等性，再比较 target 成本；异常保留具体失败字段，不仅输出 READY。

### 保持的模型边界

- 已执行 I/O 与缓存可见收益分开；状态推进与 DAG 共用 service cost，timeout 等待与后台传输分开。
- DMA 使用设备时钟，存储使用函数墙钟代理，后者可能含内部调度等待。
- 三组件组合 scope 保留必要资源/消费者依赖，排除 residual gap 和未归入组件的 wrapper/probe 成本；不是完整墙钟只减 CPU 空白。
- 分项总量、非抵消误差、适用域和环境信息完整保留，不能通过跨组件抵消或隐式倍率过关。

## 3. 09-12 实验流程（非当前用户合同）

```text
一个 base 的 3 个 profiles
        +
共享平台物理校准
        +
共享固定小型校准（page 32/128，各 2 次）
        ↓
观测提取 → 简洁成本模型 → source DAG 状态/工作量变换
        ↓
Prefill/Decode 变换 → 原子 DAG patch → 模拟
        ↓
全部预测完成后，独立打开 target profiles 评分
```

target config 在预测时决定预取、写回、容量等行为；target trace 和 target cost 只在最后评分/显式 oracle 中使用，
不会回写模型。上述历史五个 model build summary 曾记录空的 `target_inputs`、`target_score_inputs`；
现行摘要已移除这些常量声明，隔离依据是实际输入来源和 predictor/evaluator 分离，不是空字段。

## 4. 09-12 验证证据

- Release 和 validation 全量 C++ 构建通过；`hicache_io_logic_check` 覆盖零/正控制耗时、best-effort/timeout 与 oracle 控制保留。
- 15 项 Python 语义回归和 ruff 通过。
- 固定校准两端点各 2 次成功重复，四类 service 的相对范围均小于 4.3%。
- 5 个 base 各 12 个 cross 全部 READY；60 个 cell 都在固定校准记录的域内。
- 每个 base 选择 1 个 cell，执行 1 次相同成本与 5 次真实成本组合；30 次全部通过，五格恒等性全部成立。
- evaluator 新提取 15 个 target DAG，只在全部预测完成后打开，`parameters_or_capture_plan_changed=false`。
- `current_result_audit.json` 逐值核对 60 格 source manifest 和完整 C++ 模型输入不变；36 格只增加零成本控制边界，耗时不变。

旧 Oracle 的 READY 不能证明成本替换有效：它没有检查相同成本回放恒等性，且会清零 Prefetch 控制成本并改变依赖。
旧约 13% 偏差因此撤回为真实调度误差证据。修复后 C5/W1→C4 完整成本回放误差为 0.0059%；
五格中四格低于 0.01%，C4/W2→C3 仍为 1.272%，保留为尚未单独归因的时序投影残差。
effect shape 与工作量比较正确，仍不等于完整 target 时序同构。

当前 5×3 矩阵没有触发 required/partial capacity gate；这一路径只有机制级证据，尚无本矩阵数据级覆盖。

## 5. 数据资产

清理代码不意味着可以清理下列数据。除目录本身外，须保留各 manifest、组声明和模型报告引用的 profile、
forced-token、CPU 配对、独立校准和 target score 资产；不能只保留汇总而删除复现输入。

| 根目录（位于 `data/modeling_runs/`） | 用途与保留要求 |
| --- | --- |
| `user_workflow_refactor_20260923/` | 当前重构的 base 组、校准账本/测量、CPU 服务、代表预测及复现输入 |
| `hicache_calibration_cpu_correction_20260923/` | 采集开销修正的共享依据及重构前完整 60 格稳定对照 |
| `hicache_semantic_generation_20260921/` | 结构生成、独立成本与完整矩阵历史对照 |
| `hicache_full60_rollout_20260921/` | 完整/轻量 base 采集、配对与正常真值等后续仍使用的资产 |
| `hicache_full_e2e_development_20260915/` | 历史诊断、原语和配对成本来源；具体实验见开发日志，不能整目录当缓存删除 |
| `hicache_generalization_20260914/` | 新 workload、配置和 TP 的泛化实验及其输入引用 |
| `hicache_gap_excluded_causal_20260912/` | 历史静态预测、同成本/真实成本回放与评分证据 |
| `hicache_io_logic_repair_20260911/` | 下述历史对照依赖的物理校准、模型及观测 |

`data/profile_runs/`、`data/calibration/` 中被上述结果引用的原始采集继续保留。
旧诊断的取舍需要逐项核实引用，本文不授权批量删除任何数据目录。

09-12 对照根目录：

```text
data/modeling_runs/hicache_gap_excluded_causal_20260912/
```

其中包含：

- 五个 `C*/predictions/`：当前 60 个预测及复现输入；
- `representative/`、`representative_oracle/`：机制代表格和显式成本回放；
- `final_60_evaluation/`：15 个新 target DAG 观测、60 个评分 cell 与正式汇总；
- `identity*/`、`shape*/`、`current_result_audit.json`：根因反例与修复对照。

模型没有重拟合：`data/modeling_runs/hicache_io_logic_repair_20260911/` 中的物理校准、五个 base 的模型、
model build summary、base observations、targets 及其引用的原始 5×3 profiles/固定校准仍是当前必需输入，不能删除。
09-11 预测和更早结果保留为对照，不代替当前验收。

## 6. 09-12 结果的解释边界

该历史阶段可以陈述：

1. 一个固定小型校准加一个 base 的 profiles 能构建并运行所有当前 target 预测；
2. 实际执行 I/O、缓存可见收益、前台等待和后台资源占用已经从概念与实现上分开；
3. 配置决定的 HiCache 工作量/关系在当前矩阵没有不变量差异；
4. 排除 residual gap 的组合 scope 达到本阶段精度要求，但 HiCache I/O/控制分项未达到旧严格门槛；旧调度差异归因已撤回；
5. 当前数字不能称为完整请求 E2E，也不能声称“DAG 完全正确”。

## 7. 09-12 停止边界与当时的后续方向

本轮在看过最终 60-cell 结果后没有修改公式、系数或采集计划，也没有恢复逐 target 补采和 residual correction。
总体及逐 base 组合 scope 目标已通过；HiCache I/O/控制及部分 phase 分项限制保留，不因分项偏差追加经验修正。

当时提出单独研究 target-independent、可复现的调度/资源机制，重点是 best-effort 后台 I/O 如何影响 source
骨架中的资源时序，以及存储函数墙钟能否取得更明确的分解；当时 residual CPU gap 仍 deferred。09-15 的进一步诊断和当前优先级见文首。KTransformers 和 NodeScale
均保留：KTransformers 作为另一目标推理框架，NodeScale 作为默认关闭的可选 what-if，不进入默认 SGLang HiCache 流程。
