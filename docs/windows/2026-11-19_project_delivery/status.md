# Markov 项目交付总窗口状态

更新日期为 2026-11-19。**S0–S4 已验收。下一阶段是 S5，交付配置集正式入口。**

## 进入窗口时已有证据

- 执行结构已有七款模型、TP/PP/EP、非对称 PD 及具名资源和调度场景的对照。各案例保留原来的独立或开发资格。
- 自主性能闭环覆盖 SGLang/Ascend、Qwen3-8B BF16 dummy、单实例 TP1、页宽 16、KV 容量 32,768、并发 4/8。四个留出组合的等权 TTFT MAPE 为 3.67%，TPOT MAPE 为 4.92%。[独立性能结果](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s7-independent-performance/independent-results.json)
- 配置集示例可以跑给定耗时的结构流程。正式 `predict --costs` 接受单目标和已发布的成本资产。多角色主机采样、轻量评分及执行期间的 CPU 协调和传输计时需要推进。[现有示例](../../../example/coarse_graph/run.py) · [CPU 消息取时](../../../src/markov/controller/cpu_messages.cpp#L46)

## 阶段状态

| 里程碑 | 阶段 | 状态 |
| --- | --- | --- |
| M0 交付基线 | S0 | 已验收，具名场景和本机环境已登记 |
| M1 多角色计时 | S1–S3 | 已验收，TP2 与非对称 PD 均完成开发闭环 |
| M2 配置集探索 | S4–S5 | S4 已通过有界补采、样本复用与联合预测验证，S5 待启动 |
| M3 正式支持范围 | S6–S8 | 待启动，依赖 M2 |
| M4 独立验收与交付 | S9–S12 | 待启动，依赖 M3 |

## 基线、当前能力与下一步

- 八张 910B3 在盘点时均无 NPU 运行进程，盘点时可用磁盘约 1.2 TiB。窗口新增峰值存储上限初定 256 GiB，阶段累计共 106 次原生尝试、120,000 秒原生、117,600 秒后处理额度。S2 首次详细采集以单次 420 秒额度执行。[环境、场景及预算登记](../../../data/20261119T090556.273460Z_project-delivery/inputs/s0-baseline.json)
- S2 从 Qwen3-8B BF16、TP2、页宽 16、KV 容量 4,096 的九请求开发例开始。S3 用 P TP1、D TP2 的结构证据和能触发 KV 交接的短例。历史多卡记录可用于开发，CPU、设备通信及传输成本仍须取得当前测量资格。[TP2 开发输入](../../../data/20261119T090556.273460Z_project-delivery/inputs/s2-tp2-development-capture.json) · [PD 结构证据](../../../data/20261116T035124.971560Z_execution-structure-independent/results/overview.md)
- 真实权重 Dense 选本机 Qwen3-8B，MoE 选已有结构证据的 Qwen3-30B-A3B，权重及原生资格待取得。固定 KT 快照的 ARM 安装要求修改原生 C++，选 vLLM Ascend 为第二框架候选，正式资格由 S8 的无补丁原生请求和闭环验证确立。[官方模型支持](https://docs.vllm.ai/projects/ascend/en/v0.11.0/user_guide/support_matrix/supported_models.html)

S1 已统一动作、CPU 服务和传输查询，单卡三种时间模式及 PP3 的给定时长协议均已走通。三实现域的覆盖验证到达首个未训练工作。历史 PP 原生端点与当前观测合同不符，当前只取得协议给定时长证据。S2 开始采集当前环境的 TP2 工作和通信用时，S3 延续 CPU 协议和传输的测量资格。[S1 回归证据](../../../data/20261119T090556.273460Z_project-delivery/results/s1-regression.json)

S2 已完成 Qwen3-8B TP2 两 rank 的模板、CPU 与设备成本装配。两请求开发例的给定耗时和自主预测均完成 23,950 个区间及七个输出 token。自主预测的 TTFT MAPE 为 8.20%，TPOT MAPE 为 4.13%；两项指标的正式精度资格由留出验证决定。TP1 三请求回归的全部输出时刻与 S1 相同。[双卡成本](../../../data/20261119T090556.273460Z_project-delivery/assets/s2-tp2-costs.json) · [原生对照](../../../data/20261119T090556.273460Z_project-delivery/results/s2-tp2-performance.json) · [通信计时核对](../../../data/20261119T090556.273460Z_project-delivery/results/s2-collective-timeline.json)

S3 的 Prefill TP1、Decode TP2 开发例取得全节点结构对照。三个请求核对 38,500 个工作区间、64,275 个端点和 18 条收件或状态通知。自主预测覆盖三域，完成八个 token，预测吞吐 3.99 token/s，原生测得 4.05 token/s。TTFT MAPE 1.75%，TPOT MAPE 15.08%。该评分用于开发，正式精度走全新留出。组批合并和过滤的测量按相同定义与特征显式复用自 TP2，自动复用和采样需求生产由 S4 交付。[独立结构对照](../../../data/20261119T090556.273460Z_project-delivery/targets/s3-pd-transport-structure/comparison/actions.md) · [三域成本](../../../data/20261119T090556.273460Z_project-delivery/assets/s3-pd-qualified-costs.json) · [原生与预测评分](../../../data/20261119T090556.273460Z_project-delivery/results/s3-pd-performance.json)

S4 从已知需求选择两次 PD 原生轻量采样，累计 277.35 秒。补采与已合格实测复用共同覆盖 PD 三 rank 的 38,500 个已知区间。联合资产还覆盖 TP2 两 rank 的 112,078 个已知区间和两个目标的实际首缺。TP2 与非对称 PD 从初始状态分别输出七和八个 token。S5 需将来源准备、成本获取及所有目标预测纳入一次有预算的用户任务。[补采资产](../../../data/20261119T090556.273460Z_project-delivery/assets/s4-pd-native-costs/costs.json) · [联合资产](../../../data/20261119T090556.273460Z_project-delivery/assets/s4-joint-costs.json)
