# 真实流量性能预测窗口状态

更新日期为 2026-11-19。S0 与 S1 已取得阶段交付，S2 优化依赖维护。

Qwen3-8B BF16、单实例 TP1、dummy 权重。开发片段为 Azure 会话 trace 的前 200 条，两段互不重叠的留出各 48 条。两档测速固定并发 8 和给定 100 μs。 [固定输入与留出](../../../data/20261118T172915.294780940Z_real-workload-performance/inputs/workload-selection.json) · [正式来源](../../../data/20261118T172915.294780940Z_real-workload-performance/sources/s1-allocator-coverage/result.json)

100/200 条分别耗时 585.14/1271.68 秒，完整生成 10483/22007 个 token。依赖历史清理占核心循环 47%/45%，均超出 300/600 秒反馈目标。三条开发请求的明细开关结果逐字节一致。全节点实测匹配与独立成本预测待实施。 [完整基线](../../../data/20261118T172915.294780940Z_real-workload-performance/results/s1-speed.json)
