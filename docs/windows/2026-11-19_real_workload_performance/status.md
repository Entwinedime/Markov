# 真实流量性能预测窗口状态

更新日期为 2026-11-19。S0–S3 已取得阶段交付，S4 开始构造独立成本样本。

Qwen3-8B BF16、单实例 TP1、dummy 权重。开发片段为 Azure 会话 trace 的前 200 条，两段互不重叠的留出各 48 条。两档测速固定并发 8 和给定 100 μs。 [固定输入与留出](../../../data/20261118T172915.294780940Z_real-workload-performance/inputs/workload-selection.json) · [正式来源](../../../data/20261118T172915.294780940Z_real-workload-performance/sources/s1-allocator-coverage/result.json)

S2 的 100/200 条分别耗时 268.16/675.29 秒，较 S1 少 54%/47%，完整结果与基线一致。100 条达到开发目标，200 条比目标多 75 秒，用于阶段复测。 [基线](../../../data/20261118T172915.294780940Z_real-workload-performance/results/s1-speed.json) · [复测](../../../data/20261118T172915.294780940Z_real-workload-performance/results/s2-speed.json)

S3 的两请求开发例完成 150 个动作、4,084 项工作和 5,782 个端点的全量时间对照，同时核对了 683 项有独立服务区间的工作。详细观测和轻量观测在相同负载下的 TPOT 分别为 390.24 和 68.68 毫秒。S6 使用详细观测核对结构，性能以轻量运行测量。CPU 的独立成本资格及留出泛化待验证。 [结构对照](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s3-measured-work/comparison/actions.json) · [测量资格](../../../data/20261118T172915.294780940Z_real-workload-performance/results/s3-service-qualification.json) · [观测对照](../../../data/20261118T172915.294780940Z_real-workload-performance/results/s3-observation-overhead.json)
