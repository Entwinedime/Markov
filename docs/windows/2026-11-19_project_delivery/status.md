# Markov 项目交付总窗口状态

更新日期为 2026-11-19。**S0–S5 已验收。下一阶段是 S6 的单机多卡扩展。**

## 进入窗口时已有证据

- 执行结构已有七款模型、TP/PP/EP、非对称 PD 及具名资源和调度场景的对照。各案例保留原来的独立或开发资格。
- 自主性能闭环覆盖 SGLang/Ascend、Qwen3-8B BF16 dummy、单实例 TP1、页宽 16、KV 容量 32,768、并发 4/8。四个留出组合的等权 TTFT MAPE 为 3.67%，TPOT MAPE 为 4.92%。[独立性能结果](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s7-independent-performance/independent-results.json)
- 窗口起点的配置集示例使用给定耗时。正式 `predict --costs` 已有单目标能力，配置集自主预测及多角色采样需要推进。

## 阶段状态

| 里程碑 | 阶段 | 状态 |
| --- | --- | --- |
| M0 交付基线 | S0 | 已验收，具名场景和本机环境已登记 |
| M1 多角色计时 | S1–S3 | 已验收，TP2 与非对称 PD 均完成开发闭环 |
| M2 配置集探索 | S4–S5 | 已验收，冷资产创建与换请求后复用均完成 |
| M3 正式支持范围 | S6–S8 | S6 接续单机多卡，S7–S8 待推进 |
| M4 独立验收与交付 | S9–S12 | 待启动，依赖 M3 |

## 基线、当前能力与下一步

- 八张 910B3 在盘点时均无 NPU 运行进程，盘点时可用磁盘约 1.2 TiB。窗口新增峰值存储上限初定 256 GiB，阶段累计共 106 次原生尝试、120,000 秒原生、117,600 秒后处理额度。S2 首次详细采集以单次 420 秒额度执行。[环境、场景及预算登记](../../../data/20261119T090556.273460Z_project-delivery/inputs/s0-baseline.json)
- S2 从 Qwen3-8B BF16、TP2、页宽 16、KV 容量 4,096 的九请求开发例开始。S3 用 P TP1、D TP2 的结构证据和能触发 KV 交接的短例。历史多卡记录可用于开发，CPU、设备通信及传输成本仍须取得当前测量资格。[TP2 开发输入](../../../data/20261119T090556.273460Z_project-delivery/inputs/s2-tp2-development-capture.json) · [PD 结构证据](../../../data/20261116T035124.971560Z_execution-structure-independent/results/overview.md)
- 真实权重 Dense 选本机 Qwen3-8B，MoE 选已有结构证据的 Qwen3-30B-A3B，权重及原生资格待取得。固定 KT 快照的 ARM 安装要求修改原生 C++，选 vLLM Ascend 为第二框架候选，正式资格由 S8 的无补丁原生请求和闭环验证确立。[官方模型支持](https://docs.vllm.ai/projects/ascend/en/v0.11.0/user_guide/support_matrix/supported_models.html)

S1 已统一动作、CPU 服务和传输查询，单卡三种时间模式及 PP3 的给定时长协议均已走通。三实现域的覆盖验证到达首个未训练工作。历史 PP 原生端点与当前观测合同不符，当前只取得协议给定时长证据。S2 开始采集当前环境的 TP2 工作和通信用时，S3 延续 CPU 协议和传输的测量资格。[S1 回归证据](../../../data/20261119T090556.273460Z_project-delivery/results/s1-regression.json)

S2 已完成 Qwen3-8B TP2 两 rank 的模板、CPU 与设备成本装配。两请求开发例的给定耗时和自主预测均完成 23,950 个区间及七个输出 token。自主预测的 TTFT MAPE 为 8.20%，TPOT MAPE 为 4.13%；两项指标的正式精度资格由留出验证决定。TP1 三请求回归的全部输出时刻与 S1 相同。[双卡成本](../../../data/20261119T090556.273460Z_project-delivery/assets/s2-tp2-costs.json) · [原生对照](../../../data/20261119T090556.273460Z_project-delivery/results/s2-tp2-performance.json) · [通信计时核对](../../../data/20261119T090556.273460Z_project-delivery/results/s2-collective-timeline.json)

S3 的 Prefill TP1、Decode TP2 开发例取得全节点结构对照。三个请求核对 38,500 个工作区间、64,275 个端点和 18 条收件或状态通知。自主预测覆盖三域，完成八个 token，预测吞吐 3.99 token/s，原生测得 4.05 token/s。TTFT MAPE 1.75%，TPOT MAPE 15.08%。该评分用于开发，正式精度走全新留出。组批合并和过滤的测量按相同定义与特征显式复用自 TP2，自动复用和采样需求生产由 S4 交付。[独立结构对照](../../../data/20261119T090556.273460Z_project-delivery/targets/s3-pd-transport-structure/comparison/actions.md) · [三域成本](../../../data/20261119T090556.273460Z_project-delivery/assets/s3-pd-qualified-costs.json) · [原生与预测评分](../../../data/20261119T090556.273460Z_project-delivery/results/s3-pd-performance.json)

S4 的联合成本资产覆盖 PD、TP2 共五域的 150,578 个已知区间及两个实际首缺。两个目标从初态各输出七和八个 token。[联合资产](../../../data/20261119T090556.273460Z_project-delivery/assets/s4-joint-costs.json)

S5 正式入口用两个单卡池容量与两组短请求完成四项自主预测。冷任务共五次原生采集、625.76 秒。新请求 ID 和到达时间复用同一模板和成本，采集与后处理均为零，四项目标全部完成。S6 按更高 TP、PP 微批、EP 及组合核对多角色工作和成本资格。[冷启动结果](../../../data/20261119T122453.716218Z_exploration/summary.md) · [纯复用结果](../../../data/20261119T124125.641276Z_exploration/summary.md)
