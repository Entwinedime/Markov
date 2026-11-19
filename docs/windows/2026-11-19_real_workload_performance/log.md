# 真实流量性能预测窗口日志

## 2026-11-19 — 窗口筹备：确定首个真实性能验证范围

**范围：** 以 `7315c0c` 为筹备参考，整理开发速度、真实流量、执行结构泛化和性能预测的讨论。

**改动：** 建立五个实施阶段，优先测现有执行路径的开销。选定 Qwen3-8B 单实例 TP1，以两档并发和两个独立流量窗口评价预测。明确成本测量与真实目标分离，以及 MoE 和跨拓扑的后续范围。

**验证：** 静态核对窗口文档约定、现有预测时间接口、采集加载模式、输入合同与可复用的小案例记录。运行验证尚未启动。

**结论与限制：** 本批完成窗口筹备。日常预测基线、真值的观察扰动、成本标签覆盖和独立精度仍需在实施中取得证据。

## 2026-11-19 — 计划修订：先导入真实流量并细化性能接入

**范围：** 按确认的推进顺序重排窗口，明确 100/200 条测速、条件优化、性能成本与执行结构的交接及独立验收。

**改动：** 扩充为 S0–S7。加入按墙钟定位热点和同口径复测的步骤，将性能建设拆为计时职责与测量资格、有界采样、正式预测接入，并把冻结评价与结果归因设为独立阶段。

**验证：** 静态追查 workflow、CLI、SGLang 装配、动作时间线与 CPU 消息服务的计时入口。核对阶段交接、文档层次与样本隔离。运行验证尚未启动。

**结论与限制：** 主动作和 CPU 协议均有给定耗时入口，TP1 的实际调用覆盖留待实施核实。100/200 条实际墙钟、测量资格和精度均待取证。

## 2026-11-19 — 计划修订：确认全节点实测验证与自主预测

**范围：** 明确两条执行闭环、共用代码与时间资产的职责。

**改动：** 结构验证采用逐工作和事件的真实时间依据，独立比较图与状态。正常预测仅使用独立成本驱动同一执行循环。细化两条路径的输入限制、全节点覆盖和窗口验收条件。

**验证：** 核对现有 `ExecutionInputs` 的根端口与资源尾部匹配、后继时间上界和共用 `GraphTiming` 入口。实施与运行证据待后续取得。

**结论与限制：** 现有后继上界可约束局部时间，逐工作和事件的实测时间匹配及完整覆盖是本窗口待交付能力。

## 2026-11-19 — S0 开始与收尾：导入真实流量

**范围：** 以 `7315c0c` 为源码起点，固定 Azure 会话负载及单卡 SGLang 环境。窗口筹备文档纳入本次交接，reference 与本地忽略规则的既有改动独立保留。

**改动：** 在示例层增加流式 CSV 转换，复用正式 workload 校验和原子写入。固定同一片段的前 100 条和完整 200 条，并按原始到达跨度登记两个不重叠的 48 条留出。选择并发 4/8、32768 KV token、页大小 16，锁定 dummy 权重域及实际镜像。两档结构测速的开发反馈目标分别为 300/600 秒，单次运行上限独立设为 4200 秒。

**验证：** 审查转换层到正式入口的参数传递、整数时间与数据发布职责。核对 100 条是 200 条输入的原样前缀，200 条保留 322584 输入及 22007 输出 token。Docker Release 构建成功。给定 100 μs 的正式试跑在 28.68 秒内完成 3 请求和 44 token，展开 40 个批次。 [固定输入](../../../data/20261118T172915.294780940Z_real-workload-performance/inputs/workload-selection.json) · [结构试跑结果](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s0-development-3/result.json)

**结论与限制：** 本阶段业务源码净变更为零，新增可复用的示例转换。采集与独立验收未启动，较大输入的实际完成情况和开销由 S1 核对。

## 2026-11-19 — S1 开始：定界真实长度的来源需求

**范围：** 以已推送的 `47bac12` 为起点。固定 100/200 条输入、并发 8 和给定 100 μs，保留两段独立留出。

**改动：** 首次 100 条测速停在 `extend_naive`。沿原生分配分支、采集和控制交接追查，源库主要描述小分配，前 100 条中已有 12 条请求单独就能达到 200 个新页。已有长 Prefill 原始观察分别见证无回收和有回收，采用一组有序原生提交集中观察冷分配、批内多次朴素写入、两类分配遇到回收、Decode 跨页。用同实现域的 8192 token 池构造压力，测速仍用 32768 token 池。正式采集计划复用独立批次合并和筛选的原始观察。 [来源计划](../../../data/20261118T172915.294780940Z_real-workload-performance/work/s1-source-plan/plan.json)

**验证：** 固定新采集为单卡一次，11 个请求、16423 个输入 token，累计原生预算 480 秒，后处理 900 秒，集合存储 16 GiB。已核对设备空闲、模型和镜像身份、原生观察引用及两档目标输入。 [首次测速错误](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s1-speed-100/core.log) · [采集范围与额度](../../../data/20261118T172915.294780940Z_real-workload-performance/work/s1-acquisition-scope.json)

**结论与限制：** 首轮耗时只覆盖失败前路径。来源计划描述的是可触发条件，仍需核对实际采集和完整测速。

## 2026-11-19 — S1 来源交付：长 Prefill 与资源压力

**范围：** 同一 Qwen3-8B BF16 单卡域的一次代表采集，加一份相符的已有局部批次观察。

**改动：** 正式入口发布主来源与局部来源，保留原始记录的任务归属。采集覆盖跨页 Decode 和 Prefill 分配，主库记录两个朴素分配调用，其中一次包含连续成员写入。局部库记录批次合并及筛选。 [发布的来源集合](../../../data/20261118T172915.294780940Z_real-workload-performance/sources/s1-allocator-coverage/result.json)

**验证：** 原生一次尝试计 153.82 秒，其中初始化 25.62 秒、预热 52.33 秒、有效观测 35.30 秒。两份资产后处理共 78.43 秒，集合保留 92.05 MB。固定建好的来源后启动 100/200 条正式入口测速，用进程级采样和仅附着本次程序的探针量化开销。 [采集及生产计量](../../../data/20261118T172915.294780940Z_real-workload-performance/sources/s1-allocator-coverage/status.json)

**结论与限制：** 新来源已使 100 条用例继续经过长 Prefill；完整输出和阶段热区以测速终态为准。

## 2026-11-19 — S1 首档基线：定位依赖维护开销

**范围：** 冻结新来源，以并发 8、给定 100 μs、关闭明细运行 Azure 前 100 条。

**改动：** 完成 100 条基线。保持相同资产启动 200 条，之后用三条开发输入对照明细开关。性能探针附着本次程序，业务源码保持在阶段起点。

**验证：** 正式入口核对 100 条请求全部结束并消费 10483 个输出 token。墙钟 585.14 秒，核心循环 557.97 秒，生成 43159 个动作和 1727886 个工作区间。构图累计 35.22 秒，依赖绑定与时间线插入 113.49 秒，依赖清理 264.18 秒。清理走了 49310 次。进程采样显示红黑树遍历热，与代码中每轮规划扫描页、槽位内容吻合。 [请求结果](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s1-final-100/result.json) · [墙钟及资源](../../../data/20261118T172915.294780940Z_real-workload-performance/logs/s1-final-100.json) · [区段计时](../../../data/20261118T172915.294780940Z_real-workload-performance/logs/s1-final-100.probes)

**结论与限制：** 100 条已超过 300 秒反馈目标，需安排 S2。已完成事件的回收与 KV 历史同步清理，把与本轮执行无关的驻留页也带进了热路径。200 条和明细对照仍须以同口径完成。

## 2026-11-19 — S1 等待期间核对历史计时证据

**范围：** 读取九份已保留的单实例 TP1 原生图，核对后续逐工作计时可复用的证据。

**改动：** 用当前动作选择规则归属工作，并计算旧旁路根端口及资源尾部覆盖。将同一次采集的端点数量与现有输入的实际交接数量对照。 [复用证据清单](../../../data/20261118T172915.294780940Z_real-workload-performance/work/s3-retained-evidence.json)

**验证：** 原生图共产生 216566 个有时间戳的端点，现有旁路交接其中 18029 个。148917 项 Host、Device 和 Transfer 工作均保留测量起止，均可归属到一个真实动作。只有 6050 项工作的两端已通过现有旁路交接。

**结论与限制：** 可先复用这些记录开发逐工作匹配。时钟校准、预测图端点的唯一匹配和服务时间资格仍需核对。

## 2026-11-19 — S1 收尾：两档墙钟和明细开关对照

**范围：** 在同一程序、镜像、模板、配置及给定耗时下完成冻结的 100/200 条基线。

**改动：** 汇总两档公开入口的结果、累计区段计时及进程资源。使用开发片段的 3 条请求做明细开关对照。保留正式来源、测量原件和依赖明细，移除未用于采集的两份探索输入及派生计划。

**验证：** 逐请求核对生成序号和结束，明细开关的业务结果逐字节一致，开启时额外写出 40.17 MB。墙钟包含外部探针，核心循环以同一函数的进出测量，核心循环前后合计约 27 秒。按阶段起点复查 workflow 装配、控制进程和计时边界。本阶段无业务代码改动。文档中的证据均可定位，数据与本地资产保持 Git 忽略。 [完整基线](../../../data/20261118T172915.294780940Z_real-workload-performance/results/s1-speed.json)

| 请求 | 输出 token | 动作 | 工作区间 | 完成事件 | 墙钟秒 | 核心循环秒 | 核心峰值 MiB |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 100 | 10483 | 43159 | 1727886 | 2437589 | 585.14 | 557.97 | 43.8 |
| 200 | 22007 | 87966 | 3528149 | 4977059 | 1271.68 | 1244.46 | 48.9 |

**结论与限制：** 核心依赖清理累计 264.18/559.82 秒，占核心循环 47%/45%。构图 35.22/91.87 秒，依赖绑定含插入时间线 113.49/228.66 秒，合计约 3.45/3.64 ms 每动作。模板选择 2.42/5.39 秒，实例化 14.85/35.02 秒，前两项与构图存在包含关系。两档均超出反馈预算。S2 优先移除对全池历史的周期扫描，核对真实在途读写和已回收事件的所有权。

## 2026-11-19 — S2 开始：按物理地址维护依赖

**范围：** 以已推送的 `2d1b7a2` 为起点。调整执行层的页与槽位依赖历史，沿用两档测速输入和来源。

**改动：** 删除每轮规划对全部驻留页和槽位的扫描。归还时立即清除相应地址的旧内容；再次访问时只保留仍在途的读写。同一次动作中各层复用同一组请求页区间，物理页直接定位其层内记录。

**验证：** 静态追查了入图、事件回收、逻辑归还、跨参与者写入、重新分配和依赖明细。事件执行器负责在途判定，KV 池仍负责逻辑内容归还；有在途工作的地址归还后保留其历史，之后的冲突访问继续受原完成点约束。统一 Release 构建成功。三条开发请求的结果和执行明细与基线逐字节一致。 [构建日志](../../../data/20261118T172915.294780940Z_real-workload-performance/logs/s2-build.log) · [执行明细校验](../../../data/20261118T172915.294780940Z_real-workload-performance/logs/s2-detail-equal.sha256)

**结论与限制：** 小例通过依赖与事件逐条对照。两档真实负载的完整输出和反馈目标仍由本轮复测核对。

## 2026-11-19 — S2 首档复测：100 条达到反馈目标

**范围：** 复测沿用 S1 的镜像、模板、配置、100 条请求和计时口径。

**改动：** 完成 100 条复测，同一 Release 程序已启动 200 条。

**验证：** 100 条的完整结果与 S1 逐字节一致，墙钟从 585.14 秒降到 268.16 秒，核心峰值从 43.8 MiB 降到 43.1 MiB。依赖清理从 264.18 秒降到 0.43 秒，依赖绑定与入图从 113.49 秒降到 88.94 秒。 [复测墙钟](../../../data/20261118T172915.294780940Z_real-workload-performance/logs/s2-final-100.json) · [区段计时](../../../data/20261118T172915.294780940Z_real-workload-performance/logs/s2-final-100.probes)

**结论与限制：** 100 条达到 300 秒开发目标。200 条性能与结果一致性仍待终态。

## 2026-11-19 — S2 收尾：完成两档速度复测

**范围：** 对照 S1 的冻结输入、模板、镜像和计时口径，完成 100/200 条正式入口复测。

**改动：** 按实际访问和归还维护页与槽位历史。当前动作复用物理页绑定，状态所有权和实际执行因果继续由 KV 服务和时间线分别维护。

**验证：** 两档复测的完整结果和小例执行明细与基线逐字节一致。完成 Release 构建，核对所有变动文件、事件回收和调用方，业务代码净增 20 行。保留程序、测量原件及必要对照数据，文档引用和数据忽略规则已核对。 [复测报告](../../../data/20261118T172915.294780940Z_real-workload-performance/results/s2-speed.json)

| 请求 | 基线秒 | 复测秒 | 减少 | 核心峰值 MiB | 依赖清理秒 |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 100 | 585.14 | 268.16 | 54.2% | 43.1 | 0.43 |
| 200 | 1271.68 | 675.29 | 46.9% | 48.1 | 0.98 |

**结论与限制：** 200 条比 600 秒目标多 75 秒。其 647.71 秒核心循环中，构图和真实依赖绑定占 296.14 秒，剩余包括原生控制交互和事件推进。本阶段以消除全池扫描、行为一致和 100 条达标收口。后续日常用小例和 100 条检查，200 条留给阶段复测。

## 2026-11-19 — S3 开始：提取和消费逐端点时间事实

**范围：** 以已推送的 `a1005c3` 为起点。先复用原生记录 `b1-l01` 与独立旧来源验证同域动作匹配，再评估服务时长及观察扰动。

**改动：** 利用原有动作切图保存端点来源。以作用域、逻辑资源、端口和工作起止的位置匹配实际实例化子图。全量匹配后才输入共同时间线，并在明细中记录每个时间条件对应的原始端点。验证侧从原生图核对工作端点对和时钟。

**验证：** 预核对九份 TP1 记录，动作内部的资源角色均唯一。小例 `b1-l01` 有 4084 项带实测起止的工作和 150 个原生动作。动作切分有 297 个端点需重新归属，因而使用切图的来源映射定位时间。 [原生动作切口清单](../../../data/20261118T172915.294780940Z_real-workload-performance/work/s3-retained-layout.json)

**结论与限制：** 正式入口的逐工作实测回归与轻量观察对照待完成。校准范围和性能服务标签需独立验证。

## 2026-11-19 — S3 收尾：核对全节点时间与观察扰动

**范围** 使用同一镜像、设备 4、Qwen3-8B 和两请求负载各运行一次完整及轻量观测。每次额度 240 秒，分别耗时 124.75 和 113.68 秒（含启动、预热和回收）。复用独立结构来源；本轮生成的详细记录经过当前采集解释器和独立来源检查。

**改动** 全节点条件从真实起止绑定实际图，独立检查实测端点及服务区间，按已证明的纯逻辑前驱确定就绪。核心只接受本次动作的构图绑定。轻量模式卸载原生观察库及 profiler，保留实际请求接收和 token 发布计时。

**验证** Release 构建及两请求正式入口通过。独立比较覆盖 150 个动作、4,084 项工作、5,782 个端点和 683 项隔离区间，图、状态和输出一致。原生来源核对了 4,531 个主机及 1,355 个设备时间坐标；设备时间仍在校准区间内。临时移除一个工作端点时报告首处缺测，未发布条件。旧派生图的 SDK 完成时间有不一致，完整结论以新原生记录的派生图为准。 [原生来源检查](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s3-measured-work/source-qualified/source-evidence.json) · [执行对照](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s3-measured-work/comparison/actions.json) · [缺测结果](../../../data/20261118T172915.294780940Z_real-workload-performance/results/s3-missing-endpoint.json)

按变更复核原生观察、工作归属、来源映射、时间接口及产物发布。仅完整验证保留端点证据，普通执行仍复用原有时间线和构图路径。清理初步对照的重复明细，保留两次原生观测、来源核对、正式结果与首处缺测报告。

**结论与限制** 该例的详细、轻量 TTFT 分别为 4,084.82 和 3,411.08 毫秒，TPOT 分别为 390.24 和 68.68 毫秒，差异超过完整观察的发送切口宽度。原生详细数据用来验证结构，轻量数据用来评估端到端性能。一次观测不能定量解释各节点的开销。以三 task 的 Attention 片段为例，总跨度 1.292 毫秒，实际 task 服务合计 11.92 微秒。CPU 区间已区分自身工作、拷贝和等待，轻量可迁移成本仍需在 S4 取得证据。该例无 CPU 消息收发和缺测的纯逻辑节点。 [区间样本](../../../data/20261118T172915.294780940Z_real-workload-performance/results/s3-service-qualification.json) · [观察对照](../../../data/20261118T172915.294780940Z_real-workload-performance/results/s3-observation-overhead.json)

## 2026-11-19 — S4 开始：将实测工作关联到模板定义

**范围** 以已推送的 `5076900` 为起点。复用 S1 保留的开发来源，按当前解释器重新构造粗图，先盘点可归类的实际工作。

**改动** 模板编译同时给出原生工作端点与共享定义内区间的对应。成本资产按该身份聚合重复工作，再使用实例参数、形状和实际资源特征区分样本。

**验证** 已复用原生证据完成一次来源图生产。无需重新启动 Engine。归类结果及特征覆盖待核对。 [来源图生产](../../../data/20261118T172915.294780940Z_real-workload-performance/sources/s4-cost-seed/structure/result.json)

**结论与限制** 现有成本种子用于检查测量和分类。补采范围需由真实需求及资格缺口确定。

## 2026-11-19 — S4：补齐代表性组批并发布设备与搬运样本

**范围** 来源种子实际批次最多 3 路。从 100 条开发请求取 554 和 3,475 token 两档长度，围绕并发 4 和 8 各组批两次。一个预热 Engine 运行 8 组、48 个请求，额度 480 秒，实耗 157.74 秒，原始记录约 115 MB。 [采样输入](../../../data/20261118T172915.294780940Z_real-workload-performance/sources/s4-batch-coverage/scope.json) · [实际组批](../../../data/20261118T172915.294780940Z_real-workload-performance/results/s4-batch-coverage.json)

**改动** 模板编译沿原有切图映射发布每项原生工作的归一化身份。样本生产汇总同一粗区间内实际 SDK task 的服务时长，同步搬运保留原始方向和字节；同一身份和绑定特征下重复观测取中位数。

**验证** Release 构建通过。短长度命中 4/8 路 Prefill 和 Decode；长长度按原生容量分为 2 路 Prefill，随后形成 4/8 路 Decode。两份来源共发布 16,694 次设备及 10,942 次同步搬运观测，与原生粗图工作数量逐类核对。 [成本样本](../../../data/20261118T172915.294780940Z_real-workload-performance/results/s4-service-samples.json)

**结论与限制** 两请求对照的详细 Decode 阶段中，控制线程记录约 734.51 毫秒宿主工作，其中 198.65 毫秒与已标记的观察器区间重合。扣除后仍超过轻量运行完整的 137.36 毫秒，不能仅靠减掉已有标记获得 CPU 服务成本。本批样本提供设备和同步搬运基线，CPU 成本资格、形状覆盖及在线应用仍需完成。 [观察开销归属](../../../data/20261118T172915.294780940Z_real-workload-performance/results/s4-host-observer-overlap.json)

## 2026-11-19 — S4：匹配独占 CPU 区间并拟合有界成本

**范围：** 用轻量线程时钟重跑两请求开发例和已有的 48 请求、8 组来源负载，各一场。较大采样额度 300 秒，含初始化、预热和回收共用时 135.86 秒，有效观测 23.82 秒，采集与样本约 1.4 MB。 [两请求采集](../../../data/20261118T172915.294780940Z_real-workload-performance/sources/s4-native-host-cut/native/result.json) · [八组采集](../../../data/20261118T172915.294780940Z_real-workload-performance/sources/s4-host-fragments/native/result.json)

**改动：** 原生 CPU 调用按已采子调用切开，读取各段的线程 CPU 时间。只将同一切口中的完整单项 CPU 工作纳入拟合。形状坐标只保留当前作用域的张量和守卫依赖，另取实际批次成员的长度。跨来源的重复点合并观测后取中位数。

**验证：** Release 构建和公开采样、拟合入口通过。八组两种观测的 40 批一致，匹配 1,829 个 CPU 区间，覆盖 15 个归一化区间。初版成本表有 1,027 项工作被无关的槽位容量挡住，修正后两请求独立样本的设备和搬运覆盖分别为 615/616、422/424，按特征点的绝对相对误差中位数分别为 2.24% 和 6.19%。 [拟合和开发对照](../../../data/20261118T172915.294780940Z_real-workload-performance/costs/s4/measured.json) · [CPU 标签资格](../../../data/20261118T172915.294780940Z_real-workload-performance/sources/s4-host-fragments/measured-samples.json)

**结论与限制：** 原生调用匹配后仍可能包住多个粗图区间。模型主体的 CPU 提交和等待需要更具体的原生切口，异步提交线程需要自身的时钟证据。八组来源中有 43,802 项 CPU 工作尚无独立标签。局部误差只覆盖已测的设备和搬运，端到端精度尚未验证。

## 2026-11-19 — S4：定位模型内的 CPU 计时切点

**范围：** 复用两请求开发例，试采一次 CPU profiler 和一次关闭 ATen 算子记录后的原生队列。每次额度 240 秒，分别实耗 116.54 和 115.84 秒，采集目录约 1.89 和 0.58 MB。本阶段五次原生采集共计 646.89 秒。

**改动：** 仅在试验镜像调整观测开关，保持同一推理底层。比较实际批次、原生调用内的入队顺序和提交线程对应的出队。确认采样函数多一项拷贝来自 workload 注入 token，详细观察通过辅助作用域记录其归属。

**验证：** 两种试采均保留 1,633 对完整队列记录。关闭 ATen 算子记录后，108 个 Attention 与 108 个解码层调用的队列命令逐个匹配详细采集，该例 TPOT 从 86.36 降至 79.38 毫秒。轻量线程采样为 75.53 毫秒，无 profiler 及线程 probe 时为 68.68 毫秒。独立短进程验证 host perf 可在 RAW 时钟下记录线程切换。 [CPU 试采](../../../data/20261118T172915.294780940Z_real-workload-performance/sources/s4-cpu-profiler-probe/queue-evidence.json) · [队列试采](../../../data/20261118T172915.294780940Z_real-workload-performance/sources/s4-cpu-queue-probe/queue-evidence.json)

**结论与限制：** 队列事件足以定位本例主路径的提交切点。仍需在同一原生运行内核对线程占用与等待边界，试采墙钟未作为 CPU 成本标签。

## 2026-11-19 — S4 收尾：发布成本基线与完整需求缺口

**范围：** 对比 `5076900` 后所有成本采集、模板身份、特征、拟合和正式 CLI。检查来源保留、产物归属与数据忽略规则。

**改动：** 样本与需求共用逐工作身份表，读入时核对完整对应。覆盖报告从开发图直接归并全部需求，按工作身份区分未见工作和已知工作超出坐标范围。保留八组来源、两请求开发对照、试采事实和可追溯的基线资产。

**验证：** 核对真实工作与同步搬运边界、CPU 父子计时、低观测启动及不改结构的身份旁路，业务代码净增 776 行。Release 增量构建已通过，普通模板输出与 S3 程序逐字节一致，工作身份可重现。公开采样和拟合入口产生 145 项模型。两请求对照中 CPU 覆盖 77/3,044，其中 2,946 项没有模型、21 项超出范围。另一轻量运行的 32 个标量 CPU 观测全部可查询，按四类特征点的相对误差中位数为 22.13%。 [样本、拟合及需求](../../../data/20261118T172915.294780940Z_real-workload-performance/costs/s4/baseline.json)

**结论与限制：** S4 交付独立成本资产、需求范围和采样复现材料。S5 接入在线查询并覆盖剩余 CPU 服务和形状，当前缺口可从需求表定位，端到端精度仍待开发及留出对照。

## 2026-11-19 — S5 开始：核对正式计时入口及预热资格

**范围：** 从已核对远端的 `ecadfc4` 起步。复核执行结构、成本资产和原生真值入口，复用已保留的两请求试采及八组来源。

**改动：** 正常预测由任务显式选择成本资产，模板实例化交付归一化工作身份，执行前统一核对成本覆盖。将形状预热纳入本阶段，保留编译状态和重置业务 KV 各自的职责。

**验证：** 两请求试采的原生 KV 分配片段墙钟为 2,678.02 毫秒，其中调度线程实际运行 97.62 毫秒。另一会话内，4 路、2,216 token 的相同 Prefill 分配由第一次的 2,716.00 毫秒降至第二次的 16.60 毫秒。原生分配器的 Triton 调用以向上取整到 2 的幂的批次成员数和扩展 token 数进行特化。 [预热资格诊断](../../../data/20261118T172915.294780940Z_real-workload-performance/results/s5-prewarm-qualification.json) · [原生分配器](../../../third_party/sglang/python/sglang/srt/hardware_backend/npu/allocator_npu.py#L53)

**结论与限制：** 已定位首次大延迟所在的原生工作，现有预热无法为该类工作保证稳态。时间与代码支持优先检查形状首次编译，其实际耗时尚无独立记录。在线接入和稳态复测仍待完成。

## 2026-11-19 — S5：接入独立时长与形状预热

**范围：** 使用 S4 的正式成本资产、S3 两请求开发例及原有真实时间事实。预热验证在设备 4 上完成一次，额度 240 秒，原生实耗 102.82 秒，其中预热 41.43 秒，正式观测 0.86 秒。

**改动：** 模板实例化按需交付每项工作对应的定义和区间。C++ 一次装载并按选中定义索引成本资产，查询时复用作用域特征。在动作进入时间线前发布首个缺口。原生采集接受与 workload 同合同的预热输入，在同一 Engine 执行并重置业务缓存。

**验证：** Release 编译通过。两请求的真实时间结构回归覆盖 150 个动作和 4,084 项工作，结果和执行记录均与 S3 逐字节一致。独立入口在无设备、无结构真值挂载下查到 Prefill 准备的首个 CPU 缺口，保留 `cost-gap.json`。同形状预热后，原生两请求 TTFT 从 3,411.08 变为 715.28 毫秒，多 token 请求的 TPOT 为 72.08 毫秒，两次均生成 4 个 token。 [两条入口证据](../../../data/20261118T172915.294780940Z_real-workload-performance/results/s5-timing-entry.json) · [形状预热对照](../../../data/20261118T172915.294780940Z_real-workload-performance/results/s5-shape-warmup.json)

**结论与限制：** 在线时间来源和缺口交接已验证。两请求预热只证明该形状的首次开销可移出测量窗口，尚需对一般形状识别首次特化。原生队列切点及 CPU 自身服务的统一采样仍是完整预测的主要缺口，端到端精度尚待验证。

## 2026-11-19 — S5 采样线程活动并收紧直接设备提交的边界

**范围** 同形状预热后，两请求 CPU 试采共两次，各计入 240 秒额度。第一次因切换记录丢失失败，耗时 164.26 秒。改为持续读取后完成，耗时 109.52 秒，其中预热 45.01 秒、观测及收尾 1.33 秒，完整保留约 2.4 MB。 [首次采集记录](../../../data/20261118T172915.294780940Z_real-workload-performance/sources/s5-host-activity/native/execution.json) · [成功采集](../../../data/20261118T172915.294780940Z_real-workload-performance/sources/s5-host-activity-buffered/native/result.json)

**改动** 将调用、队列及 runtime 的实测切点交到同一线程活动计时，扣除等待、独立搬运和有记录的观测开销。模型内两段 CPU 工作被同线程的 SDK 包装入口隔开，直接提交统一以驱动实际提交为界，异步队列继续保留调用方入队依赖。发现 CPU-only profiler 的启动换算存在约 4 微秒偏移后，增加同次观测前后的独立计时夹点，删除旧线程时钟字段。

**验证** 成功记录的控制和提交线程均无丢失，队列完整配对 1,633 项，未发现新 Triton 特化。用同一原始记录重建后，试算匹配 2,847／2,936 项 CPU 工作。新版图独立对照通过 150 个动作、3,976 项工作、5,674 个端点和全部 616 个设备提交，分配及请求结果与原回归一致。正式独立入口在 `ScheduleBatch.prepare_for_extend` 的提交线程缺成本处停止。原生桥 Release 构建和主机时钟夹点检查通过。 [CPU 试算](../../../data/20261118T172915.294780940Z_real-workload-performance/costs/s5/direct/host.json) · [结构回归](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s5-direct-submission/comparison/actions.json) · [首处成本缺口](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s5-independent-live/prediction/cost-gap.json) · [主机时钟](../../../data/20261118T172915.294780940Z_real-workload-performance/results/s5-cpu-queue-clock.json)

**结论与限制** CPU 试算使用当时记录的换算，新校准仍需在正式观测内验证。剩余缺口集中在采样与结构的分段入口，以及队列和驱动的时间对齐。两档开发流量与留出对照仍待完整独立输出。

## 2026-11-19 — S5：统一原生切点并打通两请求独立预测

**范围：** 两请求和已保留的 48 请求八组来源各采一次 CPU 时间，分别用 240 和 360 秒额度。实际共耗 258.55 秒，预热分别 44.50 和 63.33 秒，观测及收尾分别 1.36 和 24.09 秒，保留约 2.47 和 55.66 MB。 [两请求采集](../../../data/20261118T172915.294780940Z_real-workload-performance/sources/s5-host-shared-cuts/native/result.json) · [八组采集](../../../data/20261118T172915.294780940Z_real-workload-performance/sources/s5-host-representative/native/result.json)

**改动：** 结构侧与轻量侧共用调度、KV、主机读取和模型切点，轻量侧直接记录原生作用域。相同 workload 预热复用原有请求分组。局部操作按自身参数取成本，移除通用阶段回退。按相同出现次数和顺序配对运行时端点，保留无法对应的实际观测。

**验证：** C++23 Release 构建通过。两个案例分别配对 1,633 和 24,391 项原生队列记录，观测内的 RAW 校准范围分别为 −1,300 至 −1,230 ns 和 −2,700 至 −2,650 ns。取得全部 2,936 和 44,191 项 CPU 工作时长。正式预测完成两请求与 4 个 token，运行时查询 3,976 次，累计 2.54 ms。与单独的轻量原生运行比较，TTFT MAPE 为 1.18%（两条），TPOT MAPE 为 0.45%（一条）。 [时长证据](../../../data/20261118T172915.294780940Z_real-workload-performance/costs/s5/scope-features/representative-host.json) · [性能对照](../../../data/20261118T172915.294780940Z_real-workload-performance/results/s5-two-request-performance.json)

**结论与限制：** 两请求的两条正式路径均完成。该例的 CPU 样本覆盖同一 workload 的独立运行，精度数字仅支持开发回归。两份现有模板来源在单成员、不同长度及部分原生操作上仍有 CPU 成本缺口，按需求表扩展采样。 [开发成本覆盖](../../../data/20261118T172915.294780940Z_real-workload-performance/costs/s5/scope-features/development-costs.json)

## 2026-11-19 — S5：补齐模板来源样本并检查真实流量

**范围：** 对 11 请求主模板来源和 4 请求局部来源各做一次轻量 CPU 采样，各计入 240 秒额度。实耗共 250.96 秒，正式观测分别为 11.38 和 2.01 秒，保留约 27.50 和 3.55 MB。原始结构记录沿用两份已保留来源。 [主来源采样](../../../data/20261118T172915.294780940Z_real-workload-performance/sources/s5-host-template-sources/0/native/result.json) · [局部来源采样](../../../data/20261118T172915.294780940Z_real-workload-performance/sources/s5-host-template-sources/1/native/result.json)

**改动：** 用原生实测时长补齐两份来源的 37,935 项 CPU 工作，合并四份开发数据并发布 549 项模型。指标示例接收独立预测结果，按实际 token 发布核对 TTFT、TPOT 和吞吐。

**验证：** 四份开发图的已见工作全部通过成本覆盖检查。Azure 3 请求片段在两档并发均完成 44 个 token，两边各执行 42,573 次成本查询。100 请求片段分别在 24.01 和 24.49 秒报告首个范围缺口，并发 4 的 Prefill 分配为 3,015 token，样本上限为 2,609，并发 8 的批次合并右侧为 7 个请求，样本上限为 2。 [合并成本](../../../data/20261118T172915.294780940Z_real-workload-performance/costs/s5/scope-features/expanded-costs.json) · [并发 4 三请求](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s5-azure-development-3/concurrency-4/prediction/result.json) · [并发 8 三请求](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s5-azure-development-3/concurrency-8/prediction/result.json)

**结论与限制：** 新工作负载已通过小例预测闭环。100 请求的下一个采样范围由分配分支的有效长度和真实批次合并的成员分布决定。两次停止时间用于定位首个缺口，完整性能和墙钟仍待取得。 [并发 4 缺口](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s5-azure-development-100/concurrency-4/prediction/cost-gap.json) · [并发 8 缺口](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s5-azure-development-100/concurrency-8/prediction/cost-gap.json)

## 2026-11-19 — S5：覆盖批次合并并核对批次退出需求

**范围：** 48 请求分八组，在设备 4 分别采详细结构和轻量 CPU。共两次，累计额度 720 秒，实耗 345.47 秒，有效观测分别为 34.18 和 20.77 秒，原生记录合计约 148 MiB。 [采样范围](../../../data/20261118T172915.294780940Z_real-workload-performance/sources/s5-boundary-coverage/scope.json) · [详细结构](../../../data/20261118T172915.294780940Z_real-workload-performance/sources/s5-boundary-coverage/detailed/result.json) · [轻量 CPU](../../../data/20261118T172915.294780940Z_real-workload-performance/sources/s5-boundary-coverage/host/result.json)

**改动：** 同一容量下取得小于 200 页的分配上界、合并人数两端和长上下文。观测到 1+7 与 7+1 合并。纳入 40,163 项 CPU、8,153 项设备工作和 5,870 项同步搬运时长。 [CPU 样本](../../../data/20261118T172915.294780940Z_real-workload-performance/costs/s5/scope-features/boundary-host.json) · [设备与搬运](../../../data/20261118T172915.294780940Z_real-workload-performance/costs/s5/scope-features/boundary-device.json)

**验证：** 五份开发图的已见工作均被成本资产覆盖。100 请求两档越过已有缺口，分别于 24.40 与 24.70 秒在同一过滤操作停下，缩减规模为 4→1 和 8→5。已有部分退出样本的输入批次最多 3 人，存留最多 2 人。 [并发 4 缺口](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s5-azure-development-100-bounded/concurrency-4/prediction/cost-gap.json) · [并发 8 缺口](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s5-azure-development-100-bounded/concurrency-8/prediction/cost-gap.json)

**结论与限制：** 同长同输出组覆盖入批与同步退出，遗漏在途批次部分请求陆续完成的形状。下一轮在固定目标池内改变输出长度，覆盖过滤前后人数、页分配分界和在途长上下文。复测尚未产出完整的真实流量性能结果。 [分批退出配方](../../../data/20261118T172915.294780940Z_real-workload-performance/sources/s5-cohort-coverage/scope.json)

## 2026-11-19 — S5：测量分批退出并统一拟合容量

**范围：** 固定 32,768 token 的 KV 池。同一串 55 条开发配方分九组，详细结构和轻量 CPU 各采一次，累计额度 1,080 秒，实耗 358.83 秒，原生记录约 270 MiB。 [配方](../../../data/20261118T172915.294780940Z_real-workload-performance/sources/s5-cohort-coverage/scope.json) · [详细运行](../../../data/20261118T172915.294780940Z_real-workload-performance/sources/s5-cohort-coverage/detailed/result.json) · [轻量运行](../../../data/20261118T172915.294780940Z_real-workload-performance/sources/s5-cohort-coverage/host/result.json)

**改动：** 结合三份同容量来源拟合 549 项成本模型，局部参数与批次组合均使用各工作自身的记录。轻量真值复用 CPU 采样已有的 Triton 特化记录，保留业务窗口内新加载的特化，以核对稳态。 [固定容量资产](../../../data/20261118T172915.294780940Z_real-workload-performance/costs/s5/scope-features/target-pool-costs.json)

**验证：** 实际覆盖 4→1、8→1、8→7 等部分退出，最大 Decode 合计 32,632 token。详细结构和轻量 CPU 对应到 86 个批次，取得 79,231 项 CPU 时长，全部排定的工作都有样本。固定容量资产覆盖三个来源的 220,487 项已见工作及四处已报告的范围缺口。新版轻量原生镜像保持原生框架与 SDK 版本，已启动 100 请求性能测量。 [CPU 样本](../../../data/20261118T172915.294780940Z_real-workload-performance/costs/s5/scope-features/cohort-host.json) · [测量范围](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s5-azure-development-100-native/scope.json)

**结论与限制：** 开发拟合使用同一 KV 容量，避免从前几轮混合容量选择长度的间接指标。真实 100 请求对新覆盖与模型误差的检验仍在运行。

## 2026-11-19 — S5：分开自身工作特征与外层计算量

**范围：** 两次预测均通过批次部分退出，分别于 75.50 与 40.59 秒因 7,194 token 的长分配停止。并发 8 另行轻量实测 100 请求一次，额度 1,200 秒，实耗 486.87 秒，其中预热 252.87 秒，观测及收尾 174.09 秒。 [长分配缺口](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s5-azure-development-100-pool/concurrency-8/prediction/cost-gap.json) · [原生运行](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s5-azure-development-100-native/native/result.json)

**改动：** 分配器按实际请求和页处理，外层 Attention 计算量无对应输入。采样与在线查询均由子图的局部 KV 读取决定是否加入上下文及 Attention 特征。拟合按工作已声明的容量坐标约束样本，无此坐标的工作复用已有同定义观测。 [特征提取](../../../src/markov/performance/work_features.cpp) · [拟合](../../../src/workflow/markov_workflow/fit_costs.py)

**验证：** C++23 Release 构建通过。重新生成六份图的身份表，规范定义、物理区间及采集窗口逐项一致，沿原身份复用全部时长。549 项新版模型覆盖三份目标容量图的 220,487 项已见工作。并发 8 原生实测完成 100 条、10,483 token，业务窗口无新 Triton 特化，吞吐为 60.22 token/s。 [新版成本](../../../data/20261118T172915.294780940Z_real-workload-performance/costs/s5/work-geometry/assignment.json) · [原生指标](../../../data/20261118T172915.294780940Z_real-workload-performance/results/s5-azure-development-100-native.json)

**结论与限制：** 同一长分配定义现有 7,207 token 的原生样本可以覆盖该范围。原生入口相对声明到达的偏差中位数为 433.22 毫秒，P95 为 885.17 毫秒，作性能偏差归因时独立考虑。新版两档完整预测仍在运行。

## 2026-11-19 — S5：按数据端口与实际页访问确定工作特征

**范围：** 两档越过长分配，分别于 219.47 和 141.23 秒到达同一处短 Prefill 缺口。56 token 已在旧采样的长度范围内，固定 KV 容量轴后，同池样本最短为 128 token。 [并发 4 证据](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s5-azure-development-100-geometry/concurrency-4/prediction/cost-gap.json) · [并发 8 证据](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s5-azure-development-100-geometry/concurrency-8/prediction/cost-gap.json)

**改动：** 按数据端口与 guard 取工作形状，KV 容量元数据继续用于图结构和绑定。Attention 的原生调用按实际页表、上下文及最大 query 长度表达访问，成本提取沿相同原则复用小形状观测。移除试用的固定成本坐标选项。 [特征提取](../../../src/markov/performance/work_features.cpp)

**验证：** Release 构建通过，复用原采集重建六份工作身份表，区间归属和原生时长保持一致。549 项模型覆盖四份图的 224,463 项已见工作，Prefill Attention 的输入范围为 15–8,176 token。 [成本资产与需求](../../../data/20261118T172915.294780940Z_real-workload-performance/costs/s5/data-port-geometry/assignment.json)

**结论与限制：** 已直接核对短 Prefill 工作的特征范围。两档全量开发预测已开始，新的端到端结果待发布。

## 2026-11-19 — S5 收尾：完成真实开发流量两档预测

**范围：** 复用独立模板和已发布成本，运行 Azure 前 100 条的两档预测。并发 8 使用另一次预热后的原生轻量记录评分。

**改动：** TTFT 统一到请求提交至调度器首 token 发布。原生使用实际提交时间，仿真使用声明时间。原生调度器取入时间单独保留，提交至取入的等待在 TTFT 内计入。原先混合两种起点得到 8.60% 的 TTFT MAPE。 [原口径证据](../../../data/20261118T172915.294780940Z_real-workload-performance/results/s5-azure-development-100-accuracy.json)

**验证：** 两档各完成全部 100 条、10,483 token，逐请求核对生成步和结束标记。并发 8 墙钟为 270.04 秒，S2 同配置给定时长为 268.16 秒，两次分别处理 166.49 万和 172.79 万项工作。两档查时累计 2.82 和 1.75 秒，占墙钟 0.82% 和 0.65%。独立两请求图的 616 项设备和 424 项同步搬运均有成本，逐特征点相对误差中位数为 2.00% 和 4.65%。 [阶段证据](../../../data/20261118T172915.294780940Z_real-workload-performance/results/s5-development-closure.json)

**结论与限制：** 并发 8 的开发 TTFT、TPOT MAPE 为 3.93% 和 4.65%，各 100 条。实测和预测吞吐分别为 60.22 和 57.84 token/s。阶段审查覆盖原生采样、模板身份、模型装载、时间线前查询、输出发布及外围评价，业务变更净增 1,110 行，示例净增 62 行。发布保留复现输入、证据及开发失败，数据留在 Git 之外。性能正常路径的验证范围是单实例 dense TP1，独立留出与全窗口验收进入下一阶段。

## 2026-11-19 — S6 开始：冻结四个留出组合

**范围：** 核对远端交付 `759fe37`。常态和突发流量各 48 条，分别验证并发 4 和 8。详细采集与轻量时延各一次每例，预算为 1,800 和 1,200 秒每例，使用设备 4，保留空间上限 128 GiB。 [冻结清单](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/freeze.json)

**改动：** 登记程序、镜像、两份模板、成本、输入和评价代码的摘要。原生同例预热并清空业务 KV 后采集，独立成本预测仅挂载冻结的必要输入。外围启动检查先行创建了预测输出目录，公开入口在起步即拒绝。修正挂载至输出父目录，保留启动检查记录。

**验证：** 复核留出与开发段无重叠，且原生详细采集和独立预测均进入首批实际运行。首个正常预测结束于常态并发 8 的分配工作。请求合计 7,419 token，当前对应模型的样本上限为 7,207。 [采集账目](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/normal/concurrency-8/native-detailed/execution.json) · [成本缺口](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/normal/concurrency-8/cost/prediction/cost-gap.json)

**结论与限制：** 首个失败是冻结成本覆盖差距，结构分支和其余组合仍在收集。完整矩阵前保留冻结资产和既有结论。

## 2026-11-19 — S6：收齐冻结成本预测分支

**范围：** 四组独立成本运行分别发布成功结果或首处缺口。继续在同一卡上完成详细原生采集。

**改动：** 汇总四组冻结结果，准备复用原生投影、逐端点计时和独立对照入口的结构评估脚本。冻结业务和评价代码。

**验证：** 突发并发 4 完成 48 条请求与 5,344 token，逐请求校验生成步及结束标记，墙钟 179.96 秒。常态并发 4、常态并发 8 与突发并发 8 的首处缺口均为相同的原生分配工作，实际输入分别为 7,404、7,419 与 7,850 token，样本上限为 7,207。 [四组成本结果](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/cost-results.json)

**结论与限制：** 三处均已选择对应结构模板，差距在该成本定义的实测长度范围。原生及结构对照尚未收齐，四组精度和配置排序待核对。

## 2026-11-19 — S6：封存首组详细原生记录

**范围：** 常态并发 8 完成一次详细采集，预热 188.03 秒、正式观测与收尾 634.25 秒，共 888.68 秒。常态并发 4 开始原生运行。 [首组采集结果](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/normal/concurrency-8/native-detailed/result.json)

**改动：** 由采集逐例发布触发粗图和全节点检查。检查器在冻结的控制镜像内求解时钟，修正外围输出目录的写入挂载。轻量采集结束后复用冻结评估器汇总请求数、提交偏差、特化加载、误差和配置排序。

**验证：** 首份原始记录约 2.4 GiB，已完成 SDK 导出并进入解释阶段。复核同一分配器的样本按完整工作定义归属，8,176 token 已测的是 50 个工作区间的变体，三组预测缺口对应 80 个工作区间的变体，最长合格样本 7,207 token。 [冻结成本资产](../../../data/20261118T172915.294780940Z_real-workload-performance/costs/s5/data-port-geometry/assignment.json) · [解释日志](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/normal/concurrency-8/structure/observed/logs/002-0-interpret.log)

**结论与限制：** 已发布的详细记录可供独立检验。全节点资格需等待时钟校准和结构对照结果。

## 2026-11-19 — S6：核对大批次分配的采样来源

**范围** 常态并发 4 的详细采集在 1,168.53 秒后发布，突发并发 8 开始。常态并发 8 的粗图已发布，进入来源证据检查。 [并发 4 采集账目](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/normal/concurrency-4/detailed-run.json) · [首组粗图账目](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/normal/concurrency-8/structure/observed/result.json)

**改动** 在首份粗图上使用正式工作身份导出与需求扫描，离线检查原生组批产生的各类工作能否查询冻结成本，后处理额度 1,800 秒。

**证据** 独立开发配方尝试同时提交八条 1,022 token 请求。页面为 16，向上取整后合计正好 8,192 token。原生在批次已有请求时拒绝触及该上限的候选，粗图只观察到七条一组、合计 7,154 token 的长分配。上限样本中的 8,176 token 来自整页分配。三组首处缺口对应同一实现中同时写完整页与末尾部分页的成员。 [采样配方](../../../data/20261118T172915.294780940Z_real-workload-performance/sources/s5-cohort-coverage/scope.json) · [原生准入](../../../third_party/sglang/python/sglang/srt/managers/schedule_policy.py#L919)

**验证** 常态并发 8 的粗图共扫描 1,267,071 项工作，CPU 946,773 项、搬运 95,015 项、设备 225,283 项均在冻结成本范围内。使用相同独立样本重算得到的定义和模型与冻结资产一致。该结论只覆盖这次详细观测实际产生的组批。自主预测在合计 7,419 token 的分配批次早停，仍需覆盖该路径可产生的工作。 [观测需求报告](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/normal/concurrency-8/cost/observed-demand/coverage.json) · [自主预测缺口](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/normal/concurrency-8/cost/prediction/cost-gap.json)

**结论与限制** 采样需按原生实际组批及成员分支核对可达范围。在另一份独立来源中，同一批内 3,199 和 29 token 的两个成员命中同一 80 区间定义，并共享整批特征坐标。原生循环按单成员计算写入范围，后续需核对子区间成本与成员实际写入长度的关系，再决定补采和特征修正。冻结资产及结果口径未变。 [来源工作身份](../../../data/20261118T172915.294780940Z_real-workload-performance/costs/s5/data-port-geometry/source-0-work.json) · [原生分配循环](../../../third_party/sglang/python/sglang/srt/mem_cache/allocator/paged.py#L65)

## 2026-11-19 — S6：并行处理已封存的来源

**改动** 常态并发 4 开始后处理，两个突发案例在各自采集封存后即可开始。外围编排按例互斥并复用已发布结论。设备采集保持顺序，轻量性能测量仍在结构后处理完结后启动。 [并发 4 后处理](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/normal/concurrency-4/structure/produce.log)

**验证** 两组常态详细采集各提交全部 48 条。计划到达时刻与输入逐条一致，并发 4 和并发 8 的最大提交偏差分别为 1.50 和 5.83 毫秒。 [并发 4 提交记录](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/normal/concurrency-4/native-detailed/attempts/1/source/instances/0/request-delivery.json) · [并发 8 提交记录](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/normal/concurrency-8/native-detailed/attempts/1/source/instances/0/request-delivery.json)

**结果** 突发并发 8 详细采集在 848.80 秒后发布，离线后处理已启动。首组来源检查覆盖 1,267,071 项工作和 1,829,199 个事件，原生整组一致。在 413 个时钟校准样本下检查 4,812,204 条因果时间约束，报告为相容且无时间缺测约束。动作布局已导出，回放的条件提取和实际消费仍待核对。 [首组来源检查](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/normal/concurrency-8/structure/source-evidence/source-evidence.json) · [突发并发 8 采集](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/burst/concurrency-8/detailed-run.json)

**执行调整** 首组时间条件提取的原建模镜像缺少求解依赖。核对两份冻结镜像后，结束该提取进程，保留 275.24 秒消耗与原始状态。控制镜像重新提取时直接复用已通过的来源检查、粗图及动作布局。 [镜像检查](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/normal/concurrency-8/structure/clock-environment.json) · [原执行记录](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/normal/concurrency-8/structure/qualification.json)

## 2026-11-19 — S6：首组工作时间条件提取完成

**结果** 常态并发 8 提取了 33,166 个控制步骤及 1,267,071 项工作的实测时间条件，工作结束缺测为零。1,805,298 个实测端点中有 494,482 个保留校准不确定性。以实测校准点为依据取得一套满足全图因果的可行时钟映射。正式仿真已启动，仍需独立核对条件消费和结构。 [提取报告](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/normal/concurrency-8/structure-evaluated/timing/execution-inputs-report.json)

**执行调整** 常态并发 4 的 SDK 跟踪约 4.90 GiB，首组约 3.44 GiB 的跟踪形成粗图实耗 1,348.87 秒。根据这个规模增幅，突发并发 4 尚未启动的离线投影累计额度由 1,800 秒提高到 2,700 秒，用于完成 SDK 导出、解释和投影。已启动的各步继续按原额度运行。

## 2026-11-19 — S6：详细采集封存及首组回放完成

**范围** 突发并发 4 的详细原生采集在 1,141.67 秒后发布，离线投影按 2,700 秒额度启动。四组各一次详细采集，合计墙钟 67.46 分钟。逐例核对发布来源、请求长度和声明到达，最大提交偏差为 5.83 毫秒。 [末组详细采集](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/burst/concurrency-4/detailed-run.json)

**验证** 常态并发 8 的实测时间回放完成 48 条请求、4,953 个输出 token，逐请求核对生成位置及结束标记。回放消费 31,990 个调用条件与 1,176 次输入读取条件，共执行 1,267,071 项工作和 1,805,298 个事件。独立评估器开始核对时间消费、必要依赖和资源效果。 [首组回放结果](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/normal/concurrency-8/structure-evaluated/core/prediction/result.json)

**执行调整** 常态并发 4 生成粗图时在解释步骤耗尽原 1,800 秒额度。保留失败结论及实耗 1,803.19 秒，使用已有 SDK 导出，从解释步骤续跑。累计额度调整为 3,300 秒，已耗时间继续计入。 [首次结果](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/normal/concurrency-4/structure/qualification.json) · [续跑账目](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/normal/concurrency-4/structure/produce-resume.log)

**覆盖诊断** 突发并发 8 的 1,180,757 项实测工作中有 480 项超出冻结成本范围。缺口全部来自实测的同一个包含 7 条请求、合计 7,850 token 的分配批次。六个成员各产生对应的 80 项工作，CPU、搬运和设备分别缺 324、84 和 72 项。比对完整模板定义确认与自主预测的首处缺口同源。 [突发工作覆盖](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/burst/concurrency-8/cost/observed-demand/coverage.json)

**验证** 突发并发 8 的原生粗图来源检查通过，核对了 1,180,757 项工作、1,704,511 个事件及 4,483,635 条有测量依据的因果时间约束。385 个时钟校准样本的约束相容。突发并发 4 已完成 SDK 导出，跟踪约 4.63 GiB。 [突发并发 8 来源检查](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/burst/concurrency-8/structure/source-evidence/source-evidence.json) · [突发并发 4 离线处理](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/burst/concurrency-4/structure/observed/production.json)

**评估额度** 首组独立对照运行 14 分 29 秒时读过约三分之一的 1.41 GB 轨迹。新启动的结构核对进程将独立对照额度设为 3,600 秒，以覆盖轨迹读取和后续依赖、状态检查。原生采集及已启动进程按现有额度执行。

## 2026-11-19 — S6：启动独立轻量时延测量

**执行调整** 四组详细采集已经结束，设备 4 无活跃进程。主机抽样 CPU 空闲率为 93%–94%，离线处理可与原生测量并行。轻量采集仍在同一卡上逐组执行，每组一次、1,200 秒额度。正式入口检查到首组已启用轻量模式。采集程序复用已封存结果，最终汇总等待结构与时延结果齐备。 [首组轻量采集账目](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/normal/concurrency-8/native-latency/execution.json)

## 2026-11-19 — S6：首组全节点结构对照通过

**验证** 常态并发 8 的 31,990 次调用逐项对应，内容与依赖全部匹配，双方未配对调用数为零。独立评估确认 1,267,071 项工作和 1,805,298 个执行事件的实测时间条件完整消费，89 项在途状态冲突均有依赖保护。结果覆盖该组 48 条请求的约定粗图及执行范围。 [独立对照](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/normal/concurrency-8/structure-evaluated/comparison/actions.json)

**用时** 实测时间提取 595.17 秒，回放 248.41 秒，独立对照 1,551.13 秒。该组的自主预测性能结论仍受冻结成本覆盖限制。 [步骤账目](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/normal/concurrency-8/structure-evaluated/qualification.json)

**其余组合** 突发并发 8 完成 1,180,757 项工作的时间条件提取，工作结束缺测为零，回放已启动。常态并发 4 复用原 SDK 导出完成粗图，累计离线投影耗时 2,944.16 秒，进入来源核对和工作成本覆盖扫描。 [突发提取报告](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/burst/concurrency-8/structure/timing/execution-inputs-report.json) · [常态粗图账目](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/normal/concurrency-4/structure/observed/result.json)

**轻量真值** 常态并发 8 轻量实测完成 48 条及 4,953 个输出 token，墙钟 367.63 秒，最大提交偏差 1.23 毫秒。正式观测内加载了一项 `alloc_extend_kernel` Triton 特化，按冻结口径记为稳态资格未通过。其原始 P95 TTFT 为 64.12 秒、平均 TPOT 为 133.53 毫秒，保留用于后续归因。常态并发 4 已开始轻量测量。 [轻量时延及特化记录](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/normal/concurrency-8/native-latency/attempts/1/source/instances/0/latency.json)

## 2026-11-19 — S6：封存四组粗图及第二次回放

**验证** 突发并发 4 的离线投影在 1,894.54 秒后完成，四组粗图均已发布。突发并发 8 逐请求核对了 48 条及 5,344 个输出 token 的完成与结束标记，回放使用了 1,180,757 项工作的实测时间条件。独立结构对照正在运行。 [突发并发 4 粗图账目](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/burst/concurrency-4/structure/observed/result.json) · [突发并发 8 回放结果](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/burst/concurrency-8/structure/core/prediction/result.json)

**覆盖诊断** 常态并发 4 的 1,811,188 项原生工作中有 160 项超出冻结成本范围。同一组 2 条请求、合计 7,404 token 的分配产生了这些工作。完整模板定义与突发并发 8 的 480 项以及三组自主预测首处报告的缺口一致。 [常态并发 4 覆盖](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/normal/concurrency-4/cost/observed-demand/coverage.json)

**预热证据** 常态并发 8 的预热与实测共用同一份请求及到达时间。原生组批仍根据当时的队列与完成状态决策。分配算子的特化取决于实际批次成员数、页宽和扩展 token 数，观测在正式窗口开启后 66.40 秒记录到新的特化。现有轻量观测记录到加载时刻，具体开销及预热对该特化的命中情况仍待归因。 [原生特化参数](../../../third_party/sglang/python/sglang/srt/hardware_backend/npu/allocator_npu.py#L57) · [正式窗口测量](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/normal/concurrency-8/native-latency/attempts/1/source/instances/0/latency.json)

**轻量真值** 常态并发 4 实耗 441.48 秒，完成 48 条及 4,953 个输出 token。正式窗口无新增 Triton 特化，最大提交偏差 1.28 毫秒，原生 P95 TTFT 为 105.82 秒、平均 TPOT 为 95.82 毫秒、输出吞吐为 32.07 token/s。其稳态真值合格，冻结成本预测还缺少完整输出。突发并发 8 已开始测量。 [常态并发 4 原生时延](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/normal/concurrency-4/native-latency/attempts/1/source/instances/0/latency.json)

**轻量真值** 突发并发 8 实耗 359.63 秒，完成 48 条及 5,344 个输出 token，正式窗口无新增 Triton 特化。原生 P95 TTFT 为 61.97 秒、平均 TPOT 为 128.44 毫秒、输出吞吐为 47.19 token/s，最大提交偏差 1.98 毫秒。突发并发 4 已开始测量。 [突发并发 8 原生时延](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/burst/concurrency-8/native-latency/attempts/1/source/instances/0/latency.json)

**覆盖诊断** 突发并发 4 的 1,705,966 项原生工作均可查询冻结成本。四组原生粗图的覆盖扫描全部完成，所见 640 项缺口来自同一个长分配模板定义。汇总程序一并记录原生实际组批的覆盖和自主预测的覆盖。 [突发并发 4 覆盖](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/burst/concurrency-4/cost/observed-demand/coverage.json)

## 2026-11-19 — S6：封存轻量测量和首次留出评分

**范围** 四组轻量测量各完成 48 条，合计运行墙钟 1,598.79 秒。突发并发 4 实耗 430.05 秒，测量窗口无新增 Triton 特化，最大提交偏差 1.78 毫秒。四份逐请求时延报告已发布，三组实测通过稳态检查。 [突发并发 4 时延报告](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/burst/concurrency-4/latency-metrics.json)

**验证** 突发并发 4 是本轮唯一具备合格真值和完整自主预测的组合。48 条请求的 TTFT 和 TPOT MAPE 分别为 5.19% 和 8.22%，无零分母。TTFT 在 48 条上均为高估。TPOT 的绝对误差中位数为 5.21%，输出仅 5、9、11 token 的三条请求贡献了约 42.7% 的绝对相对误差总和。均值误差不足以描述逐请求差异。四组汇总评分仍缺三个有效对照。 [逐请求对照](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/burst/concurrency-4/latency-metrics.json)

**偏差切口** 这三条请求的首末 token 发布区间内，其他请求的首发或结束与预测均有差异。例如 5 token 请求的实测首末间隔为 1.025 秒，其间另一条请求发布了首 token。预测间隔为 0.310 秒，其间没有其他请求首发。当前证据定位到发布次序，仍需核对组批、排队和服务耗时各自的贡献。 [逐请求对照](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/burst/concurrency-4/latency-metrics.json)

**配置比较** 突发流量下，并发 8 的原生 P95 TTFT 更低，输出吞吐更高，并发 4 的平均 TPOT 更低。自主预测的双配置比较仍缺并发 8 的完整输出。 [并发 8 实测](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/burst/concurrency-8/latency-metrics.json) · [并发 4 实测和预测](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/burst/concurrency-4/latency-metrics.json)

**结构证据** 两个并发 4 案例均通过来源检查。常态和突发分别核对 1,811,188 和 1,705,966 项工作，因果时间约束无缺测且与时钟校准相容。四组原生粗图至此都通过来源检查。 [常态并发 4 来源检查](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/normal/concurrency-4/structure-evaluated/source-evidence/source-evidence.json) · [突发并发 4 来源检查](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/burst/concurrency-4/structure/source-evidence/source-evidence.json)

**独立对照** 突发并发 8 通过全节点结构对照，29,879 个动作的内容与依赖逐项匹配。1,180,757 项工作和 1,682,211 个执行事件的实测条件完整消费。独立对照实耗 1,393.23 秒，目前两组并发 8 均已通过。 [突发并发 8 全节点对照](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/burst/concurrency-8/structure/comparison/actions.md)

**代码核对** 两个分配问题出现在不同的原生分支。新分配页数达到 200 时进入长分配，其工作定义出现成本样本范围缺口。新页少于 200 时调用 Triton 分配核，实测窗口内加载的特化属于这一路。当前页宽为 16、预填充预算为 8,192，多请求预填充的按页需求上限为 8,176 token。对无前缀命中的采样批次，若至少一个成员还需写末尾部分页，整批原始 token 数最多为 8,175。单请求在预算检查中有独立准入路径。上述边界来自原生代码，采样仍需核对实际组批和工作。 [原生分配](../../../third_party/sglang/python/sglang/srt/hardware_backend/npu/allocator_npu.py#L43) · [原生准入](../../../third_party/sglang/python/sglang/srt/managers/schedule_policy.py#L920)

**时间证据** 常态并发 4 提取了 46,724 个控制步骤、2,580,411 个端点和 1,811,188 项工作的实测时间条件，工作结束缺测为零。正式回放已启动。突发并发 4 正在提取时间条件。 [常态并发 4 提取报告](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/normal/concurrency-4/structure-evaluated/timing/execution-inputs-report.json)

## 2026-11-19 — S6：四组实测时间条件齐备

**验证** 常态并发 4 的回放完成 48 条及 4,953 个输出 token，逐请求核对生成位置和结束标记。独立对照已按 3,600 秒额度启动。突发并发 4 提取了 44,112 个控制步骤、2,430,568 个端点及 1,705,966 项工作的实测时间条件，工作结束缺测为零，回放正在运行。 [常态回放结果](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/normal/concurrency-4/structure-evaluated/core/prediction/result.json) · [突发时间条件](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/burst/concurrency-4/structure/timing/execution-inputs-report.json)

**评估额度** 以已完成两组的对照耗时按工作量估算，突发并发 4 的独立对照约需 2,013–2,088 秒。外围编排已安排回放完成后接续 3,600 秒对照，直接使用原生图和已发布的执行轨迹。矩阵汇总等待该对照结束。

**回放封存** 突发并发 4 完成全部 48 条请求及 5,344 个输出 token，逐请求生成位置和结束标记均相符。独立对照由原 1,800 秒进程在启动后 0.53 秒交接，3,600 秒对照已经运行。命令核对确认仅更改额度及输出路径，原生粗图、回放轨迹、程序和镜像按冻结范围复用。 [回放结果](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/burst/concurrency-4/structure/core/prediction/result.json) · [额度交接记录](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/burst/concurrency-4/structure/comparison-budget.json)

**墙钟账目** 矩阵汇总累计离线各步骤实际用时，包含中断后续跑的已付开销，复用的步骤只计一次。对照已封存账目，常态、突发并发 8 的离线结构步骤分别累计 4,696.93 和 4,413.16 秒。两组现场采集及自主预测分别记录。

## 2026-11-19 — S6：核对实际请求顺序

**验证** 四组轻量记录中有三组出现实际提交反序。突发并发 4 的 `azure-67607` 按输入应早 161 微秒提交，实际晚了 339 微秒。两条被原生调度器同轮取入，首 token 发布顺序与预测相反。这也是两边首末发布记录的第一处顺序差异。汇总已用四组实测核对反序报告。 [请求投递记录](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/burst/concurrency-4/native-latency/attempts/1/source/instances/0/request-delivery.json) · [发布顺序分析](../../../data/20261118T172915.294780940Z_real-workload-performance/results/s7-burst-c4-publications.json)

**归因边界** 原生驱动为各次提交独立等待，到达间隔小于调度抖动时允许反序。首末 token 之间出现不同发布事件的 11 条请求，TPOT MAPE 为 20.16%，其余 37 条为 4.68%。轻量记录只保留首末发布，批次与单项服务误差仍需独立证据。 [投递驱动](../../../src/collection/markov_collection/adapters/frameworks/sglang/submissions.py#L76)

## 2026-11-19 — S6 收尾：封存四组独立对照

**验证** 四组共完成 149,422 次调用、5,964,982 项工作和 8,498,488 个事件的全节点独立对照。逐请求回放均取得预期 token 数和结束标记，工作证据与消费记录全部核对。两份并发 4 对照各耗 1,916.10 和 1,805.91 秒。 [矩阵结果](../../../data/20261118T172915.294780940Z_real-workload-performance/targets/s6-heldout-matrix/heldout-results.json)

**收尾检查** 冻结的程序、二进制、模板、成本、评价文件及输入均经校验。四组各有一次详细采集和一次轻量原生测量，保留首次运行及续跑账目。约 43 GiB 原生、回放和评价证据用于复核与归因。

**结论与限制** 四组结构均通过。自主预测仍有三个长分配成本缺口，只有突发并发 4 取得一次评分，分别为 5.19% 和 8.22%。常态并发 8 的窗口内特化加载及三个组合的提交反序均记入汇总。S7 的修复和重新评价作为开发证据。
