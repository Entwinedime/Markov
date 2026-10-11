# HiCache 验证与当前限制

更新时间：2026-10-11。公式见 [HiCache 数值模型](../hicache_io_cost_model.md)，工作流见
[建模开发说明](../modeling_development.md)。本文只维护当前修复口径的验收事实；内部 JSON 字段
`hicache_direct` 指“HiCache I/O 与相关控制开销”，不是完整 E2E，也不是“只改数值”的结构假设。

10-10 已按用户要求清理过时预测、评分和采集。本文保留历史结论，但旧路径不再保证可用；
最新稳定 60 格、当前实验及仍被引用的测量保留，现存范围见[数据资产](../work_progress.md#数据资产)。

## 当前验收口径

10-10 修正 attention 逐层检查的错误主机等待后，旧 60 格回归全部完成，独立评分在
`hicache_generalization_20261010/corrected_original_cross_evaluation`：WAPE 3.001%、P90 7.640%、最大 APE 9.370%，
60/60 格低于 10%、50/60 格低于 6%。同正常真值、同成本参数的修正前结果为 4.064%/8.862%/9.950%，
39 格改善、21 格不变，无退步；十五份源回放也未变。正式状态仍为 MODEL_LIMITATION，
本批没有重跑旧 target 结构比较，源时钟矛盾和全部依赖未证明的限制仍保留。
这是旧矩阵回归，不代替新增 72 格验证。

修正后的新增 72 格及公开性能评价已完成：WAPE 2.348%、P90 5.196%、最大 APE 8.429%，
72/72 格低于 10%、66/72 格低于 6%。状态仍为 MODEL_LIMITATION，严格门槛未通过。
原 117 份正常测量及十二份源回放报告未变；本批没有重提 target 结构，也没有补采或改参。
这批已用于开发修正，七份代表图的九格比较已完成但仍为 partial，不能证明整图一致。
新留出会话暴露的 source 窗口裁剪及缺失连接编号问题已修正，现行 54/54 格预测全部完成，
状态事实消费完整且无 partial rounds；诊断代表与 Release 的执行摘要及源回放一致。
正常测量按预定波动规则追加九次，留出确认评分已完成：**54/54 格低于 6%**，
完整 HTTP WAPE/P90/最大 APE 为 **1.640%/3.717%/5.587%**，没有成本排除。
总体及三个 base 的性能分组通过原有门槛，评价退出 0；全部 75 份有效样本保留，算术复核无差异。
短超时多轮五次正常耗时为 23.015–26.550 秒，相对均值跨度 14.573%；精度针对重复均值，
不保证单次运行误差，未剔除较慢样本或继续补到分数更好。
同四个 target、三个负载的十二个场景，C5/C1/C2 WAPE 为 1.395%/1.450%/2.061%，真值相同。
独立请求分组未优于 base 墙钟，仍为 MODEL_LIMITATION；成本覆盖全部 partial，确认没有整图对照。
逐 target 的 C4 和 tight-host 分组也未通过严格门槛；tight-host 三格 WAPE 为 4.471%，
照搬 base 墙钟仅为 0.572%。整体与逐 base PASS 不等于所有分组 PASS，低于 6% 也不自动证明方法优于简单基线。
确认结果在 `hicache_generalization_20261010/confirmation/current_evaluation`；现行 72 格预测全部完成且数值未变，
原 60 格最终回归和独立评价已完成：3.001%/7.640%/9.370%，全部低于 10%、逐格与上一轮相同，
源回放数值未变，C1/W1 多保留四个合法尾部节点。证据在 `current_regression/original_evaluation`。
扩展 72 格最终代码评价也已结束，退出 2：2.348%/5.196%/8.429%，全部低于 10%、66 格低于 6%。
逐格预测、真值及完整 HTTP 分数与上一轮完全相同，117 份正常样本保留。
七份 target 的九格结构比较已完成，phase 工作及已比较通信/张量工作对齐，仍为 PARTIAL_COVERAGE；
C1→C4 多轮 matched effect 差异从两项降为零，96/99 未对应项仍保留，其他紧凑工作与依赖统计未变。
现行证据为 `hicache_generalization_20261010/current_regression/expanded_evaluation`；
全 186 格低于 10% 达成本轮泛化目标，不代表原严格门槛、任意配置或完整结构均通过。
分面板结果、采集用量及范围限制已合并到工作进展，实施日志记录收口。

10-08 稳定对照完成 Release/validation 构建和 60/60 格预测，独立评分在
`e2e_accuracy_20261008/full_cross_evaluation`。完整 HTTP WAPE 为 3.351%、P90 为 7.740%、
最大 APE 为 8.483%，60/60 格小于 10%，44/60 格小于 6%。达到本次每格小于 10% 的目标，
但原有严格精度 gate 仍未通过，状态 MODEL_LIMITATION，评价入口按合同返回 2，而非运行失败。
它使用原先预选的 30 份正常测量，逐格真值不变，不排除 gap、未归属成本或 Prefill/Decode。
训练来源隔离、成本覆盖和执行完成状态单列；本次未请求 target 结构比较，不以精度改善证明结构完全正确。
10-04 的修正前对照为 WAPE 5.520%、P90 12.071%、最大 16.211%；
旧 `three_stage_refactor_20261003/stream_order_evaluation` 已在 10-10 清理，仅保留文档中的对照结论。
当前矩阵、差异解释及资产统一查阅[工作进展](../work_progress.md)，中间修正过程保留在 plan/log，不在此重复。

三份报告分别回答以下问题，不能互相代替：

- 源回放：用未校正成本的源图副本，对照同次 profile 的正式窗口；同时给出逐请求延迟及误差，
  防止整段误差抵消。请求延迟不含请求间停顿，不替代正式窗口 E2E。事件覆盖从通道合并后开始，
  单列窗口过滤、因果尾部及上下文；不以图节点数冒充原始 JSON 记录覆盖率。
- 结构比较：分开报告工作量、语义对应、资源和可达关系。原生 worker 按唯一提交关联对应，
  D2H 提交与缓存发布分开；生命周期尾部可以进入结构比较，但不延长 HTTP 窗口。
  未对应的通信内部操作、重复调用身份及目标基础构图缺少的 CPU 跨 rank 关系仍是覆盖限制。
  额外可达端点对不是错误边数，局部一致不代表整图正确。
- 性能评分：预测与正常 HTTP 测量比较，报告 APE/WAPE/P90/最大值及带符号误差。
  成本覆盖 partial 与是否完成执行分别报告；服务量之和不等于关键路径耗时。

`evaluate` 先检查全部选中预测。有未完成格时返回 NOT_READY 和非零退出状态，列出失败原因，
不打开 target、不计算精度、不读取失败格的旧结果。真实 48 格中 16 格未完成的拒绝证据在
`three_stage_refactor_20261003/incomplete_cross_evaluation/gate_summary.json`。
全部预测完成后才进行独立评价，不训练、不补采、不回写模型。目标提取和 CPU 密集比较受
现有 jobs 上限约束，默认一；这不是并行设备 profiling。双格并行/串行一致性证据在 `parallel_structure_review`。

## 历史完整 E2E 对照（09-28）

`data/modeling_runs/e2e_accuracy_20260928/mutation_index/normal_http_scores` 汇总五组 60/60，
使用原 30 份正常 HTTP 测量独立评分，无排除项。WAPE 3.230%、P90 6.717%、最大 9.612%，
53/60 低于 6%；整体仍为 MODEL_LIMITATION，仅 C3 通过组级指标。来源变化、逐组结果和剩余问题见
[工作进展](../work_progress.md#最新完整-60-格)。本次矩阵未启动新采集，不能据此声称所有历史依据的取得成本为零。
历史成绩保留为对照，不代表当前成绩；原始预测/评分已在 10-10 清理，评分没有回写模型或改变采集计划。
本轮边索引优化后，60 份运行摘要及完整 E2E 评分与 `collective_cpu` 一致，
受支持静态回放的运行和模型摘要也一致。精度未改变；按用户要求完成本轮后暂停推进。

## 重构流程复验（09-27）

重构后的 prepare-hicache 已完成五组共 60/60 预测，随后通过 evaluate-hicache --normal-http
对原 30 份正常运行重复测量完成独立评分。当前总体、逐 base 和最大误差统一见
[工作进展](../work_progress.md#当前目标与未完成项)，原始结果在
`data/modeling_runs/user_workflow_refactor_20260923/modular_refactor_20260927/full60/normal_http_scores`。
本次状态为 MODEL_LIMITATION：完整 E2E 门槛未通过，成本覆盖和执行式组件归属仍有限制。
60 份 run_summary 与重构前 `full60_current_20260927` 相同，证明本轮结果保持，不证明误差已经解决。

当前完整 HTTP 评分要求总面板和逐 base 的 WAPE ≤ 3%、单格 APE 的 P90 ≤ 5%，
且 cross 的绝对误差总和优于直接沿用 base 实测墙钟的参照。self 单列，不要求优于自身零误差参照。
缺失或无效窗口计入覆盖失败；gap、未归属成本及 Prefill/Decode 不从评分中剔除。
下述 09-23 成绩是历史运行证据，不能代替当前代码的统一验收。

正常运行重复测量取算术均值，保留样本数和范围；base 墙钟参照也来自正常运行。
P90 采用最近秩，即排序后的第 ceil(0.9 × 格数) 项，不额外偏移一个格子。
WAPE 为逐格绝对时间误差之和除以真实时间之和；APE 为单格绝对误差除以该格真实时间。
两次重复不足以确定完整波动分布；60 格复用 15 个配置/workload 的真值，不是 60 个独立新场景。
成本收益报告仍为 `INCOMPLETE_COST_EVIDENCE`：旧资产首次取得成本和同口径实测对照不完整，
不能仅凭本轮没有重采，就声称首次使用成本很低或已证明预测比逐目标实测更省。

09-27 诊断瘦身后，普通评分只汇总完整 HTTP 和 base-wall 门槛，并标注验收范围；不再输出历史
scope、phase、phase delta、HiCache 分项及结构总 gate。数值公式和 HTTP 阈值没有放宽。
下述历史成绩保留为结论记录，旧报告已按 10-10 清理范围移除；停止生成细分报告不表示这些项目已经通过。
显式历史 oracle 回放仍要求严格结构匹配，保留同成本恒等性和五种成本替换诊断。
实际执行 09-12 资产时发现旧聚合 Prefetch 参数与当前解析器不兼容，回放在 DAG 执行前失败；
本批删除前后均能复现。下文历史成功结果不代表这条旧资产路径现已重新通过。
按用户最新确认，旧结果仅作历史证据，不恢复旧运行规则；保留能力的验收对象改为现行规则下的静态回放。

### 现行静态回放的已验证范围

10-04 使用本轮 validation 二进制复跑下述相容输入，静态 patch 已应用，未校正 HTTP 回放为
7.948477 秒。88 项原成本填回后，时间、图规模和归属汇总的既定恒等指标全部一致。
本轮资产为 `data/modeling_runs/three_stage_refactor_20261003/static_current_rules_replay`；
本轮另完成三种真实 target 成本替换：I/O 为 7.686054 秒，计算为 7.948477 秒，I/O 加阶段全归属
为 7.698675 秒；计算 120/120、阶段全归属 168/168 成本均实际绑定，图规模和归属计数未变。
上述三种替换使用窗口尾部修正前的本轮 validation。修正后另完成阶段全归属替换（7.961098 秒）、
I/O 加计算替换（7.686054 秒）及同成本恒等复验（7.948477 秒）；图规模及归属计数均未变，
同成本的既定八项恒等指标无差异。两批合计覆盖五种替换，但不是五种全部在最终二进制上重跑；
资产分别为同目录中的 phase_owner、direct_phase_compute、identity_after_boundary 运行摘要。

本轮 C3→C5/W1 使用相容的当前规则输入，384 项操作及阶段工作严格匹配、88 项 I/O 逐项绑定。
原成本填回恒等，五种真实 target 成本替换均完成，图规模和归属计数不变，target E2E 未参与成本设置。
本轮根目录为 `data/modeling_runs/user_workflow_refactor_20260923/modular_refactor_20260927`；
证据为 `static_admitted/admission.json`、`static_admitted/oracle_audit.json` 和 `static_final/final_audit.json`。
此前 `static_compatible` 的源成本扰动仅验证敏感性，不能冒充真实 target 成本替换。

该诊断明确采用未校正 profile 对照；它不是正常 HTTP 精度，也不是执行式 60 格的全量结构验收。
部分静态重建 gap 缺少源坐标，仍不能叠加源 CPU 校正；C1→C3 缺 inactive layer 样本的尝试也仍失败。
命令退出 0 但内部 patch blocked 的结果不算通过。具体调用与成本合同见建模说明第 8 节。

## 历史完整 E2E 复验（09-23）

共享加载索引、加载提交、层等待校准先经过独立完整/轻量配对的 CPU 开销修正，再统一重跑 60 cross。
**60/60 格完成预测及正常 HTTP 评分**，不排除 gap、未归属成本或 Prefill/Decode。WAPE **2.9460%**，
P90 单格绝对误差 **6.1857%**，最大 **10.0601%**；同真值对照分别为 5.1984%、11.2721%、15.4552%。
51 格改善、8 格持平、1 格轻微退步，执行一致性检查全部通过。该检查不等于所有成本都已准确归因；
未测量的采集扰动、其他控制校准及成本覆盖告警仍保留。没有用 target 真值回写模型。

当轮逐格报告 `hicache_calibration_cpu_correction_20260923/results.md` 已在 10-10 清理；上述数字是历史结论。
原逐轮日志统一保存于[文档归档](../work_progress.md#文档整理记录09-27)。下方分项章节是 09-14 及更早的历史实验，
其中隔离 gap 的指标和门槛不能直接当作这轮完整 E2E 的结论。

## 历史结构生成与误差归因（09-21 至 09-23）

09-21 的 `hicache_semantic_generation_20260921/independent_full60` 完成 60/60 正常 HTTP 评分：
WAPE 5.1984%、P90 11.2721%、最大 15.4552%。源绑定、状态消费、层调用、分配准备和执行后重放检查通过；
这是一组生成规则及执行一致性证据，不是 target 全图同构。旧 `full60` 的 4.5024% 混入过 C5 base I/O 比例，
不能作为独立输入成绩。原始输入、执行审计和数值见该目录 `inputs.json`、`execution_audit.json`、`full60_results.json`。

09-23 的 `hicache_error_diagnosis_20260923` 保存代表格自身回放和成本敏感性实验。
它定位到新生成层等待、加载提交使用未经同源校正的共享 CPU 成本，而 base 已有操作使用了配对校正。
随后独立校准补齐 full/light 配对，得到上节的精度改善；没有用 target 残差拟合参数。
敏感性置零只能说明影响大小，不代表这些 CPU 工作都可删除；C2/W3 的自身回放偏差也没有被该项完全解释。
后续需分别检查自身基线和配置变化量，不能把 self 误差作为整行补偿。

## 09-14 及更早的分项与泛化实验

09-14 的新 workload / 新配置 / TP=4 十二格实验结果见第 10 节：组合耗时通过，但严格分项与结构仍有失败。
第 1–9 节中的 60-cross 数字来自 09-12 历史对照，不是新面板成绩，也没有在 09-14 首次分配归属修复后重跑。

09-12 使用当时的生产 patch 完成了 60-cross，并重新提取 15 份 target 评分 DAG。该面板数值来自该次执行，
与 09-11 普通预测误差相同；修复的是验证合同和零成本控制边界，没有调参数提高成绩。
60 格的 effect shape 全部严格一致；旧的 1,376 项差异来自 target visibility 页数漏填。
旧 oracle 约 13% 偏差也受回放清零 terminal control、改变依赖的缺陷污染，不能作为真实调度误差证据。
五个代表格均通过相同成本回放恒等性与五种真实成本变体，证据见下文历史资产路径；原日志保存在文档归档。

## 1. 验证对象与信息边界

09-12 对照矩阵为 5 个 HiCache config × 3 个 workload。每个 config 轮流作为 base，用自己的 3 个真实 profiles
预测另外 4 个 config：

```text
5 base × 3 workload × 4 target = 60 cross cells
```

每个 base 模型只使用：

- 自己的 3 个 base profiles；
- 同一份 target-independent 平台物理校准；
- 同一份固定小型校准：page 32/128 两个端点，每端点 2 个成功重复；
- 从自己的 base profiles 得到的 Prefill/Decode 参数。

五个模型的 HiCache I/O/控制参数逐字段相同，Prefill/Decode 参数随 base 不同。历史摘要记录了
`target_inputs=[]`、`target_score_inputs=[]`，但这两个常量字段本身不是隔离证据，现行流程已不再生成。
60 个预测全部完成后，独立 evaluator 才打开 15 个 target
profiles；评分记录 `parameters_or_capture_plan_changed=false`，没有用 target 结果回写参数。

09-12 对照产物：

```text
data/modeling_runs/hicache_gap_excluded_causal_20260912/
  C1_l1_cliff_wait/predictions/ ... C5_writeback_long_gate_timeout/predictions/
  final_60_evaluation/gate_summary.json
  current_result_audit.json
```

## 2. 主目标与分项诊断门槛

该历史阶段的主目标是总体及逐 base 的组合 scope WAPE ≤ 3%、p90 ≤ 5%，争取总体 WAPE ≤ 1%。
下列门槛用于解读历史报告，不是瘦身后普通 evaluator 的输出合同；不要求为了全部通过而强行拟合。

WAPE 是绝对误差总和除以 target 总量；scope/I/O p90 是 cell 级 APE 的第 90 百分位。
Phase 的 WAPE 累计 request/rank 成本误差；phase p90 则先在每格内计算 request/rank APE 的 p90，
再对各格 p90 取 p90，不是每格总 compute APE 的 p90。delta weighted L1 以 source→target 实际变化量的绝对值之和为分母。

| Gate | 门槛 |
| --- | --- |
| Structure | 每格 READY，HiCache I/O/控制与 phase 结构均严格一致 |
| Gap-excluded scope | WAPE ≤ 3%，cell p90 ≤ 5% |
| Prefill、Decode、combined compute | 各自 WAPE ≤ 1.5%，request/rank 二级 p90 ≤ 3% |
| Phase delta | weighted L1 ≤ 2%，所有大变化方向正确 |
| HiCache I/O/控制 | 总量和不抵消分项均要求 WAPE ≤ 3%、cell p90 ≤ 5%，delta ≤ 3% |

“不抵消分项”先分别计算 Prefetch、Load、D2H、H2S 的绝对误差，再相加；不能靠一项多算、另一项少算过关。
这些数值门槛没有因本轮新结果而放宽。

## 3. 60-cross 正式结果

| 项目 | WAPE | p90 | Delta | Gate |
| --- | ---: | ---: | ---: | --- |
| Gap-excluded scope | **0.921%** | **1.981%** | — | PASS |
| Prefill compute | 0.808% | 3.436% | — | p90 未过 3% |
| Decode compute | 0.302% | 0.976% | — | PASS |
| Prefill+Decode combined | **0.694%** | **2.316%** | 1.383% | PASS；54 个大变化方向全对 |
| HiCache I/O/控制总量 | 5.186% | 32.518% | 8.121% | FAIL |
| HiCache I/O/控制不抵消分项 | 5.995% | 32.518% | — | FAIL |

总状态为 `MODEL_LIMITATION`。`scope` 通过不代表各组件通过，也不代表完整请求 wall-clock 通过：它是
HiCache I/O/控制、Prefill 和 Decode 在规范化 DAG 上的组合关键路径，并明确排除 residual CPU gap。
总体及五个 base 均通过 scope 门槛，60 格的单格 scope APE 也全部低于 3%；最大为 2.712%（C2/W2→C3）。

## 4. 结构结果如何解释

09-12 的 60 个预测全部 `READY`、DAG patch validation 通过且位于固定校准记录的 I/O 域内。
从 15 个 target trace 独立重新提取后，60/60 的 operation/page/batch、existing/new、语义关系及比较的 lane 顺序严格一致，
phase 工作量/结构也为 60/60 一致。
原 40 格合计 1,376 项差异来自 target visibility 页数漏填，不是已证实的到达/调度错误；比较字段未删减。

这证明该矩阵的 **effect 工作量与所比较的依赖投影** 一致，不证明预测 DAG 与完整 target DAG 同构，
也不能排除未比较的 CPU/资源调度差异。

当前 5×3 矩阵没有产生一个 `required/partial` 的 capacity-gate effect；相关依赖实现通过了机制检查，但尚未由本矩阵的
真实 target 分支覆盖，不能宣称已经做了数据级泛化验证。

## 5. Oracle-cost replay

旧 25 次回放虽然报告 READY，但没有检查“相同成本填回是否恒等”。09-12 反例证明旧合同会清零正载荷 Prefetch 的
terminal control，删节点后意外把请求前驱接到后台 service。因此旧约 13% 偏差不能用来判断真实调度模型。

当前回放原样注入每个 operation 的 service 与 intrinsic control；控制边界不依赖 duration 是否为零。
先执行相同预测成本回放，确认计时、节点/边计数和 ownership 汇总复现，再执行五种 target 成本变体：
HiCache I/O/控制、phase device、完整 phase owner，以及两种组合。target 变体继续检查结构计数和原有依赖验证。
这些检查仍不是完整 target 图同构证明，oracle 结果也不计入普通预测精度或回写参数。
09-27 瘦身后不再输出或比较逐段关键路径诊断报告；仍计算去 gap 的仿真时长，历史报告不改写。

五格共 30 次回放均 READY（5 次相同成本、25 次 target 成本变体），覆盖等待、超时及 best-effort。
这是显式选取的五个代表格，不是全部 60 格的 oracle。结果保存在本轮根目录 `representative_oracle/summary.json`。

| Cell | 普通预测 scope APE | 完整真实成本回放 scope APE |
| --- | ---: | ---: |
| C1/W1→C2 | 0.803% | 0.0053% |
| C2/W1→C5 | 1.734% | 0.0075% |
| C3/W3→C5 | 0.478% | 0.0092% |
| C4/W2→C3 | 1.822% | 1.2717% |
| C5/W1→C4 | 1.073% | 0.0059% |

原 best-effort 反例从约 13.0% 降为 0.0059%，确认旧偏差主要来自回放清零控制。
C4/W2→C3 仍有 60,758 us 残差，尚未单独归因；phase submit 总成本仍按 source CPU 节点分配，
填入语义成本不是逐节点复制 target 图。保留这一限制，不根据残差追加参数。

## 6. 按 base 的 12-cross

百分数均为 `WAPE / p90`；最后一列为严格结构一致 cell 数。

| Base | Scope | Phase combined | Phase delta | HiCache I/O/控制 | 不抵消分项 | I/O delta | Strict shape |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| C1 | 1.060 / 1.981 | 0.767 / 4.656 | 2.584 | 5.564 / 32.518 | 5.720 / 32.518 | 8.095 | 12/12 |
| C2 | 1.281 / 2.576 | 0.910 / 2.316 | 1.907 | 5.647 / 32.518 | 6.554 / 32.518 | 9.312 | 12/12 |
| C3 | 0.569 / 1.175 | 0.533 / 1.931 | 1.118 | 6.226 / 32.518 | 7.221 / 32.518 | 8.853 | 12/12 |
| C4 | 1.103 / 1.868 | 0.823 / 2.213 | 0.822 | 2.969 / 3.494 | 3.846 / 3.844 | 4.473 | 12/12 |
| C5 | 0.629 / 1.178 | 0.473 / 1.550 | 1.171 | 6.006 / 32.518 | 7.055 / 32.518 | 11.120 | 12/12 |

表内耗时与 Strict shape 均来自 09-12 当时代码重跑及独立评分。五个 base 的 scope 都通过，
但没有一个 base 通过 HiCache I/O/控制的全部 gate。C4 的总量接近 3%，
但不抵消分项和 delta 仍失败。

## 7. 按 I/O 类型

| 类型 | WAPE | p90 | Target 总时间 | 当前含义 |
| --- | ---: | ---: | ---: | --- |
| Prefetch | 6.372% | 32.518% | 77.532 s | 已包含执行但零可见页的后台读取；当前最大绝对误差来源 |
| Load | 6.684% | 14.780% | 6.599 s | 小总量、长尾明显 |
| D2H | 8.949% | 39.733% | 2.722 s | 总量最小，单格相对误差敏感 |
| H2S | 5.409% | 9.444% | 71.298 s | 存储函数墙钟代理仍含合法阻塞与未分离调度 |

固定校准重复的四类 service 相对范围均小于 4.3%，没有高于 10% 的重复波动项；这只能证明同一固定输入可重复，
不能证明当前简化公式已覆盖真实 workload 的所有 batch、缓存冷热和资源竞争状态。

## 8. 与旧口径的关系

2026-09-10 历史结果的 HiCache I/O/控制 WAPE/p90/delta 为 2.971%/10.131%/3.454%。旧口径把“已经执行、
但请求结束时没有可见页”的 Prefetch service 排除；当前口径保留这些真实资源工作，所以 Prefetch target 总时间从约
65.258 s 增至 77.532 s，当前结果不能与旧数字当作同一指标下的单纯精度退化。

旧文档的“60/60 exact”也来自较弱的结构检查。当前只保留一套生产实现和一套当前结果；旧目录仅作为历史参照，
不得用于宣称当前 gate 通过。

## 9. 09-12 当时的限制与停止边界

- 存储 service 仍是函数墙钟代理，不应称作纯磁盘传输时间；不能简单减去 thread CPU 或所有等待。
- 资源 lane 表达必要串行关系，不是完整的 CPU/内存/文件系统竞争模型。
- effect shape 一致不等于完整 target 时序同构；五格完整成本回放仍有一格 1.272% 残差，不能将其直接归因于 cost 系数。
- residual CPU gap 继续 deferred；当前没有稳定、可观测、可干预的因果变量支持建立泛化模型。
- 当前公式在看过最终 60-cell 结果后没有修改，也没有新增 target-specific scale、逐格 correction 或补采循环。

当前阶段以组合 scope 为主验收，分项旧 gate 保留为限制诊断。不根据矩阵残差调参；必要改善必须来自
target-independent 证据支持的物理或调度机制。

## 10. 09-14 新 workload / 新配置 / TP=4 泛化结果

### 10.1 实验与参数边界

实际完成 TP=2→TP=2 与 TP=4→TP=4 各六格：C5 base × 两个新 workload × 三个新 HiCache target。
不是 TP=2→TP=4，也不是五 base 的 60-cross。本轮检验单 base、串行请求、同 TP 环境内的新组合；
不证明跨 TP 扩图、多请求并发或任意配置均可泛化。

| 配置 | Page tokens | Device / host tokens | 写入策略 | 预取门槛 tokens | 停止策略 |
| --- | ---: | ---: | --- | ---: | --- |
| C5 base | 128 | 3584 / 16128 | write_back | 1024 | timeout 2 s |
| G1 | 64 | 4096 / 8192 | write_back | 384 | wait_complete |
| G2 | 128 | 4096 / 8192 | write_through_selective | 384 | best_effort |
| G3 | 32 | 3072 / 9216 | write_back | 768 | timeout 10 ms |

W4 混合 768/1536/2560-token 上下文、共享前缀及缓存回访，输出 8 tokens；W5 使用 1024/2048-token
上下文、局部性轮换及更长 Decode，输出 32 tokens。每个 workload 为 20 个准备请求和 12 个正式请求，
两种 TP 和全部配置使用同一套 forced-token 请求。配置在读取新 target 数据前确定。

TP=2 复用原 C5 参数与同环境校准；TP=4 用自己的 C5/W1–W3 profiles、物理校准及固定小型校准构模。
固定校准仍只有一个逻辑输入，在 page 32/128 各原样重复两次。W4/W5 的 base profiles 提供 source DAG 和工作量，
不参与参数拟合；“新 workload”不表示预测不需要该 workload 的 base profile。
两种 TP 的参数输入审计中，新 base/target 与构模输入均无重叠，target_inputs/target_score_inputs 均为空。
全部十二格预测先完成，随后才采 target 并独立评分；评分新增预测/采集均为 0，没有按新成绩调参。

### 10.2 组合关键路径结果

Scope 包含 HiCache I/O/控制及 Prefill/Decode 的归属计算、提交和必要等待，排除 residual gap 与未归属成本；
不是完整应用墙钟只减 CPU 空白。WAPE 从逐格绝对误差之和重算，不平均两个 TP 的百分比。

| 分组 | 格数 | Scope WAPE | p90 / 最大单格 APE | HiCache 严格结构一致 | Phase 结构一致 |
| --- | ---: | ---: | ---: | ---: | ---: |
| TP=2 | 6 | 0.276% | 0.935% / 0.935% | 4/6 | 5/6 |
| TP=4 | 6 | 0.389% | 1.453% / 1.453% | 4/6 | 5/6 |
| W4，两种 TP | 6 | 0.495% | 1.453% / 1.453% | 4/6 | 4/6 |
| W5，两种 TP | 6 | 0.244% | 0.683% / 0.683% | 4/6 | 6/6 |
| 总体 | 12 | **0.317%** | **0.935% / 1.453%** | **8/12** | **10/12** |

两种结构同时一致为 6/12。十二格均预测 READY、patch applied、topology valid、无 phase owner conflict；
I/O 调用大小均在当前观测域内，但域内不等于精度保证。所有组的 scope gate 通过；总体绝对误差为
317304 us，target scope 总量为 100022541 us。以下保留全部单格 APE，单位 %：

| Target | TP2 / W4 | TP2 / W5 | TP4 / W4 | TP4 / W5 |
| --- | ---: | ---: | ---: | ---: |
| G1 | 0.935 | 0.458 | 1.453 | 0.683 |
| G2 | 0.166 | 0.125 | 0.253 | 0.145 |
| G3 | 0.326 | 0.027 | 0.060 | 0.147 |

### 10.3 分项结果与未通过项

下表百分数为 WAPE / p90；phase p90 使用第 2 节定义的 request/rank 二级统计。

| 项目 | TP=2 | TP=4 | 总体 |
| --- | ---: | ---: | ---: |
| Prefill compute | 1.904 / 8.952 | 1.442 / 5.817 | 1.662 / 8.634 |
| Decode compute | 0.084 / 0.380 | 0.208 / 0.700 | 0.151 / 0.476 |
| Prefill+Decode combined | 0.485 / 2.747 | 0.413 / 1.999 | 0.446 / 2.688 |
| HiCache I/O/控制 | 12.950 / 32.413 | 16.209 / 38.436 | 14.772 / 32.413 |
| HiCache I/O/控制不抵消分项 | 17.463 / 32.434 | 20.486 / 38.436 | 19.153 / 32.434 |

Phase delta 总体为 2.628%（TP2 1.926%、TP4 3.294%），大变化方向 10/12 正确；
HiCache I/O/控制 delta 总体为 65.642%（TP2 52.620%、TP4 77.779%）。两种 TP 与总体均为
`MODEL_LIMITATION`：scope gate 通过，structure、phase、phase_delta、HiCache I/O/控制 gate 未通过。
Prefill+Decode combined 与 Decode 单项通过，不代表 Prefill 单项通过。

HiCache 分类型 WAPE：Prefetch 25.719%、Load 12.377%、D2H 8.014%、H2S 17.119%。
Prefetch 与 H2S 占不抵消分项绝对误差约 97.4%。其成本总和不是组合关键路径时长，可能包含被计算掩盖的后台服务，
不能从小 scope WAPE 推导 I/O 预测准确，也不能只靠调系数解释全部偏差。

已确认或尚需厘清的机制问题：

- **完整前缀命中边界**：两种 TP 的 G2/W4 均在 formal_7 预测 Prefill 0 tokens，实测各 rank 为 128 tokens。
  SGLang 将可匹配长度限制到 input_len−1 后再按页对齐；模型对完整路径重新匹配时缺少该限制。
  应修复缓存匹配/分配状态语义，不能只改最终 Prefill 数字或添加单格分支。其他 W4 格结构一致仍有 Prefill 计时偏差，
  因此这一缺陷不能解释全部 Prefill 误差。
- **短超时等待与可见性投影**：G3 的四格严格 HiCache 结构未通过；等待依赖和缓存可见性在两侧尚未完全对齐。
  模型已生成独立 10 ms policy wait，节点时长被 scope ownership 保留，不能说成预测 DAG 漏了等待。
  target 在零可见页时使用另一条非 canonical 观测路径，两侧等待计时是否等价仍需验证；低耗时误差并不能证明其正确。
  不删除比较字段、不降 gate、不将这些格算作 exact。
- **成本泛化限制**：真实 workload 的 Prefetch/H2S 成本偏差仍大，Prefill 请求级尾部误差也存在。
  本轮没有证明所有偏差的唯一物理原因，未据此增加拟合项、target correction 或新校准。

上述问题是 09-14 的初步泛化发现，尚缺当前执行式实现上的关闭证据。后续先复现，再判断修复范围；
不能直接用旧代码描述断言当前缺陷相同，也不能视为已经解决。保留原预测作对照，修复回归不称为首次盲测。

### 10.4 复现资产与完成边界

```text
data/modeling_runs/hicache_generalization_20260914/
  run_experiment.py                   # 调用公共入口的本轮编排
  tp2_profiling_suite.json / tp4_profiling_suite.json
  targets/                           # 预定三个 HiCache target
  tp4_parameters/model_build_summary.json
  predictions/tp2/ / predictions/tp4/ # 正式十二格，区别于保留的早期失败目录
  tp2_new_targets.json / tp4_new_targets.json
  tp2_evaluation/gate_summary.json / tp4_evaluation/gate_summary.json
  generalization_summary.json        # 逐格、逐 TP、逐 workload、参数来源审计
  experiment_execution.json          # 包含失败尝试；评分退出码 2 表示模型限制
```

实际 19 份普通 profile、4 份固定校准、2 个 token-only workload 及物理校准均完成。
TP4 固定校准使用 927.67 s / 4 次启动 / 56 请求 / 143416 tokens；物理校准为 139.52 s / 2 次启动 /
42916118528 逻辑字节，均未超过预定预算。普通 profile 共 685/685 请求成功，230 个正式请求；
最终十二份 target 均完成独立 DAG/phase/I/O 提取，两卡每通道两 rank、四卡每通道四 rank。
曾发生 TP4/W5 source 导出不完整：保留原始失败，使用原始采集数据串行重导出恢复，没有重发请求。
正式 target 导出均正常完成；采集期间修复首次分配写回归属，并为 NPU profiler 增加可选串行导出。
机制测试和采集过程日志已保存到文档归档；正式十二格证据仍在上述运行资产中。

本次“扩展 workload/配置并实际验证 TP=4”的初步实验已完成，不等于模型全面达标。
没有重跑旧 60-cross、没有做本轮 target-cost oracle replay，也未实现跨 TP 扩图。
