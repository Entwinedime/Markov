# Markov 项目交付总窗口状态

更新日期为 2026-11-20。**S0–S6 已验收。单机代表拓扑完成开发验证，TP1 同一用例跑通结构旁路、固定图和自主预测。下一阶段为 S7 调度与缓存。**

## 进入窗口时已有证据

- 执行结构已有七款模型、TP/PP/EP、非对称 PD 及具名资源和调度场景的对照。全节点独立证据包括 Qwen3-8B BF16 dummy 单实例 TP1 的常态与突发流量、并发 4/8 四组，合计 149,422 次调用和 5,964,982 项工作。各场景按实际覆盖及独立性保留原结论。[结构留出结果](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/heldout-results.json)
- 自主性能闭环覆盖 SGLang/Ascend、Qwen3-8B BF16 dummy、单实例 TP1、页宽 16、KV 容量 32,768、并发 4/8。四个留出组合的等权 TTFT MAPE 为 3.67%，TPOT MAPE 为 4.92%。[独立性能结果](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s7-independent-performance/independent-results.json)
- 窗口起点的配置集示例使用给定耗时。正式 `predict --costs` 已有单目标能力，配置集自主预测及多角色采样需要推进。

## 阶段状态

| 里程碑 | 阶段 | 状态 |
| --- | --- | --- |
| M0 交付基线 | S0 | 已验收，具名场景和本机环境已登记 |
| M1 多角色计时 | S1–S3 | 已验收，TP2 与非对称 PD 均完成开发闭环 |
| M2 配置集探索 | S4–S5 | 已验收，冷资产创建与换请求后复用均完成 |
| M3 正式支持范围 | S6–S7 | S6 已验收，实际 rank 成本绑定和单机拓扑完成开发验证，TP1 的三条正式验证路径均有同例证据。S7 待启动 |
| M4 独立验收与交付 | S9–S12 | 待启动，S7 交付后进入 S9 |

## 基线、当前能力与下一步

- 八张 910B3 在盘点时均无 NPU 运行进程，盘点时可用磁盘约 1.2 TiB。窗口新增峰值存储上限初定 256 GiB，阶段累计共 106 次原生尝试、120,000 秒原生、117,600 秒后处理额度。S2 首次详细采集以单次 420 秒额度执行。[环境、场景及预算登记](../../../data/20261119T090556.273460Z_project-delivery/inputs/s0-baseline.json)
- S2 从 Qwen3-8B BF16、TP2、页宽 16、KV 容量 4,096 的九请求开发例开始。S3 用 P TP1、D TP2 的结构证据和能触发 KV 交接的短例。历史多卡记录可用于开发，CPU、设备通信及传输成本仍须取得当前测量资格。[TP2 开发输入](../../../data/20261119T090556.273460Z_project-delivery/inputs/s2-tp2-development-capture.json) · [PD 结构证据](../../../data/20261116T035124.971560Z_execution-structure-independent/results/overview.md)
- S7 按固定输出与给定专家路由的 workload 验证调度和缓存。S0 登记的 Qwen3-8B 和 Qwen3-30B-A3B 真实权重案例转为可选兼容性试跑。S8 暂缓，保留 vLLM Ascend 的候选依据。[候选框架的模型支持记录](https://docs.vllm.ai/projects/ascend/en/v0.11.0/user_guide/support_matrix/supported_models.html)

S1 已统一动作、CPU 服务和传输查询，单卡三种时间模式及 PP3 的给定时长协议均已走通。三实现域的覆盖验证到达首个未训练工作。历史 PP 原生端点与当前观测合同不符，当前只取得协议给定时长证据。S2 开始采集当前环境的 TP2 工作和通信用时，S3 延续 CPU 协议和传输的测量资格。[S1 回归证据](../../../data/20261119T090556.273460Z_project-delivery/results/s1-regression.json)

S2 已完成 Qwen3-8B TP2 两 rank 的模板、CPU 与设备成本装配。两请求开发例的给定耗时和自主预测均完成 23,950 个区间及七个输出 token。自主预测的 TTFT MAPE 为 8.20%，TPOT MAPE 为 4.13%；两项指标的正式精度资格由留出验证决定。TP1 三请求回归的全部输出时刻与 S1 相同。[双卡成本](../../../data/20261119T090556.273460Z_project-delivery/assets/s2-tp2-costs.json) · [原生对照](../../../data/20261119T090556.273460Z_project-delivery/results/s2-tp2-performance.json) · [通信计时核对](../../../data/20261119T090556.273460Z_project-delivery/results/s2-collective-timeline.json)

S3 的 Prefill TP1、Decode TP2 三请求例取得同源开发结构对照，核对 38,500 个工作区间、64,275 个端点和 18 条收件或状态通知。自主预测覆盖三域，完成八个 token，预测吞吐 3.99 token/s，原生测得 4.05 token/s。TTFT MAPE 1.75%，TPOT MAPE 15.08%。该评分用于开发，正式精度走全新留出。组批合并和过滤的测量按相同定义与特征显式复用自 TP2，自动复用和采样需求生产由 S4 交付。[开发结构对照](../../../data/20261119T090556.273460Z_project-delivery/targets/s3-pd-transport-structure/comparison/actions.md) · [三域成本](../../../data/20261119T090556.273460Z_project-delivery/assets/s3-pd-qualified-costs.json) · [原生与预测评分](../../../data/20261119T090556.273460Z_project-delivery/results/s3-pd-performance.json)

S4 的联合成本资产覆盖 PD、TP2 共五域的 150,578 个已知区间及两个实际首缺。两个目标从初态各输出七和八个 token。[联合资产](../../../data/20261119T090556.273460Z_project-delivery/assets/s4-joint-costs.json)

S5 正式入口用两个单卡池容量与两组短请求完成四项自主预测。冷任务共五次原生采集、625.76 秒。新请求 ID 和到达时间复用同一模板和成本，采集与后处理均为零，四项目标全部完成。S6 按更高 TP、PP 微批、EP 及组合核对多角色工作和成本资格。[冷启动结果](../../../data/20261119T122453.716218Z_exploration/summary.md) · [纯复用结果](../../../data/20261119T124125.641276Z_exploration/summary.md)

## S6 交付证据

- TP8 沿用原模板、workload 和成本资产，实际参与者各自绑定成本域。两组完整输出计数及工作区间数量一致，TTFT MAPE 由 14.46%、20.15% 降为 0.28%、6.80%，TPOT MAPE 变为 13.19%、1.67%。均为开发结果，仍需定位剩余误差。[单请求评分](../../../data/20261119T090556.273460Z_project-delivery/work/s6-worker-cost-binding/tp8-singular/score.json) · [并发评分](../../../data/20261119T090556.273460Z_project-delivery/work/s6-worker-cost-binding/tp8-concurrent/score.json)
- TP1 同例结构旁路使用另一轮原生执行的模板，全节点对照覆盖 150 个动作、8,712 项工作和 12,858 个动作内端点。自主预测使用独立成本，完成同样的工作数、两条请求及四个 token。[结构旁路](../../../data/20261119T090556.273460Z_project-delivery/work/s6-scheduler-reference/verification-inputs/structure-timed/result.json) · [自主预测](../../../data/20261119T090556.273460Z_project-delivery/work/s6-scheduler-reference/verification-inputs/autonomous/result.json)
- 补采规划、样本检查和图成本查询共用 C++23 求值。TP1 的 3,976 项工作两路耗时完全一致。PP3 旧来源的零原生额度回归判定 29,729 项工作已覆盖，保留旧长度缺口。[拟合与需求回归](../../../data/20261119T090556.273460Z_project-delivery/work/s6-shared-cost-query/fitted-costs.json) · [补采回归](../../../data/20261119T090556.273460Z_project-delivery/work/s6-shared-cost-acquisition/costs.json)
- PP3 长请求补采覆盖 571 个主机特征点，三请求自主预测输出九个 token。相同请求预热的原生对照未观察到新特化，TTFT MAPE 0.31%，TPOT MAPE 1.66%，用于开发回归。[PP3 评分](../../../data/20261119T090556.273460Z_project-delivery/results/s6-pp3-warmed-performance.json)
- TP2×PP3 的两份留存来源共 12 个 worker 图通过建库。用重建成本完成 64,680 项工作、9 个 token，开发评分的 TTFT/TPOT MAPE 为 5.49%/6.50%。EP4 复用两份四卡来源，重建图和成本后自主预测 382,038 项工作、11 个 token，开发评分为 2.75%/9.76%。[组合评分](../../../data/20261119T090556.273460Z_project-delivery/work/s6-pipeline-relay-layout/score.json) · [EP4 评分](../../../data/20261119T090556.273460Z_project-delivery/work/s6-ep4-release-layout/score.json)
- TP1 双请求固定图以原始实测服务重算通过全部 12,962 个端点，4 个 token 的发布时刻与原生记录一致。用另两份原生运行重建成本后，8,712 项工作均可替换。只恢复实测 CPU 服务，TTFT 的偏差由提前约 331 毫秒变为延后约 1.94 毫秒，表明该固定案例的偏差集中在 CPU 服务。详细观察与轻量采样的测量差异仍需保留和核对，留出精度在 S11 验收。[固定图替换结果](../../../data/20261119T090556.273460Z_project-delivery/work/s6-scheduler-reference/fixed-independent-costs/result.json) · [成本来源](../../../data/20261119T090556.273460Z_project-delivery/work/s6-fixed-training/costs.json)

## S7 起点

S6–S7 共用的 30 次原生尝试已用 29 次。S7 优先盘点留存的模板、成本和实际触发证据，再按剩余额度选择能覆盖多项机制的具名采集。真实权重兼容性仍为可选范围。[S6 最后一次原生参考](../../../data/20261119T090556.273460Z_project-delivery/work/s6-scheduler-reference/capture/result.json)
