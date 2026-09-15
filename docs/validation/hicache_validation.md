# HiCache 验证与当前限制

更新时间：2026-09-14。公式见 [HiCache 数值模型](../hicache_io_cost_model.md)，工作流见
[建模开发说明](../modeling_development.md)。本文只维护当前修复口径的验收事实；内部 JSON 字段
`hicache_direct` 指“HiCache I/O 与相关控制开销”，不是完整 E2E，也不是“只改数值”的结构假设。

最新的新 workload / 新配置 / TP=4 十二格泛化实验已完成，结果见第 10 节：组合耗时通过，但严格分项与结构仍有失败。
第 1–9 节中的 60-cross 数字来自 09-12 历史对照，不是新面板成绩，也没有在 09-14 首次分配归属修复后重跑。

09-12 使用当时的生产 patch 完成了 60-cross，并重新提取 15 份 target 评分 DAG。该面板数值来自该次执行，
与 09-11 普通预测误差相同；修复的是验证合同和零成本控制边界，没有调参数提高成绩。
60 格的 effect shape 全部严格一致；旧的 1,376 项差异来自 target visibility 页数漏填。
旧 oracle 约 13% 偏差也受回放清零 terminal control、改变依赖的缺陷污染，不能作为真实调度误差证据。
五个代表格均通过相同成本回放恒等性与五种真实成本变体。过程与证据见 [本轮日志](../tmp/hicache_gap_excluded_causal_log_20260912.md)。

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

五个模型的 HiCache I/O/控制参数逐字段相同，Prefill/Decode 参数随 base 不同。所有模型的
`target_inputs=[]`、`target_score_inputs=[]`。60 个预测全部完成后，独立 evaluator 才打开 15 个 target
profiles；评分记录 `parameters_or_capture_plan_changed=false`，没有用 target 结果回写参数。

09-12 对照产物：

```text
data/modeling_runs/hicache_gap_excluded_causal_20260912/
  C1_l1_cliff_wait/predictions/ ... C5_writeback_long_gate_timeout/predictions/
  final_60_evaluation/gate_summary.json
  current_result_audit.json
```

## 2. 主目标与分项诊断门槛

当前阶段主目标是总体及逐 base 的组合 scope WAPE ≤ 3%、p90 ≤ 5%，争取总体 WAPE ≤ 1%。
下列分项旧门槛继续保留为诊断，不要求为了全部通过而强行拟合；evaluator 的全 gate 状态与主目标是否通过分开报告。

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
先执行相同预测成本回放，确认计时、节点/边计数、ownership 和关键路径完全复现，再执行五种 target 成本变体：
HiCache I/O/控制、phase device、完整 phase owner，以及两种组合。target 变体继续检查结构计数和原有依赖验证。
这些检查仍不是完整 target 图同构证明，oracle 结果也不计入普通预测精度或回写参数。

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

## 9. 当前限制与停止边界

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

上述新缺陷保留为本轮初步泛化发现，尚未修复。后续优先对齐前两项语义，再用代表格独立验证结构与成本，
保留本轮预测作为原始对照，不将看过 target 后的修复回归称为首次盲测成绩。

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
机制测试、代表格及正式十二格回归证据见 [执行日志](../tmp/hicache_generalization_log_20260914.md)。

本次“扩展 workload/配置并实际验证 TP=4”的初步实验已完成，不等于模型全面达标。
没有重跑旧 60-cross、没有做本轮 target-cost oracle replay，也未实现跨 TP 扩图。
