# Markov 项目交付总窗口状态

更新日期为 2026-11-19。**S0 已验收，下一阶段为 S1 多角色计时。** 窗口以 `59618c2` 为代码起点，阶段接续以包含 S0 基线记录的开发分支提交为准。

## 进入窗口时已有证据

- 执行结构已有七款模型、TP/PP/EP、非对称 PD 及具名资源和调度场景的对照。各案例保留原来的独立或开发资格。
- 自主性能闭环覆盖 SGLang/Ascend、Qwen3-8B BF16 dummy、单实例 TP1、页宽 16、KV 容量 32,768、并发 4/8。四个留出组合的等权 TTFT MAPE 为 3.67%，TPOT MAPE 为 4.92%。[独立性能结果](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s7-independent-performance/independent-results.json)
- 配置集示例可以跑给定耗时的结构流程。正式 `predict --costs` 接受单目标和已发布的成本资产。多角色主机采样、轻量评分及执行期间的 CPU 协调和传输计时需要推进。[现有示例](../../../example/coarse_graph/run.py) · [CPU 消息取时](../../../src/markov/controller/cpu_messages.cpp#L46)

## 阶段状态

| 里程碑 | 阶段 | 状态 |
| --- | --- | --- |
| M0 交付基线 | S0 | 已验收，具名场景和本机环境已登记 |
| M1 多角色计时 | S1–S3 | S1 待启动，S2–S3 已明确开发来源 |
| M2 配置集探索 | S4–S5 | 待启动，依赖 M1 |
| M3 正式支持范围 | S6–S8 | 待启动，依赖 M2 |
| M4 独立验收与交付 | S9–S12 | 待启动，依赖 M3 |

## S0 交付与下一步

- 八张 910B3 在盘点时均无 NPU 运行进程，盘点时可用磁盘约 1.2 TiB。窗口新增峰值存储上限初定 256 GiB，阶段累计共 106 次原生尝试、120,000 秒原生、117,600 秒后处理额度。当前原生尝试为零。[环境、场景及预算登记](../../../data/20261119T090556.273460Z_project-delivery/inputs/s0-baseline.json)
- S2 从 Qwen3-8B BF16、TP2、页宽 16、KV 容量 4,096 的九请求开发例开始。S3 用 P TP1、D TP2 的结构证据和能触发 KV 交接的短例。历史多卡记录可用于开发，CPU、设备通信及传输成本仍须取得当前测量资格。[TP2 开发输入](../../../data/20261119T090556.273460Z_project-delivery/inputs/s2-tp2-development-capture.json) · [PD 结构证据](../../../data/20261116T035124.971560Z_execution-structure-independent/results/overview.md)
- 真实权重 Dense 选本机 Qwen3-8B，MoE 选已有结构证据的 Qwen3-30B-A3B，权重及原生资格待取得。固定 KT 快照的 ARM 安装要求修改原生 C++，选 vLLM Ascend 为第二框架候选，正式资格由 S8 的无补丁原生请求和闭环验证确立。[官方模型支持](https://docs.vllm.ai/projects/ascend/en/v0.11.0/user_guide/support_matrix/supported_models.html)

S1 先统一动作、CPU 协调和 CPU 传输的取时职责，处理多个实现域的资产装配。S1 复用单卡和具名多卡历史输入做限定开发回归。S9 固定新留出和最终验收清单。
