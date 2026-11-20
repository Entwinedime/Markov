# Markov 项目交付总窗口状态

更新日期为 2026-11-20。**S0–S7 已验收并推送。S9 已冻结验收资产并推送 `0de58e8`，S10 独立结构验收正在运行。**

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
| M3 正式支持范围 | S6–S7 | 开发验收完成。代表单机并行、分块、混合组批、前缀命中和淘汰均能用独立成本完成具名请求 |
| M4 独立验收与交付 | S9–S12 | S9 已交付，S10 首轮冻结矩阵已收齐，五例通过全节点结构对照，八例保留阻断。下一步是 S11 固定图及自主性能验收，之后统一修复并复验受影响范围，再进行 S12 复现与交付 |

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

## S7 交付证据

- Qwen3-8B TP2 的分块混合用例完成 13 请求、45 token 和 212,994 项工作。原生和预测均有 12 条请求分多批 Prefill，16 个混合批的成员数、Query token 数和上下文 token 数按次对应。[批次对照](../../../data/20261119T090556.273460Z_project-delivery/work/s7-scheduling/chunk-service-mechanisms.json) · [自主预测](../../../data/20261119T090556.273460Z_project-delivery/work/s7-scheduling/chunk-service-prediction/result.json)
- 同模型 TP2 的缓存压力用例完成 18 请求、27 token 和 104,512 项工作。原生和预测均有 20 次非空命中及一次淘汰。预测在 rank 0 也展开了一次查找拆分和一次插入拆分。更大的 40 请求历史用例用于确认原生压力范围，包含当前 8 字节索引释放样本之外的 288 token 释放。[缓存开发对照](../../../data/20261119T090556.273460Z_project-delivery/work/s7-scheduling/prefix-pressure-score.json) · [40 请求原生记录](../../../data/20261116T035124.971560Z_execution-structure-independent/targets/independent-b4/b4-s02/native/attempts/1/source/resources/rank-0.json)
- 两案例的独立成本预测已完成。采用详细采集的原生发布边界评分，分块混合的 TTFT/TPOT MAPE 为 79.25%/86.67%，缓存压力为 73.01%/58.49%。该对照用于开发，采集介入与成本误差的归因及 6% 的正式留出验收在 M4 完成。S6–S7 累计 34 次、约 5,162 秒原生，使用了从暂缓 S8 调拨的四个尝试名额。[分块开发评分](../../../data/20261119T090556.273460Z_project-delivery/work/s7-scheduling/chunk-service-score.json)

## S9 冻结准备

- 更新后的结构、自主评分入口通过具名开发检查。单卡 100、200 请求的执行生成分别用时 289.01、637.67 秒，双卡缓存压力 40 请求用时 43.95 秒。三组均完整输出声明的 token。[单卡测速](../../../data/20261119T090556.273460Z_project-delivery/work/s9-freeze/speed-results/summary.json) · [双卡测速](../../../data/20261119T090556.273460Z_project-delivery/work/s9-freeze/speed-results/multi-40/result.json)
- 13 个留出输入已按正式预测入口装配。冻结成本覆盖新版训练图的 2,129,571 项工作，TP8 保留 640 项主机成本缺口，分块补充图保留 16,000 项范围外的主机工作。PD 三 rank 的同源对照通过 12,198 个动作和 59,177 项工作，独立成本也完成三请求的八个 token。[冻结清单](../../../data/20261119T090556.273460Z_project-delivery/work/s9-freeze/frozen/matrix.json) · [PD 结构回归](../../../data/20261119T090556.273460Z_project-delivery/work/s9-freeze/pd-release-structure-audit/result.json)
- M4 已结算 S9 的五次原生尝试，共 663.93 秒。S10 和 S11 共用冻结版本，计划各提交 13 次详细和轻量观测。固定图服务验证使用页边界、TP2 两项详细观测。[冻结环境与额度](../../../data/20261119T090556.273460Z_project-delivery/work/s9-freeze/frozen/environment.json)

## S10 独立结构验收

- 九个来源域均已取得结构对照的来源资格。15 份观察覆盖 55 个 rank，图映射和时钟兼容性均通过。TP8 旧来源缺少的资源状态历史由留出目标独立取得。[来源核验](../../../data/20261119T090556.273460Z_project-delivery/results/m4-independent/sources.json)
- 四个 Azure 目标均完成详采，96 条请求的 11,752 个输出 token 全部对应，实际 Decode 批次达到每例的并发上限。四例参考图均停在同一提交归属检查，留存记录中的 30 处任务号回绕覆盖算子启动和事件记录。[原生调度覆盖](../../../data/20261119T090556.273460Z_project-delivery/work/s10-diagnostics/azure-native-coverage.json) · [常态回绕](../../../data/20261119T090556.273460Z_project-delivery/work/s10-diagnostics/azure-normal-rollover-cohorts.json) · [突发回绕](../../../data/20261119T090556.273460Z_project-delivery/work/s10-diagnostics/azure-burst-rollover-cohorts.json)
- 页边界、TP2、TP8 均通过独立全节点结构对照，分别覆盖 238、476、1,200 个动作。TP8 独立目标在八个 rank 上记录了资源状态，核对 151,128 项工作，全部工作结束状态均有实测依据。页边界、TP2 的同轮 CPU 活动供 S11 固定图服务验收。[页边界对照](../../../data/20261119T090556.273460Z_project-delivery/results/m4-independent/cases/qwen8-p16-boundaries/result.json) · [TP2 对照](../../../data/20261119T090556.273460Z_project-delivery/results/m4-independent/cases/qwen8-tp2/result.json) · [TP8 对照](../../../data/20261119T090556.273460Z_project-delivery/results/m4-independent/cases/qwen8-tp8/result.json)
- PP3 和 TP2×PP3 参考图均已生成。两例都在时间旁路把纯逻辑等待端点固定到调用提交之前时停止预测。对照器随后基于已完成端点重新校时，误报未执行的设备工作时长发生变化。[等待端点与时钟证据](../../../data/20261119T090556.273460Z_project-delivery/work/s10-diagnostics/pp-wait-conditions.json)
- 非对称 PD 推进至两个请求合并 Decode，在原生 KV 分配中遇到冻结来源缺少的多成员模板。同一分配分支在两个 Decode rank 上均有另一种工作结构。[PD 结果](../../../data/20261119T090556.273460Z_project-delivery/results/m4-independent/cases/qwen8-p1-d2/result.json) · [模板覆盖依据](../../../data/20261119T090556.273460Z_project-delivery/work/s10-diagnostics/pd-decode-family.json)
- 分块混合实测 13 个混合批次。留出仿真在转入两请求的纯 Decode 时停止，主来源和互补来源的这一动作都只覆盖了单请求。EP4 和前缀缓存均通过全节点结构对照，分别覆盖 934 和 1,950 个动作。前缀缓存每个 rank 观察到 24 次命中和一次 672 token 的淘汰。[分块目标](../../../data/20261119T090556.273460Z_project-delivery/results/m4-independent/cases/qwen8-chunk-mixed/result.json) · [EP4 对照](../../../data/20261119T090556.273460Z_project-delivery/results/m4-independent/cases/qwen30moe-ep4/result.json) · [缓存对照](../../../data/20261119T090556.273460Z_project-delivery/results/m4-independent/cases/qwen8-prefix-pressure/result.json) · [原生机制覆盖](../../../data/20261119T090556.273460Z_project-delivery/work/s10-diagnostics/native-mechanism-coverage.json)
- 13 例均完成原生详采并取得首轮结论。五例共核对 4,798 个动作、609,840 项工作和 914,612 个实测端点。六例停在参考图或时间旁路，两例停在模板缺口。矩阵计费为 13 次原生、3,717.76 秒原生及 6,562.21 秒后处理，离线归因约 10 分钟。S11 保留 13 次轻量观测额度，固定图先使用页边界和 TP2 的同轮服务证据。[矩阵产出](../../../data/20261119T090556.273460Z_project-delivery/results/m4-independent/comparisons.json)
