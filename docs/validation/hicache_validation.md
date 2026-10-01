# HiCache 验证与当前限制

更新时间：2026-09-28。公式见 [HiCache 数值模型](../hicache_io_cost_model.md)，工作流见
[建模开发说明](../modeling_development.md)。本文只维护当前修复口径的验收事实；内部 JSON 字段
`hicache_direct` 指“HiCache I/O 与相关控制开销”，不是完整 E2E，也不是“只改数值”的结构假设。

## 最新完整 E2E 复验（09-28）

`data/modeling_runs/e2e_accuracy_20260928/mutation_index/normal_http_scores` 汇总五组 60/60，
使用原 30 份正常 HTTP 测量独立评分，无排除项。WAPE 3.230%、P90 6.717%、最大 9.612%，
53/60 低于 6%；整体仍为 MODEL_LIMITATION，仅 C3 通过组级指标。来源变化、逐组结果和剩余问题见
[工作进展](../work_progress.md#最新完整-60-格)。本次矩阵未启动新采集，不能据此声称所有历史依据的取得成本为零。
旧结果保留为对照，不代表当前成绩；评分没有回写模型或改变采集计划。
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
旧报告和下述历史成绩原样保留；停止生成细分报告不表示这些项目已经通过。
显式历史 oracle 回放仍要求严格结构匹配，保留同成本恒等性和五种成本替换诊断。
实际执行 09-12 资产时发现旧聚合 Prefetch 参数与当前解析器不兼容，回放在 DAG 执行前失败；
本批删除前后均能复现。下文历史成功结果不代表这条旧资产路径现已重新通过。
按用户最新确认，旧结果仅作历史证据，不恢复旧运行规则；保留能力的验收对象改为现行规则下的静态回放。

### 现行静态回放的已验证范围

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

逐格数据及测量边界见 [本轮结果](../../data/modeling_runs/hicache_calibration_cpu_correction_20260923/results.md)。
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
