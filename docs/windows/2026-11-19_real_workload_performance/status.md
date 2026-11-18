# 真实流量性能预测窗口状态

更新日期为 2026-11-19。S0–S2 已取得阶段交付，S3 进入逐工作实测时间匹配。

Qwen3-8B BF16、单实例 TP1、dummy 权重。开发片段为 Azure 会话 trace 的前 200 条，两段互不重叠的留出各 48 条。两档测速固定并发 8 和给定 100 μs。 [固定输入与留出](../../../data/20261118T172915.294780940Z_real-workload-performance/inputs/workload-selection.json) · [正式来源](../../../data/20261118T172915.294780940Z_real-workload-performance/sources/s1-allocator-coverage/result.json)

S2 的 100/200 条分别耗时 268.16/675.29 秒，较 S1 少 54%/47%，完整结果与基线一致。100 条达到开发目标，200 条比目标多 75 秒，用于阶段复测。全节点实测匹配与独立成本预测待实施。 [基线](../../../data/20261118T172915.294780940Z_real-workload-performance/results/s1-speed.json) · [复测](../../../data/20261118T172915.294780940Z_real-workload-performance/results/s2-speed.json)
