# 全量重构当前状态

更新日期：2026-10-31。Goal 继续；R3 在声明范围内收口，R4 已启动原始 60 cross 的来源与成本准备。全局精度、完整面板和资源搜索尚未验收。
工作目录 /home/markov-rebuild，分支 rebuild；R3 交付基线 1e96ac1，R4 从该提交继续。本批只操作重构项目，未复制老项目数据或修改 markov-next。
本文件保留当前资格和恢复起点，历史过程仍由原日志及运行资产记录。

## 阶段资格

| 阶段 | 当前资格 | 边界或下一步 |
| --- | --- | --- |
| 既有 0/A 底座、R0 复盘 | 已交付 | 完整请求优先、大 base、小校准、粗粒度程序 |
| R1 完整 SGLang 预测 | 限定范围交付 | 有 self/cross 和正常对照，不等于整体精度通过 |
| R2 用户流程与共享准备 | 限定范围交付 | 新 base、空库、整组补采、恢复与复用；现补齐跨工作域的数据保留 |
| R3 框架与关键分支 | 限定范围交付 | 两框架、缓存路径、NodeScale、回放及准备屏障有真实证据；近似见下 |
| R4 实验与资源搜索 | 实施中 | 薄矩阵已启动原始 60 格；先完成来源、共享成本及正常评分，再做有限搜索 |
| R5 完整验收与清理 | 待开始 | 扩展 72、留出 54、三类报告、正常精度和受控全流程效率 |

## 当前正式流程

用户声明 deployment、workload、base、targets 和性能来源，经 scripts/markov.sh predict 准备来源、整组成本并预测。
普通预测只读 manifest 指定的必要观察，不建源细图、不读大型设备 trace；同次 C++ 会话共享源上下文和只读模型，各 target 独立维护状态。
目标规则生成工作与依赖，变化工作查独立模型，已证明未变的成本才迁移。完整正式 HTTP 保留 gap、未知控制和 Prefill/Decode；后台排空单列，缺项保留 null。
预测容器只挂载程序、指定 base、选定模型、准备输入和输出；正常真值、数据库和 evaluator 不进入 predictor。

workload 现可声明 drain_before_formal：首个正式请求前等待 HiCache 和原生服务空闲，按轻量 pending 计数判断，不读缓存树或 tensor snapshot。
屏障单列，实际请求/token 用量不增加；预测必须与 source 的控制一致。KTransformers 不支持 HiCache 屏障，不静默忽略。
自动补采按整个采样器保留此前已选中的合格独立来源，防止补一类成本时覆盖掉其他页大小；不把库中未选的历史采集加入训练。

## 已保留的框架与执行范围

SGLang 已实际运行 Qwen3-32B/BF16/TP2、Ascend 910B3、eager/FIA、串行固定输出，覆盖有限容量、选择性/直写/写回、回载及失败回退、文件预取、逐页发布和取消。
CPU 返回、流进度、输出可读分开；回载的本地完成推进本 rank GPU，整组汇合保留缓存发布和保护释放。
一次淘汰扫描一轮候选，结束后只重试分配；部分备份后失败保留已做工作，释放主机保护并沿设备前缀重新计算。
后台查询、发起、最终检查、停止和接受有独立边界；检查期间可发布，停止后固定页数，取消不提前归还 worker 仍持有的缓冲。

配额核对以当前串行入口为限：原生调度调用没有传入 load quota，原面板的正预取额度在前一请求继续计算前归还；在途缓冲另按真实生命周期占用容量。
现有高阈值跳过、失败回载、取消后后台继续、跨请求文件写入及保护释放均有实际输入或给定成本诊断证据，不把不可达的并发占用当作已实测能力。

KTransformers 已实际运行完整 Llama-2-7B/float16、单 Ascend、Torch/dense 来源和独立采样，self、分块 Prefill、SDPA 进入共同执行器；仅 empty 初始化。
该范围的原正常开发对照约 2.6%—3.8%；本批三个既有目标连同图保持不变，不能推广到其他后端或并发。
NodeScale 默认关闭；源/目标固定路径回放保留成本归属与依赖，不重新选择时间分支，也不将诊断参数用于拟合。

## 本批原始 workload 的实际结果

原 W1 重新编码为 47 请求/102,272 输入/47 输出，前 35 请求准备，后 12 请求正式计时；准备结束后排空后台。
配置仍为原 C2/C4 声明，未缩短原 workload 或复制旧采集资产。

| W1 条件 | 完整正式窗口预测 | 正常单次实测 | APE |
| --- | ---: | ---: | ---: |
| C2 → C2，self | 8.647043 秒 | 8.427619 秒 | 2.604% |
| C2 → C4，cross | 5.480101 秒 | 5.519728 秒 | 0.718% |

窗口含客户端间隔；正常关闭 profiler 和探针，保留同一准备屏障。这里只是 1 个 cross 和 1 个 self，各一次正常观察，不是 60 格成绩。
source 一次 477.693 秒、7,106,674,771 bytes；两次共享补采累计 194.339 秒，其中 12 个模型请求/16,329 输入/30 输出，另一次是无模型请求的原生物理采样。
补采组最终保留量快照为 705,216 bytes，现行仍为 38 类响应；原已选来源全部保留。两个正常条件分别另用 169.193、161.366 秒，均不训练。
默认两目标复用 7.401 秒，source 解释 5.855 秒，C++ 峰值 97.99 MiB、报告 93,672 bytes；不建立源细图、不读两份设备 trace。
R3 交付时自有源码 193 文件/25,206 行，含空行，该批净增 91 行，无独立检查源码。

Docker Release/Debug 与改动文件静态检查通过；新两目标连同完整图一致，11 个既有条件不变，两条原成本回放的全部操作/请求零差。
同源独立评价替换 2,408/3,910 项，1,502 项保留估计；1,384 条可检查顺序零违例，固定路径 partial oracle 11.660712 对 profiled 11.788559 秒。
该评价不认证全部设备/缓存因果，也不是正常精度。完整证据为 data/r3/preparation_gate/checks.json。

## 仍然存在的近似

- GPU 回载仍等本地全部层完成，逐层复制/计算重叠、资源竞争和冷介质没有精确资格。
- 未知 CPU residual、客户端间隔、首次使用与其他外围控制仍有 source 迁移或区域响应近似；部分备份失败的排他 CPU 还存在外推不确定性。
- 源细图曾修复资源首次可用、设备空白及 KTransformers 同步返回；SGLang/KTransformers 仍有最大 3.755486 ms/9.849 μs 的跨 CPU/设备坐标冲突，没有据此校时或改系数。
- 源/目标固定路径诊断和可观察的 CPU 顺序不能替代完整结构比较；单次正常误差不授予全局或任意部署的精度。

这些限制进入完整面板报告与后续误差分析，不从 HTTP 中删除相关成本。原始 60、扩展 72、留出 54、资源搜索和受控 10× 比较仍是未完成范围。

## 恢复工作所需资产

| 资产 | 当前入口 |
| --- | --- |
| 性能库与当前选择 | data/r3/performance.sqlite；data/r3/preparation_gate/prediction/performance/selected_models.json |
| 原三个 workload | data/r3/preparation_gate/w1_device_spill_qualification_ring.json、w2_writeback_dual_tier_cascade.json、w3_prefetch_admission_ladder_and_survival.json；分别 47/39/87 请求 |
| 原五配置 | data/r3/preparation_gate/original_configurations.json；后续移入独立的可复现 example 声明 |
| W1/C2 source | data/r3/preparation_gate/source/base/captures/1/profile_manifest.json；可复用为原 15 份来源中的一份 |
| W1/C2、W1/C4 正常对照 | data/r3/preparation_gate/normal、normal_cross；各一次样本，独立评分时读取 |
| 当前预测/回放/评价 | data/r3/preparation_gate/prediction、debug、compact、replay_self、replay_cross、evaluation |
| 真实跨请求后台来源 | data/r3/copy_boundaries/frontier_source/base/captures/2/profile_manifest.json；对应检查在同目录 checks.json |
| 其他缓存与控制证据 | data/r3/prefetch_control、prefetch_query、load_failure、load_failure_followup、cpu_broadcast 各 checks.json |
| 无文件、文件与部分备份来源 | data/r3/cpu_broadcast/source_no_file、prefetch_control/source_file、load_failure_followup/source_partial；原资产保留 |
| 两框架源细图诊断 | data/r3/source_causality/checks.json、sg_fenced.json、k_fenced.json |
| KTransformers 真来源与正常对照 | data/r3/ktransformers/capture/base/captures/3/profile_manifest.json、normal_evaluation.json、checks.json |
| R1/R2 准备与恢复 | data/r1、data/r2；失败、预算、原始独立数据继续保留 |

当前镜像为 markov-modeling:ubuntu24.04、markov-sglang:ubuntu22.04、markov-ktransformers:ubuntu22.04。
R3 按准备屏障、共享数据保留及窗口证据分层交付；下一阶段从交付提交继续。

## R4 当前运行与下一步

examples/hicache_matrix 已提供独立配置、输入生成、准备、预测、正常测量与汇总编排，仍待完整实际运行验证。
data/r4/matrix 已完成 W2/C2、W2/C5、W2/C1、W2/C3 四份新来源；加上既有 W1/C2，共 5/15 份来源可用，W2/C4 正在采集。
四个 W2 base 的五配置准备预测均完成，共 16 cross 和 4 self；这不是最终共同模型选择下的 60 格评分。
六份 ShareGPT 开发/留出输入已在 CPU 容器生成，未使用目标误差选样；扩展面板尚未实跑。

早期两组补采因临时页文件超过 256 MiB 中止；调至 1024 MiB 后，首组补齐域并供其他组复用。
该组累计 12 启动、1354.664 秒、71 请求/85,246 输入/125 输出，另有 15.179 秒处理；最后组保留快照为 4,034,623 bytes，包含失败而不重置。
两份失败会话的无引用页文件分别经检查后清理 402,653,184 bytes，日志、输入、状态及历史消耗保留。
新成功采集在导出必要资产后删除会话临时页文件，两份新来源分别回收 2,650,800,128 和 7,918,845,952 bytes；原始 trace 与失败证据保留。

纯预测、评价与 modeling Shell 现显式使用 runc，不继承宿主默认 Ascend runtime；真实入口两目标的 HTTP、后台及节点/提交计数不变。
新矩阵入口另复用真实 W1/C2、C4 正常样本完成一组评分，APE 仍为 2.604%/0.718%；这只有一个 self/cross。
该 source 的公开静态回放生成 16,337,033 节点，47 请求回放耗时与观察一致；不据此认证全部因果或正常精度。
设备实际占用时，1 秒等待预算在 1.162 秒后明确超时，没有启动采集。
调度进程已重载并接管同一 W2/C3 容器：原第 1 次启动完成 39 请求，用时 430.213 秒，没有重采，已继续下一组；证据在 data/r4/live_resume.json。
清理临时页后的新 W2/C2 来源也完成静态回放，13,593,338 节点、39 请求耗时零差，证明这些页不是该回放的输入。
当前源码 197 文件/26,085 行，含空行，其中 examples 4 文件/851 行；未新增独立检查源码。批次统一证据在 data/r4/checks.json。
本批代码分层提交为 968dd0a（任务生命周期与 CPU 隔离）、1c92c9a（矩阵例子），文档同批交付；R4 仍在实施中。

先补齐原 5 配置×3 workload 的来源和评分。整组准备成本后，正式预测使用共同的现行选择；输出全部 60 cross，self 单列，失败和缺项保留。
首轮先做单次正常测量以尽快取得全表，不能替代默认三次重复或稳定精度验收。
随后接有限资源成本搜索，再完成其余 126 格和全局精度、三份报告、效率与清理；不重新启动已终止的大空间研究。
完整精度仍按总体及逐 base 的 WAPE≤3%、P90≤5% 评价，逐格尽量≤6%；不得按正常误差逐格补参数。
