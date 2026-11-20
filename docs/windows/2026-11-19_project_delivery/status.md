# Markov 项目交付总窗口状态

更新日期为 2026-11-21。**S12 已完成干净环境复现，D1–D6 的依据和范围均已核对，进入发布和关闭。**

| 里程碑 | 阶段 | 当前交付 |
| --- | --- | --- |
| M0 交付基线 | S0 | 已交付，登记 SGLang/Ascend 的正式场景、可复用来源和设备预算 |
| M1 多角色计时 | S1–S3 | 已交付，TP2 与 Prefill TP1、Decode TP2 完成计时和开发闭环 |
| M2 配置集探索 | S4–S5 | 已交付，从空资产预测四个组合，换请求后完整复用 |
| M3 单机覆盖 | S6–S7 | 已交付，具名 TP、PP、EP、PD、并行组合、分块混合和一级缓存完成开发回归。S8 第二框架暂缓 |
| M4 独立验收与交付 | S9–S12 | S9 已交付。S10 新旧验证共二十一例通过全节点结构。S11 双卡固定图基线和替换归因通过，十三例取得完整自主评分。S12 干净检出构建、冷启动及换输入复用通过 |

## 交付状态

- EP4 与前缀缓存的两例新独立目标已完成全节点结构及自主评分。[独立对照](../../../data/20261119T090556.273460Z_project-delivery/results/m4-ep4-prefix-independent/comparisons.json) · [评分审计](../../../data/20261119T090556.273460Z_project-delivery/work/s11-repair/release-evidence/independent-topology.json)
- PD 新留出已取得全节点结构对照和完整自主评分，Azure 并发 4 稳态复核完成。M4 最终登记 112 次原生尝试，含 S12 四次，低于 113 次阶段上限。[PD 复核](../../../data/20261119T090556.273460Z_project-delivery/work/s11-repair/release-evidence/independent-pd.json) · [Azure 稳态复核](../../../data/20261119T090556.273460Z_project-delivery/work/s11-repair/release-evidence/independent-azure-steady-pair.json) · [S12 验证](../../../data/20261119T090556.273460Z_project-delivery/work/s12-release/verified.json)
- 定向唤醒已接入原生详采。新 TP2 的固定图原结论有 28 个提前端点，其中一处为合并通知引起的 24.12 微秒局部差异。修订后使用同次记录重放，64,186 个实测端点全部对齐，41,606 项工作完成实测与成本配对。较早 TP2 的基线和全部成本替换结果逐项相同。两项结果记为开发回归。[冻结原结论](../../../data/20261119T090556.273460Z_project-delivery/work/s11-repair/fixed-holdouts/tp2-kernel-independent/target-wake-implementation/independent/fixed/result.json) · [修订回归](../../../data/20261119T090556.273460Z_project-delivery/work/s11-repair/fixed-holdouts/tp2-kernel-independent/target-wake-implementation/notification-count-regression/repair/result.json) · [旧例回归](../../../data/20261119T090556.273460Z_project-delivery/work/s11-repair/fixed-holdouts/tp2-kernel-independent/target-wake-implementation/notification-count-regression/unchanged/result.json)

## S10 独立结构验收

- 首轮 13 例均取得结论。页边界、TP2、TP8、EP4 和前缀缓存五例通过全节点结构，共核对 4,798 个动作和 609,840 项工作。九个来源域均具备对照资格。首轮受限结果保留在原矩阵。[首轮结构结果](../../../data/20261119T090556.273460Z_project-delivery/results/m4-independent/comparisons.json) · [来源核验](../../../data/20261119T090556.273460Z_project-delivery/results/m4-independent/sources.json)
- 新页边界、TP2、Azure 常态并发 4 和 8 另四例通过全节点结构。PP3、TP2×PP3 的 1,097、2,216 个动作全部匹配，102、172 个 CPU 搬运区间仍只有两端边界，结论为已检查范围内结构通过。[单卡对照](../../../data/20261119T090556.273460Z_project-delivery/work/s11-repair/tp1-refresh/results/m4-tp1-refresh/comparisons.json) · [TP2 对照](../../../data/20261119T090556.273460Z_project-delivery/work/s11-repair/tp2-fixed-refresh/results/m4-tp2-fixed-refresh/cases/tp2-fresh-fixed/result.json) · [流水线独立对照](../../../data/20261119T090556.273460Z_project-delivery/results/m4-pipeline-refresh/comparisons.json)
- 分块混合和 Azure 突发并发 4、8 的新独立留出分别通过 2,248、18,019、14,427 个动作的全节点对照。用于固定图的新 TP2 和页边界也通过 354、261 个动作。[调度对照](../../../data/20261119T090556.273460Z_project-delivery/results/m4-scheduler-refresh/comparisons.json) · [新 TP2](../../../data/20261119T090556.273460Z_project-delivery/work/s11-repair/fixed-holdouts/tp2-independent/structure/result.json) · [新页边界](../../../data/20261119T090556.273460Z_project-delivery/work/s11-repair/fixed-holdouts/page-independent/structure/result.json)
- 另一组 TP2 独立留出再次通过 354 个动作的全节点结构对照，真实时间条件全部消费。固定图中两个额外队列唤醒的资格单独保留。[TP2 复核对照](../../../data/20261119T090556.273460Z_project-delivery/results/m4-tp2-fixed-evidence/comparisons.json)
- 新 EP4 四 rank 和前缀缓存双 rank 独立留出均通过全节点结构，分别核对 934、1,950 个动作和 276,114、116,368 项工作，全部消费实测条件。前缀缓存的对照包含完整 KV 分配与释放历史。[EP4 对照](../../../data/20261119T090556.273460Z_project-delivery/results/m4-ep4-prefix-independent/cases/qwen30moe-ep4-independent/comparison/actions.json) · [前缀缓存对照](../../../data/20261119T090556.273460Z_project-delivery/results/m4-ep4-prefix-independent/cases/qwen8-prefix-pressure-independent/comparison/actions.json)
- 带内核调度记录的新 TP2 留出通过 354 个动作的全节点结构，后续固定图独立核对主机测量资格。[结构对照](../../../data/20261119T090556.273460Z_project-delivery/work/s11-repair/fixed-holdouts/tp2-kernel-independent/structure/result.json)
- 新非对称 PD 通过全节点结构，共 6,980 个动作、53,983 项工作、85,611 个实测端点。六次 KV 和六次 metadata 交接均有原生依据，九个发布值与预测对应。[PD 对照](../../../data/20261119T090556.273460Z_project-delivery/results/m4-pd-independent/cases/qwen8-p1-d2-independent/comparison/actions.json)
- 带定向唤醒记录的 TP2 新留出通过全节点结构，对照和固定图共用同次原生详采。[结构对照](../../../data/20261119T090556.273460Z_project-delivery/work/s11-repair/fixed-holdouts/tp2-kernel-independent/target-wake-implementation/independent/structure/result.json)
- 合并通知修订后的双卡新留出在校正逻辑等待的证据核验后通过全节点结构，354 个动作、41,606 项工作、62,446 个实测动作端点及四个动作内逻辑端点全部对应。原冻结的证据资格结果保留。[结构复核](../../../data/20261119T090556.273460Z_project-delivery/work/s11-repair/fixed-holdouts/tp2-kernel-independent/target-wake-implementation/logical-wait-reference-review/structure/result.json) · [原冻结结论](../../../data/20261119T090556.273460Z_project-delivery/work/s11-repair/fixed-holdouts/tp2-kernel-independent/target-wake-implementation/notification-count-independent/structure/result.json)

## S11 独立性能验收及支持范围

首轮 13 例全部取得结论，完整自主评分为 0 例。补充独立验证有十三例完整评分，等权 **TTFT MAPE 2.91%、TPOT MAPE 4.67%**，两个指标的请求分母分别为 144 和 129。[首轮自主结果](../../../data/20261119T090556.273460Z_project-delivery/results/m4-independent/autonomous.json) · [十三例汇总](../../../data/20261119T090556.273460Z_project-delivery/work/s11-repair/release-evidence/independent-pd.json)

TP2 固定图在独立于模板与成本训练的原生留出上复核。64,178 个有时间依据的端点全部对齐，另有八个逻辑端点按真实依赖推进，41,606 项工作全部完成成本配对。全量替换令 P95 TTFT 提前 147.60 毫秒，恢复实测主机服务后提前 5.24 毫秒。此归因限于该固定图与实测条件。验证器修订复用了原观察，另用 TP2×PP3 的 86 个逻辑等待端点核对原始证据规则。[固定图资格及归因](../../../data/20261119T090556.273460Z_project-delivery/work/s11-repair/release-evidence/fixed-reference.json)

下表记录 SGLang/Ascend 的具名配置。结构与自主评分可取自同一配置下的不同冻结留出，精度按已评分的请求窗口报告。

| 场景 | 模型及关键配置 | 结构资格 | TTFT / TPOT MAPE | 自主评分证据 |
| --- | --- | --- | ---: | --- |
| Azure 常态，并发 4 / 8 | Qwen3-8B BF16，TP1，页 16 | 全节点 | 1.36% / 2.67% · 1.32% / 4.08% | [单卡评分索引](../../../data/20261119T090556.273460Z_project-delivery/work/s11-repair/tp1-refresh/results/m4-tp1-refresh/autonomous.json) |
| 双卡 | Qwen3-8B BF16，TP2，页 16 | 全节点 | 1.15% / 10.38% | [双卡评分](../../../data/20261119T090556.273460Z_project-delivery/work/s11-repair/tp2-autonomous-refresh/autonomous/result.json) |
| 八卡，50 毫秒交错 | Qwen3-8B BF16，TP8，页 32 | 全节点 | 3.74% / 4.17% | [八卡评分](../../../data/20261119T090556.273460Z_project-delivery/work/s11-repair/tp8-service-training/staggered-autonomous/autonomous/result.json) |
| 流水线 | Mistral 7B FP16，PP3，页 32 | CPU 搬运有端点界限 | 0.49% / 0.78% | [流水线评分索引](../../../data/20261119T090556.273460Z_project-delivery/results/m4-pipeline-refresh/autonomous.json) |
| 张量与流水线组合 | Mistral 7B FP16，TP2×PP3，页 16 | CPU 搬运有端点界限 | 2.37% / 2.75% | 同上 |
| 单卡页边界 | Qwen3-8B BF16，TP1，页 16 | 全节点 | 0.66% / 3.12% | [页边界评分](../../../data/20261119T090556.273460Z_project-delivery/results/m4-fixed-refresh/autonomous/tp1-page-independent-fixed/result.json) |
| 分块预填充与混合组批 | Qwen3-8B BF16，TP2，分块 128 token | 全节点 | 3.80% / 11.86% | [分块评分](../../../data/20261119T090556.273460Z_project-delivery/results/m4-scheduler-refresh/autonomous/qwen8-fresh-chunk-mixed/result.json) |
| Azure 长前缀突发，并发 4 / 8 | Qwen3-8B BF16，TP1，页 16 | 全节点 | 7.57% / 5.43% · 6.72% / 2.27% | [稳态评分](../../../data/20261119T090556.273460Z_project-delivery/work/s11-repair/release-evidence/independent-azure-steady-pair.json) |
| 专家并行 | Qwen3-30B-A3B BF16，TP4×EP4，页 128 | 全节点 | 4.08% / 2.07% | [多卡拓扑评分](../../../data/20261119T090556.273460Z_project-delivery/results/m4-ep4-prefix-independent/autonomous.json) |
| 一级前缀缓存与回收 | Qwen3-8B BF16，TP2，页 16 | 全节点，观察到命中和淘汰 | 1.20% / 2.56% | 同上 |
| 非对称 PD | Qwen3-8B BF16，Prefill TP1、Decode TP2，页 16 | 全节点 | 3.33% / 8.52% | [独立评分](../../../data/20261119T090556.273460Z_project-delivery/work/s11-repair/release-evidence/independent-pd.json) |

- 已从原始请求记录复算六例分数并核对训练来源。Azure 常态并发 4/8、PP3 与 TP2×PP3 均在相同请求时序和 token 内容下比较。两对三项指标的实测排序与预测排序一致，各配置只有一次测量，结论限于该次点估计。[评分与排序对照](../../../data/20261119T090556.273460Z_project-delivery/work/s11-repair/release-evidence/independent-autonomous.json)
- Azure 长前缀突发的并发 4 与 8 共用 24 条请求和成本。实测与预测均显示并发 8 降低 P95 TTFT，并发 4 降低平均 TPOT。原生吞吐的单次差距约 1.37%，轮间波动未测。[突发排序](../../../data/20261119T090556.273460Z_project-delivery/work/s11-repair/release-evidence/independent-azure-steady-pair.json)
- 分块的自主与真实条件重放，两 rank 的 44 个批次按请求、阶段与 token 范围完整匹配。独立轻量评分的八个 TPOT 都偏高，中位偏差为 11.81 毫秒。轻量运行只记录请求发布时刻，现有批次时间来自另一详采运行，分层延迟归因仍缺同轮证据。[批次对照](../../../data/20261119T090556.273460Z_project-delivery/work/s11-repair/release-evidence/chunk-timing-development/batch-timing.json) · [请求偏差](../../../data/20261119T090556.273460Z_project-delivery/work/s11-repair/release-evidence/outlier-observations.json)
- 固定图开发回归中，页边界 22,515 个端点和 TP2 的 64,186 个端点全部重放对齐，分别完成 15,137、41,606 项成本配对。TP2 全量替换的 p95 TTFT 提前 161.67 毫秒，只恢复实测主机服务后延后 1.60 毫秒。页边界另用留存目标完成自主开发评分，TTFT、TPOT MAPE 为 2.72%、2.69%。[双例固定图证据](../../../data/20261119T090556.273460Z_project-delivery/work/s11-repair/tp2-fixed-refresh/device-record-candidate/review.json) · [页边界自主回归](../../../data/20261119T090556.273460Z_project-delivery/work/s11-repair/tp1-page-service-training/development/autonomous/result.json)
- 固定时间资格及校时优化已在原批次解冻后合入。TP2 的 1,480 个设备 wait 有独立完成依据。校时跳过测量已证明的顺序，分块案例的约束准备从 41.1 秒降至 24.0 秒，求解约束及 300,074 个已测端点的时刻一致。[合入回执](../../../data/20261119T090556.273460Z_project-delivery/work/s11-repair/tp2-fixed-refresh/observer-record-qualification/integration.json) · [校时对照](../../../data/20261119T090556.273460Z_project-delivery/work/s11-repair/clock-qualification-audit/result.json)
- 原冻结对照保留 TP2 设备记录漏接及单卡页边界唤醒缺测的结论。提交队列的取队证据和详采 tracepoint 挂载已修正，缺挂载在采集入口报错。[TP2 冻结结果](../../../data/20261119T090556.273460Z_project-delivery/work/s11-repair/fixed-holdouts/tp2-independent/fixed/result.json) · [页边界冻结结果](../../../data/20261119T090556.273460Z_project-delivery/work/s11-repair/fixed-holdouts/page-independent/fixed/result.json) · [合入核对](../../../data/20261119T090556.273460Z_project-delivery/work/s11-repair/fixed-holdouts/tp2-kernel-independent/host-service-partition/integration.json)
- 严格 TP2 新留出记录 389,978 个内核事件，41,606 项工作均有实测和成本。隔离修订按实际 CPU 占用区分业务服务和固定活动，消除三处提交线程局部偏差。剩余一处控制线程阻塞为 423.13 微秒，观察到离核和恢复，缺唤醒者证据。较早 TP2 的完整基线继续通过，仍为开发证据。[冻结固定图](../../../data/20261119T090556.273460Z_project-delivery/work/s11-repair/fixed-holdouts/tp2-kernel-independent/fixed/result.json) · [四处原始诊断](../../../data/20261119T090556.273460Z_project-delivery/work/s11-repair/fixed-holdouts/tp2-kernel-independent/local-causal-audit.json) · [隔离回归](../../../data/20261119T090556.273460Z_project-delivery/work/s11-repair/fixed-holdouts/tp2-kernel-independent/host-service-partition/latest-tp2/result.json)
- 正式查询覆盖 PD 原参考三 rank 的 8,912、22,403 和 22,387 项工作。旧参考的开发回归与新独立目标分别保留。[正式查询](../../../data/20261119T090556.273460Z_project-delivery/work/s11-repair/pd-service-training/admission-sequence/costs/costs.json) · [开发评分](../../../data/20261119T090556.273460Z_project-delivery/work/s11-repair/topology-holdouts/development-pd-admission/case/result.json) · [新目标审计](../../../data/20261119T090556.273460Z_project-delivery/work/s11-repair/release-evidence/independent-pd.json)

## S12 可复现交付

- 从远端提交 `605eb41` 干净检出，初始化 SGLang `4e155ecc`。复用已有 Ascend 基础环境镜像，正式构建采集、控制及 modeling 镜像，C++23 主程序 SHA-256 与 S11 冻结程序相同。构建墙钟为 529.58 秒，包含 Ubuntu 源下载和依赖安装。完整镜像 ID 和二进制校验值见回执。[复现回执](../../../data/20261119T090556.273460Z_project-delivery/work/s12-release/verified.json)
- 单卡 Qwen3-8B 空目录冷启动完成 4/4 个组合，墙钟 665.72 秒，四次原生采集占 494.16 秒，来源和成本后处理占 66.21 秒。更换请求身份及到达时间并新增一条迟到请求后，四个目标继续使用冷运行资产。复用墙钟 106.87 秒，原生及后处理均为零，按需保留四份执行轨迹。逐目标输出、三项排序、精简结果和明细分别核对。[冷启动](../../../data/20261119T090556.273460Z_project-delivery/work/s12-release/checkout/data/20261120T172737Z_exploration/summary.md) · [换请求复用](../../../data/20261119T090556.273460Z_project-delivery/work/s12-release/checkout/data/20261120T172737Z_exploration-reuse/summary.md)
- 保留零额度约束及缩短输出后明确报告 KV 释放成本缺口的预检。正式复用的两个 workload 输出总量按实际输入核对为六和五，原外层核对记录保留。[覆盖边界](../../../data/20261119T090556.273460Z_project-delivery/work/s12-preflight/example/reuse-shorter-preflight/result.json) · [复核回执](../../../data/20261119T090556.273460Z_project-delivery/work/s12-release/verified.json)

## 已交付基线

- 本窗口前的单卡独立结构覆盖四个常态与突发组合，共 5,964,982 项工作。随后在四个独立性能组合上取得等权 TTFT 3.67%、TPOT 4.92% MAPE。[原结构留出](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/heldout-results.json) · [原性能留出](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s7-independent-performance/independent-results.json)
- S5 配置集冷运行用五次原生采样得到四项目标。换新请求身份与到达时间后，零采样和零来源后处理完成四项预测。S9 的单卡 100、200 请求执行生成分别用时 289.01、637.67 秒，双卡缓存压力 40 请求用时 43.95 秒。[冷运行](../../../data/20261119T122453.716218Z_exploration/summary.md) · [复用结果](../../../data/20261119T124125.641276Z_exploration/summary.md) · [规模测速](../../../data/20261119T090556.273460Z_project-delivery/work/s9-freeze/speed-results/summary.json)
