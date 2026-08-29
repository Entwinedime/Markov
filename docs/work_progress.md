# 当前工作进展

更新时间：2026-08-29。

详细执行记录维护在：

- `docs/tmp/final_project_semantic_slimming_plan_20260828.md`
- `docs/tmp/final_project_semantic_slimming_log_20260828.md`

本文件只保留当前状态，不保存迭代时间线。

## 目标

项目收敛为：

```text
profile -> source DAG -> HiCache effect -> I/O cost -> DAG patch -> simulation
```

同时保留 KTransformers 的 `profile -> source DAG -> simulation` 路径。当前阶段只评分 `hicache_direct`，不把 gap、prefill、
decode 或 probe snapshot overhead 混入 Direct 误差。

## 已完成

- 公共 shell 入口收敛为 `scripts/profile.sh` 和 `scripts/model.sh`；
- Python workflow 收敛为一次 plan、execute、summary；
- target I/O structure 与 cost model 分离；
- gap/prefill/decode 建立独立 ownership；
- C++ Release/validation 链接边界明确；
- workload 收敛到三个 JSON template，Python probe 默认 snapshot-free；
- KTransformers image、installer、compose、hook、profile dispatch 与 framework-neutral DAG smoke 保留并适配；
- oracle-cost replay 收敛为一个 compact summary；
- calibration reader 不再依赖内部身份、文件摘要或冻结证明；
- compact one-base I/O cost 模型已经替换旧 correction/pressure/contention surface；
- effect planning 只读 calibration-only `io_planning`，duration 只读 `io_cost`；
- P0–P11 已全部执行，包括三组开发 base 回归、五 base 最终 60 cross、source-invariance、
  oracle replay、clean build 和保护资产审计。

## 当前代码量

本轮基线 active product surface 为 76,947 行、359 个 active 文件。P11 最终为：

- 46,422 行；
- 300 个 active 文件；
- 正式流程规范化完成后的基线为 295 个 active 文件 / 43,993 行；
- 最终语义瘦身完成后为 295 个 active 文件 / 43,511 行，净减 482 行；
- Python modeling workflow：7,555 行；Python validation：2,504 行；C++ TraceGraph：22,806 行；
- 不再以文件是否超过某个行数作为验收标准。

## 稳定数据

受保护的主 profile：

```text
data/profile_runs/sglang/20260824_222856_hicache_manual_template_lightweight_dag_replay
```

它包含五配置、三 workload，共 15 个真实 profile cell。当前开发不重新采集 5×3，除非证明现有资产无法被当前代码读取。

当前 compact 输入和五个 one-base model：

```text
data/calibration/hicache_io_qwen3_32b_tp2/calibration_report.json
data/calibration/hicache_io_qwen3_32b_tp2/control_primitives.json
data/modeling_inputs/hicache/C5_observations.json
data/modeling_inputs/hicache/models/C5_writeback_long_gate_timeout/hicache_io_model.json
data/modeling_inputs/hicache/formal_oracle_scores.json
```

其他 base 的 observation/model 位于同一 `data/modeling_inputs/hicache/` 树。旧开发轮次、gate、版本目录和
retention/SHA manifest 不再属于正式输入；当前代码验证必须重新运行关键 cell，不能通过复用旧 row/run summary 宣称通过。

## 稳定数值基线

重构前 one-base 12-cross：

| Base | Direct WAPE | Direct P90 | 同骨架 oracle WAPE | 同骨架 oracle P90 |
| --- | ---: | ---: | ---: | ---: |
| C5 | 1.564% | 4.387% | 0.518% | 2.185% |
| C1 | 1.605% | 4.305% | 0.560% | 2.661% |
| C3 | 0.854% | 4.305% | 0.587% | 1.922% |

这些是回归参考，不要求 P9 简化后逐数值一致。

P9 cost 简化前的 9-cell 基线是 Direct WAPE 1.1735%、p90 APE 3.2016%；同骨架 oracle WAPE 0.3473%。

P9-D 最终重新运行的 C5 -> C3 / W1：

- 384 个 predicted effect，112 个 required cost；
- structure exact、topology valid、cost ready，0 blocker；
- operation/page/byte 与 planning/cost 分离前精确一致；
- Direct predicted service+control：3,311,756 µs；
- target Direct service+control：3,169,507 µs，APE 4.488048%；
- prefill/decode 显式 deferred，target cost/E2E 不参与参数估计。

这是边界回归证据，不能替代 P10 的 12-cell 或 P11 的 60-cell gate。

P10-A 简化后 9-cell 回归：

- 9/9 structure acceptance ready、9/9 cost ready、8/9 diagnostic exact；唯一非 exact cell 的 32 项均为
  gap/prefill/decode 尚未建模时的 arrival-sensitive prefetch consumer relation，invariant mismatch 为 0；
- Direct WAPE 2.1252%、p90 4.3048%、max 4.4880%，通过总体 3%/5% 门禁；
- 同骨架 oracle normalized WAPE 0.6140%、p90 2.3108%、max 4.0222%，488/488 cost effect 有执行响应；
- load、D2H 和 H2S 局部 tail 仍失败，作为 12/60-cell 限制继续观察，不恢复旧 correction。

P10-B 三个 selected-base 的 12-cross 外推已经完成：

| Base | Structure acceptance | Direct WAPE / P90 | Delta weighted L1 | 大变化方向 | 同骨架 WAPE / P90 |
| --- | ---: | ---: | ---: | ---: | ---: |
| C5 | 12/12 | 2.239% / 4.488% | 3.123% | 100% | 0.803% / 3.862% |
| C1 | 12/12 | 1.829% / 4.305% | 2.094% | 100% | 0.783% / 2.936% |
| C3 | 12/12 | 2.002% / 4.305% | 2.200% | 100% | 1.667% / 4.308% |

三组合计 36 个 cross，不含 self：Direct WAPE/P90 为 2.029%/4.488%，delta weighted L1 为 2.444%，33 个大变化
方向 100%；同骨架 normalized WAPE/P90 为 1.075%/4.017%。C3 有六格 arrival-schedule-sensitive 诊断差异，但 Direct
invariant mismatch 为 0，不能在 HiCache direct 阶段通过硬编码 consumer relation 修补。

## 当前最终结果

60 个 cross prediction 全部完成，不含 self。结构结论为 PASS：60/60 usable/shape/oracle binding，
15/15 个 target/workload 组在四个 source 间保持相同语义 I/O 结构。

| Base | Direct WAPE / P90 | Delta weighted L1 | 大变化方向 | 同骨架 WAPE / P90 | 结果 |
| --- | ---: | ---: | ---: | ---: | --- |
| C1 | 1.829% / 4.305% | 2.094% | 100% | 0.783% / 2.936% | PASS |
| C2 | 6.474% / 9.208% | 8.508% | 100% | 0.729% / 3.033% | cost limitation |
| C3 | 2.002% / 4.305% | 2.200% | 100% | 1.667% / 4.308% | PASS |
| C4 | 23.944% / 39.320% | 24.000% | 100% | 9.665% / 21.870% | cost limitation |
| C5 | 2.239% / 4.488% | 3.123% | 100% | 0.803% / 3.862% | PASS |

60-cell 合计 Direct WAPE/P90 为 8.487%/36.857%，delta weighted L1 为 9.906%，同骨架 WAPE/P90
为 3.145%/18.762%。数值结论为 `MODEL_LIMITATION`：C2 没有 D2H/H2S positive service observation，C4 没有
任何 positive-payload service observation，所以单一 base 无法辨识缺失 family 的 runtime scale。这不通过读取
target score 回拟。

## 当前阶段

最终语义瘦身已执行完成。工程门禁全部通过，结构模型通过，数值模型以明确可辨识性限制收束。
当前 60-cell 汇总位于 `data/modeling_runs/formal_workflow_final_60cell_20260829/workflow_summary.json`，与瘦身前汇总逐字节一致。
数据资产从约 223GB 收敛到约 70GB，只保留当前 5×3、forced-token、compact calibration、模型输入、score-only、
KTransformers smoke 和当前正式结果。
后续应先设计一个信息完备的 single-base 观测/独立 runtime calibration，再分别建立 gap、prefill 和 decode
组件；不应回到多 target residual 拟合。

## 最终报告口径

- 60 cross = `5 base × 4 non-self target × 3 workload`；
- 15 self 不是预测矩阵；
- target observation 只用于 structure/cost/oracle score；
- 分别报告 structure、Direct cost 和 isolated E2E；
- 完整 source-to-target E2E 的 phase/gap 残差另行报告，不能归入 Direct cost 失败。
