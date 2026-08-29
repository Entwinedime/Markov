# HiCache Direct 验收

本文件只定义当前 Direct structure/cost 验收口径。命令和产物见 `docs/modeling_development.md`。

## 1. 被验收的预测

```text
source profile + target HiCache config + calibration/selected base
  -> predicted target effect plan
  -> predicted Direct cost
  -> patched source DAG
  -> simulated hicache_direct timing
```

不验收 gap、prefill、decode 或 probe snapshot overhead。

## 2. Structure gate

每个 cell 必须满足：

- effect plan 可生成；
- effect identity 与 operation shape 可比较；
- operation/page/byte count 完整；
- source carrier attribution ready；
- patch validation ready；
- topology valid；
- target E2E 未被消费。

target profile 中的 I/O structure 只用于 score。预测结构必须由 source facts + target config 生成。

## 3. Cost gate

每个 required effect 必须有：

- direction 与 resource lane；
- operation/page/byte features；
- service/control duration；
- parameter provenance 属于 calibration + selected base；
- 无 target score parameter。

当前 cost 已收口为整体可解释模型：共享 calibration 提供物理曲线，一个 selected base 只估计
family runtime scale 和少量 intrinsic control 系数。

## 4. Oracle gate

oracle-cost replay 使用同一个预测 structure，替换逐 effect Direct cost。必须满足：

- supplied/required/applied effect identity exact；
- operation shape exact；
- patch/topology 仍 ready；
- 每个 cost node completion 对零成本对照产生响应；
- foreground blocking effect 有 consumer endpoint；
- target E2E 未输入。

oracle 不是 target DAG replay，也不是正式 cost prediction。

## 5. Isolated E2E

定义：

```text
absolute_error_us = abs(model_replay_e2e_us - oracle_replay_e2e_us)
reference_us = target_direct_service_us + target_direct_control_us
normalized_error_pct = 100 * absolute_error_us / reference_us
```

model 与 oracle 使用相同 source phase skeleton、target effect plan、dependency 和 simulator，因此差分排除了固定的
prefill/decode/gap。reference 不使用完整 target E2E。

正式 12-cell gate：

- WAPE ≤ 3%；
- P90 normalized error ≤ 5%。

`--max-predictions` 的部分结果只能报告 PARTIAL。

## 6. 重构前参考

| Base | 12-cell Direct WAPE | Direct P90 | oracle WAPE | oracle P90 |
| --- | ---: | ---: | ---: | ---: |
| C5 | 1.564% | 4.387% | 0.518% | 2.185% |
| C1 | 1.605% | 4.305% | 0.560% | 2.661% |
| C3 | 0.854% | 4.305% | 0.587% | 1.922% |

旧模型最好分数不是 P9 必须逐点恢复的目标。简化后允许精度小幅下降，但必须满足最终接受范围。

## 7. 当前工作树 12-cell 验收

| Base | Direct WAPE | Direct P90 | Delta weighted L1 | Oracle WAPE | Oracle P90 | Structure |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| C5 | 2.239% | 4.488% | 3.123% | 0.803% | 3.862% | 12/12 |
| C1 | 1.829% | 4.305% | 2.094% | 0.783% | 2.936% | 12/12 |
| C3 | 2.002% | 4.305% | 2.200% | 1.667% | 4.308% | 12/12 |

三组大变化方向均为 100%。C3 的六个非 diagnostic-exact cell 只有 arrival-schedule-sensitive consumer relation 差异，
Direct operation identity、方向、pages/bytes 和 cost binding 均通过。局部 load control、D2H、部分 H2S/prefetch tail 失败继续
作为模型限制报告，不恢复 target-driven correction。

## 8. 最终矩阵

```text
5 selected base × 4 non-self target × 3 workload = 60 cross
```

五轮使用同一参数估计方法，每轮只换 calibration/base input。60 target score 只用于最终评分。

最终报告至少包含：

- 60/60 planned、usable、structure ready、topology valid、cost ready；
- 15 个 target/workload source-invariance 组；
- Direct WAPE/P90/max；
- isolated E2E WAPE/P90/max；
- 按 operation family 的误差；
- gap/prefill/decode deferred 声明。

### 8.1 2026-08-28 正式流程最终结果

| Base | Structure | Direct WAPE / P90 | Delta L1 | >50 ms 方向 | Oracle WAPE / P90 | 结果 |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| C1 | 12/12 | 1.829% / 4.305% | 2.094% | 100% | 0.783% / 2.936% | PASS |
| C2 | 12/12 | 6.474% / 9.208% | 8.508% | 100% | 0.729% / 3.033% | cost limitation |
| C3 | 12/12 | 2.002% / 4.305% | 2.200% | 100% | 1.667% / 4.308% | PASS |
| C4 | 12/12 | 23.944% / 39.320% | 24.000% | 100% | 9.665% / 21.870% | cost limitation |
| C5 | 12/12 | 2.239% / 4.488% | 3.123% | 100% | 0.803% / 3.862% | timing PASS；strict delta 超 0.123pp |
| 60 cross | 60/60 | 8.487% / 36.857% | 9.906% | 100% | 3.145% / 18.762% | MODEL_LIMITATION |

60/60 均为 non-self prediction，target parameter cell 为 0，target E2E 未被消费。15 个 target/workload 组中，
每组四个 source 的 predicted effect family/state/direction/operation/page/byte/H2S-residency multiset 完全一致。
因此结构验收为 PASS。

C2 只能从自身三个 workload 观测 prefetch/load service，C4 只观测到 zero-payload prefetch control。
未出现的 family 保持 calibration scale 1.0，而不从 target label 补齐。这证明当前数值失败是
single-base observation 不完备导致的 cost identifiability limitation，不是 target I/O 结构预测错误。

本轮统一 evaluator 产物位于
`data/modeling_runs/formal_workflow_final_60cell_20260829/workflow_summary.json`。C1/C3 通过全部 strict Direct gate；
C5 的 WAPE/p90 通过，delta L1 为 3.123%，比 3% strict gate 高 0.123 个百分点。C2/C4 保留
`MODEL_LIMITATION`，未为评分恢复 target-driven correction。
