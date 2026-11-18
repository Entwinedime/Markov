# 真实流量性能预测窗口状态

更新日期为 2026-11-19。S0 真实流量准备和小规模结构试跑已完成，下一步为 S1 的 100/200 条墙钟基线。

起点为 `7315c0c`。本窗口复用已发布的 Qwen3-8B BF16 单实例 TP1 模板及相符镜像，使用 dummy 权重域。开发片段为 Azure 会话 trace 的前 200 条，两段互不重叠的留出各 48 条。 [固定输入与留出](../../../data/20261118T172915.294780940Z_real-workload-performance/inputs/workload-selection.json) · [运行资产与预算](../../../data/20261118T172915.294780940Z_real-workload-performance/inputs/runtime.json)

3 条真实请求已用给定耗时完成结构展开。100/200 条默认路径墙钟、全节点实测匹配与独立成本预测均待取得证据。 [开发试跑结果](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s0-development-3/result.json)
