# 真实流量性能预测窗口状态

更新日期为 2026-11-19。S0–S6 已完成阶段交付。S7 处理独立成本覆盖、请求投递顺序和性能差异归因。

Qwen3-8B BF16、单实例 TP1、dummy 权重。开发片段为 Azure 会话 trace 的前 200 条，两段互不重叠的留出各 48 条。两档测速固定并发 8 和给定 100 μs。 [固定输入与留出](../../../data/20261118T172915.294780940Z_real-workload-performance/inputs/workload-selection.json) · [正式来源](../../../data/20261118T172915.294780940Z_real-workload-performance/sources/s1-allocator-coverage/result.json)

S2 的 100/200 条分别耗时 268.16/675.29 秒，较 S1 少 54%/47%，完整结果与基线一致。100 条达到开发目标，200 条比目标多 75 秒，用于阶段复测。 [基线](../../../data/20261118T172915.294780940Z_real-workload-performance/results/s1-speed.json) · [复测](../../../data/20261118T172915.294780940Z_real-workload-performance/results/s2-speed.json)

S3 的两请求开发例完成 150 个动作、4,084 项工作和 5,782 个端点的全量时间对照，同时核对了 683 项有独立服务区间的工作。详细观测和轻量观测在相同负载下的 TPOT 分别为 390.24 和 68.68 毫秒。S6 使用详细观测核对结构，性能以轻量运行测量。 [结构对照](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s3-measured-work/comparison/actions.json) · [测量资格](../../../data/20261118T172915.294780940Z_real-workload-performance/results/s3-service-qualification.json) · [观测对照](../../../data/20261118T172915.294780940Z_real-workload-performance/results/s3-observation-overhead.json)

S4 汇集 16,694 次设备服务、10,942 次同步搬运和 1,829 次独占 CPU 区间观测，发布 145 项有界模型。两请求开发例的实际需求中，设备覆盖 615/616，搬运覆盖 422/424，CPU 覆盖 77/3,044。其余 CPU 需求主要落在 Attention 和解码层的控制及提交线程。仅开启队列记录时，这两类调用的 216 组队列切点均可对应。 [成本基线及缺口](../../../data/20261118T172915.294780940Z_real-workload-performance/costs/s4/baseline.json) · [队列切点对照](../../../data/20261118T172915.294780940Z_real-workload-performance/sources/s4-cpu-queue-probe/queue-evidence.json)

S5 的新版两请求粗图通过全节点结构对照，覆盖 150 个动作、3,976 项工作和 5,674 个注入端点。结构采集与轻量计时使用同一套原生切点。带负载校准后，两请求及八组代表性来源分别取得全部 2,936 和 44,191 项 CPU 时长。 [结构对照](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s5-direct-submission/comparison/actions.json) · [代表性 CPU 样本](../../../data/20261118T172915.294780940Z_real-workload-performance/costs/s5/scope-features/representative-host.json)

独立预测在无设备和目标真值挂载下完成两请求及 4 个 token。与另一次轻量原生运行比较，TTFT 和 TPOT MAPE 分别为 1.18% 和 0.45%，对应 2 条与 1 条请求。补齐两份模板来源的 CPU 样本后，549 项模型覆盖四份开发图的已见需求。 [开发回归](../../../data/20261118T172915.294780940Z_real-workload-performance/results/s5-two-request-performance.json) · [成本需求](../../../data/20261118T172915.294780940Z_real-workload-performance/costs/s5/scope-features/expanded-costs.json)

并发 4 和 8 各完成 100 条及 10,483 个输出 token，墙钟分别为 345.66 和 270.04 秒，成本查询占 0.82% 和 0.65%。并发 8 从相同提交边界计算开发 TTFT、TPOT MAPE，分别为 3.93% 和 4.65%。原生实测 60.22 token/s，预测 57.84 token/s。 [开发收口](../../../data/20261118T172915.294780940Z_real-workload-performance/results/s5-development-closure.json) · [成本资产](../../../data/20261118T172915.294780940Z_real-workload-performance/costs/s5/data-port-geometry/assignment.json)

S6 以 `759fe37` 为冻结基线。四组均通过全节点结构对照。三组自主预测在同一长分配模型的范围上限停止，冻结样本最多覆盖 7,207 token。 [冻结清单](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/freeze.json) · [矩阵汇总](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/heldout-results.json)

| 组合 | 全节点结构对照 | 自主预测 | 轻量实测及误差 |
| --- | --- | --- | --- |
| 常态并发 8 | [通过](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/normal/concurrency-8/structure-evaluated/comparison/actions.md) | 7,419 token 分配超出范围 | [正式窗口内加载 Triton 特化](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/normal/concurrency-8/latency-metrics.json) |
| 常态并发 4 | [通过](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/normal/concurrency-4/structure-evaluated/comparison/actions.md) | 7,404 token 分配超出范围 | [稳态实测合格](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/normal/concurrency-4/latency-metrics.json) |
| 突发并发 8 | [通过](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/burst/concurrency-8/structure/comparison/actions.md) | 7,850 token 分配超出范围 | [稳态实测合格](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/burst/concurrency-8/latency-metrics.json) |
| 突发并发 4 | [通过](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/burst/concurrency-4/structure-evaluated/comparison/actions.md) | 完成 48 条、5,344 token | [TTFT MAPE 5.19%，TPOT MAPE 8.22%](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/burst/concurrency-4/latency-metrics.json) |

冻结口径下只取得突发并发 4 的预测评分，四组合精度和预测排序尚未取得。突发流量的两档单次实测分别显示并发 8 降低 P95 TTFT、提高吞吐，并发 4 缩短平均 TPOT。四组实际提交最晚偏差低于 2 毫秒，其中三组仍出现请求顺序互换。 [矩阵指标及投递记录](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/heldout-results.json)
