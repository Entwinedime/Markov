# 2026-10-14：已完成部分的完整审查与整改

> 历史审查/观测记录：结论限定于文中日期与当时实现。2026-10-16 清空数据后，旧 `data/redesign` 地址不再可复核；本文的“当前”“待完成”不作为现在的任务或授权状态。实时状态见[主计划](../2026-10-08_redesign_execution_plan.md)。

依据：[工程约束](../../redesign/engineering_constraints.md)、[整体框架](../../redesign/01_system_framework.md)、[采集合同](../../redesign/04_collection_templates.md)、[性能合同](../../redesign/06_performance_execution.md)和[实施计划](../2026-10-08_redesign_execution_plan.md)。本页是本日完整审查的统一记录，覆盖此前整改后的当前实现；历史发现合并在§6，不继续叠加整份审查和滚动进度。

**结论：主要抽象边界成立，存在七项需要整改的新问题，其中独立通信采样的环境记录缺字段会阻断公开拟合。确认问题已逐项处理；S3/S4 已验收的规则域保持，S5/S6 的未完成能力没有因审查被提升资格。** 功能 goal 保持暂停，本轮是用户另行授权的审查工作，未执行新采集。

## 1. 范围、方法与证据口径

起点是实际未提交工作区，而非 Git HEAD。包含 **193 个 C++/Python 文件、32423 行**，以及正式 CMake、Python 包配置、9份Shell、Docker、7份配置和 redesign 文档。逐文件目录、依赖、消费者和静态工具检查覆盖全部活动源码；人工沿下列职责链核对接口、生产者/消费者、关键分支与所有权。没有以格式通过、低引用或文件长度代替逻辑审查，也不声称逐行证明所有上游及故障场景。

整改后为 **195 个文件、32422 行，净减1行**。新增文件均承接实际共享职责：CPU阶段归属与原生JSON字符串编码；删除两处归属实现、两处编码实现和重复输入校验。新增完整性及环境记录补正后总量基本持平，未通过压缩排版或删除必要资格规则降低行数。精确逐文件差异见证据。

独立证据目录：`data/redesign/2026-10-14-current-project-review`。执行前声明原生 attempt **0**、raw **0**、派生上限 **64 MiB**；复用真实原始记录、已编译模板、成本及请求，普通预测关闭大图。第三方、reference、主分支和`.clang-format`均保持；没有新增test工程、mock、兼容格式或设备任务。

| 证据 | 实际用途 |
| --- | --- |
| `baseline.json`、`before/`、`preexisting.diff` | 完整起点快照；区分存量改动与本批整改 |
| `file-review-index.json`、`dependency-review.json` | 活动文件归属、正式target、传递include、Python层向和消费者核对 |
| `review-changes.patch`、`code-size.json` | 本批实际变化与代码规模，不重复计入历史减量 |
| `cpu-replay.json`、`device-completion.json` | 真实日志的归一化保持、逐rank完整性及单rank记录的缺口 |
| `prediction-verification.json` | 固定公开预测的完整组合业务字段、一次计费和零采集 |
| `host-evaluation.json`、`same-source.log` | 固定候选的独立来源评价及同源拒绝 |
| `communication-before.log`、`communication-import.json` | 通信输入阻断复现及明确保留未知值的历史元数据导入 |
| `verification.json`、`accounting.json` | 检查结果、未验证范围、派生额度与累计账本结算 |

## 2. 分层与完整数据流审查

```text
公开入口 → 集合输入 → 框架/环境适配 → 原生事实 → 统一模板资产
目标配置/request → 轻量控制 → 有限变换 → 目标依赖/资源 → 节点或区域成本
                       ↑                               ↓
                       └────────── 完成事件与唯一时钟 ──┘
独立性能观测 → 共同工作描述/实际覆盖 → 拟合与固定评价 → 只读成本候选
```

### 2.1 输入、入口与装配

`ProjectReader`拥有Project、配置集合、请求集合和显式资产路径；Request/Environment/ServingLimits留在contracts，SG/KT配置在adapters。CLI是具体适配实现的组合入口，Planner和PredictionWorkflow通过CollectionAdapter/TemplateAdapter解释差异。Python只编排容器、文件及恢复，没有复制C++调度算法。

核对相对声明文件解析、身份唯一、整数范围、只读外部挂载、采集准备与预测资格分离。源码中外部原始字典保留在Python采集边界；不能据此要求全部移入C++。采样配方和原生策略数值已统一使用公共输入校验，修复F35；没有再建一套拒绝表。

### 2.2 流程、wrapper和插桩

ServingContract定义五个block的参数、状态效果及完成端口；框架绑定决定位置。BoundBlock/BlockWrapper/Recorder共用身份和异常清理，NativeRegionWrapper和SGLang内联区域读取真实上下文。语义与性能生命周期分开，发布成员的AST选取工具可共享。

核对提前返回、continue、原生decorator、组合hook及父scope恢复；AST匹配检查确实约束当前上游位置，属于适配边界，不是针对失败样本的黑名单。观察仅读标量/布局，不复制激活、不增加serving同步。KT保留采集适配，没有共同预测或统一预热资格。

### 2.3 框架、模型与runtime

frameworks拥有原生组批、请求参数与模型语义；models拥有Qwen3模块及实际kernel解释；runtimes拥有SDK、profiler、时钟、队列、workspace和HCCL解释。公共instrumentation/artifacts不导入adapters，runtime不反向导入frameworks。

原生符号、ARM宏、Cat上限、HCCL分支和allocator对齐有真实实现依据；核心消费编译后的有限关系，不按框架名或kernel名重新猜规则。核对了疑似死分支：FloorDiv包装实际会导出Cast/Slice辅助kernel，不能因为外层名称筛选而删除；已有真实样本证实这些分支有消费者。KT启动wrapper依赖官方spawn语义，没有据表面赋值判断为递归缺陷。

### 2.4 采集、进程与恢复

NativeAttempt拥有专用PID命名空间、设备锁、deadline、子进程清理和终态；请求/KV状态由框架管理。CollectionWorkflow分开接收原生终态和处理终态，CaptureProcessing持有处理锁。预测缺兼容来源的配置组最多尝试一次；规则/成本缺口不触发补采。

核对启动失败、宿主中断、实际容器终态、处理失败、预算累计与显式修订。完成任务的status/resume复用结果，本批公开恢复没有增加轮次或attempt。多成功采集组、任意崩溃时刻及设备清理未在本轮重新实机验证，仍属原计划的剩余验收。

### 2.5 发布、保留与归一化

公共JSON发布负责目录和原子替换；原生日志缓冲在测量区间之外。vendor导出核对完成后按显式引用保留，压缩不改变事件或独立性。normalizer保留rank/process/thread/task等身份，时间包络只核对已有同步关联，不以相近时间替代队列或设备因果。

CPU队列与操作的阶段归属原来有重复实现，F33将其收敛为一个共同所有者；窗口跨越、相同最内层或交叉scope仍保留缺口。F32补齐逐rank完成覆盖。函数区间仍用同线程绝对锚点，不跨线程相减、不分摊父包络时长。F38补齐独立通信产物与公共拟合输入的环境合同。

### 2.6 子图提取与模板生产

BlockExtractor按真实归属提取子图和端口，注解区域只引用已有节点。SGLangTemplateBindings解释成员、模型、KV、发布和参数关系，Ascend生产端解释硬件/队列工作。TemplateSource拥有观测和规则，TemplateGraph共享局部结构；TemplateAsset先恢复拥有数据，再恢复借用引用。

核对源manifest限定的span、source interval身份、供体所有者、map/set及vector移动后的引用、初始化证据和共享区域结构。没有发现需要重写codec或增加摘要握手的现有反证；也没有把正常资产回归说成恶意/任意损坏资产的穷举验证。

### 2.7 选择、有限变换与装配

TemplateBank按完整phase/rank/cohort选择来源，再联合核对成员、长度域、实际分支、切口和资源。TemplateTopology暂存整组变换后提交；TemplateAssembly区分原始来源和新目标身份，重新绑定端口、等待及跨批前沿。新输入增加实例，不默认增加模板定义。

核对filter/merge、请求循环、shape、KV、供体、通信组与workspace替换；所有拥有节点必须保留/完整重定位，替换不能以共享端点冒充完整。资源顺序来自实际依赖，原生时间与规划顺序不能决定跨资源串行。支持范围按原合同保持，未新增模板族或变换规则。

### 2.8 控制、请求与KV

LightweightController唯一维护目标请求进度、等待、准入、逻辑槽位及KV页。Published提交token与释放；Submitted和DeviceDone通过不同依赖决定发布入口与可见结果。输入预算停止和持续内存/容量阻塞分别表达，同刻事件整体交付，预约中的批次不吸收后来到达。

核对一个输出token、pending prefill、运行成员过滤、准入后状态和页需求先核对后提交。整理四个动作的构造，使依赖可读，未改变动作顺序。当前cold、非overlap、非chunk、无prefix/graph范围保持，不复制原生缓存/撤回状态机。

### 2.9 执行、成本区域与workspace

ExecutionGraph拥有目标边界、依赖和成本记录；等待/注释只提供因果。完整CPU区域保留原节点和边，由一个入口→出口费用覆盖服务，首服务是唯一查询所有者；实际公开端口或必要内点依赖会阻止合并。冗余内点边必须由已有更强可达路径证明，不能按关系名忽略。

逐一核对CpuHostWork及PublicationWork的借用寿命：查询视图同步使用，缓存只保存数值；实例vector移动保持其拥有数据地址。所有入队必须保留并解析实际目标根。WorkspaceState独占容量，整批暂存成功才提交，重调度不重复分配。累计图与资格推导仍有重复遍历，这是规模风险，不据此新增第二执行器。

### 2.10 性能观测、拟合与消费

ServiceWork提供目标工作，来源身份仅用于追溯及显式givens。CostFittingWorkflow只读独立观测，evaluate不调用fit；sample_sources继续追溯原始文件。NodeCosts先查适用候选，再查全部成员的给定值，缺值不返回部分和或零。

核对准入、worker、enqueue、纯操作、host区间、发布、kernel、通信及搬运的计费边界。F37修复总体测量缺口被单元缺口覆盖的问题。模型仍是有域的经验中位数，省略特征、等待、探针尾部及部署迁移没有因接口接通而解决；本轮不改变拟合算法或校正参数。

### 2.11 组合、报告与隔离

共享只读资产，各组合独立创建控制、KV、workspace、图和时钟。Python轮次只装配明确输入，只有结果目录可写；自动导入按配置组重算，显式替换资产完整重算。源资产损坏要求修复，不能用新的结构采集掩盖错误。

核对组合身份合并、源/目标评价角色分离、活跃轮次不接收修订、成本成功缓存和完成结果复用。四个完整目标组合的全部业务字段保持，缺givens的两个组合继续needs_data。坏公共成本文件仍是输入错误；不能把组合隔离理解为忽略用户明确声明的坏输入。

### 2.12 风格、构建与文档

正式依赖固定为workflow → adapters → core；传递include也没有把具体框架配置带入core。67个C++实现均注册在正式target，C++23在modeling Docker构建。原生观察库使用实际Torch ABI和CANN环境；新公共编码头复用既有include路径。

人工检查逻辑空行、关键生命周期/计费注释、浅层类职责和无用包装。低引用helper、虚接口、plugin及可选诊断有实际调用，不为减量删除。F36修正暂停状态和多份设计中残留的S3/S4旧结论，历史数据和验证规模留在日志，当前状态只在计划维护。

## 3. 本轮发现与依次整改

P1为已复现的功能阻断；P2为观测、合同或维护问题。下表按影响组织，不把静态风险冒充实测故障。

| 编号 | 位置、问题与影响 | 整改与关闭依据 |
| --- | --- | --- |
| F38 / P1 | `runtimes/ascend/collective_sampling.py::NativeCollectiveSampling.run`的环境产物缺framework_version/runtime；公共CostFittingWorkflow必读，真实独立通信资产fit-costs退出2 | 生产端记录实际包版本及RuntimeEnvironment。历史资产用明确导入视图保留两个未知值为null，不回填当前环境或加入兼容reader；24组真实物理任务候选拟合通过。新采集的现场发布未重跑 |
| F32 / P2 | `SamplingDomain`及`sglang/sampling.py::_cpu_coverage`用跨rank完成并集，可能掩盖丢失的worker日志；device路径没有共用完成报告 | 完成记录由SamplingDomain逐rank持有，CPU/device共用completion_coverage和缺口。训练/留出各18个、device 21个声明请求完整；仅消费既有rank 0日志时全部报告缺rank 1，不生成假事件 |
| F37 / P2 | `cost_fitting_workflow.cpp::read`以cpu_unit/worker/operator_gaps覆盖manifest.gaps，候选会丢掉整体预热/记录缺口 | 合并总体缺口和所选单元限制，run/evaluate继续共用读取。103组host统计和固定评价保持；候选实际保留两类缺口，不提升资格 |
| F35 / P2 | `SourceAdmissionPolicy::read`及输出间隔读取先get整数再校验，JSON小数可能已被截断；采样域复制公共字段/数值校验 | 使用input::integer/positive/fields，保留原有合法范围与原生分支判断；事实导入和模板编译读取同一数值规则。正式构建、真实采样准备及目标业务回归通过；小数拒绝为公共校验语义的静态核对 |
| F33 / P2 | `CpuWorkerUnits.publish`与`CpuOperatorUnits.publish`重复阶段读取、lane分组、最内层选取和歧义判断 | 归入`artifacts/cpu_scopes.py::CpuMeasurementScopes`，runtime只解释自身队列/操作。两份原始日志重处理后全部内容相同，仅引用路径重定位；无需额外状态管理器 |
| F34 / P2 | 两个原生观察器复制JSON字符串编码；SourceGraph.seal重复判断已保证的CPU类型；诊断复制layout编码；成本种类筛选重复分发条件；控制动作构造拥挤 | 共用native/json_string.hpp，删除重复实现和类型旁路，layout使用现有describe，成本种类由一次分发判断；动作按依赖分组。原生/核心构建、全体格式、四组合业务字段及唯一计费保持 |
| F36 / P2 | 当前计划仍写active，设计残留“仅request_identity”“S3/S4尚未完成”和固定2→1；审查正文多轮结果叠加 | 计划同步paused；设计只解释规则与资格，阶段状态链接计划；本页重写为完整责任链与七项当前整改，旧发现压缩到§6。文档链接、旧表述、diff检查通过 |

所有确认缺陷已处理，未另建长期审查平台或新功能分支。数据资格及规模风险单独保留，不通过补采或局部常数使案例过关。

## 4. 验证及其实际边界

| 检查 | 结果 |
| --- | --- |
| Docker正式构建 | 新Release目录及默认正式入口通过；原生Ascend观察库和实际Torch操作库通过。SDK已有扩展警告保持 |
| 静态与层向 | 146个C++格式、49个Python的Ruff/格式/AST、9份Shell语法、7份JSON；CMake注册和核心传递包含/Python层向无违规 |
| CPU日志完整回放 | 每份95670个队列单元；55368个纯操作、161640个host区间全部保持。已生成规范化视图，原始采集不改 |
| rank完整性 | CPU训练/留出及device阶段日志各自回放，全rank完整；只读rank 0真实日志时返回rank 1缺口。未重跑整个device vendor导出 |
| 固定host拟合/评价 | 103组估计与历史候选一致，独立来源143680条正式观测的误差保持；同训练来源仍退出2，不重拟合留出 |
| 条件预测 | 两档Prefill预算及pool 3/4四组合complete，全部组合业务字段与审查前一致；114708/44086个服务节点保持唯一计费 |
| 缺成本与恢复 | 无givens两组合继续needs_data/node_cost_missing，零采集；完成任务status/resume不增加轮次或attempt |
| 独立通信 | 原有缺字段输入退出2；明确未知的历史导入可拟合24组，未知条件和原测量缺口保留；不是新现场验证或精度验收 |
| 文档与工作区 | 当前状态、局部合同、历史记录分清；本地链接与差异检查通过，保留其他未提交工作和受保护文件 |

这些是开发回归、静态审查与真实资产重处理，不构成新的独立目标精度验证。CPU/设备硬件任务为0，原始字节新增0；最终占用和累计账本见accounting.json，历史超额及未决启动计量保留。

## 5. 限制与交接

- **实际支持域有限。** 共同控制器和模板消费者不解释框架；换模型、TP、backend或控制策略仍须相应生产适配和明确规则。KT只有采集能力。
- **成本没有全覆盖。** 四组合仍依赖显式givens，范围与缺口以实施计划为准；complete是声明条件下的运行，不是完整真实性能闭环。当前name/count/position和物理描述模型仍省略工作特征。
- **观察与运行条件未资格化。** 回调/dispatch尾部、未观测CPU/I/O等待、采样flush/缓存、通信竞争及部署迁移仍未建立；未知数据保留，不通过父包络分摊或残差补偿解决。
- **现场路径未重跑。** 新通信环境产物及SDK作用域补丁的完整新采集未在本轮验证；任意故障时刻、多成功建库组及最终独立精度仍属既有计划。
- **规模风险。** 长序列会重复遍历累计图，CPU区域资格和大型诊断仍有推导/重复内容；本轮没有长序列基准或内存峰值测量。模板外工作默认排除，不能以清空所有诊断为门槛。

本轮以活动责任链覆盖、确认问题整改、相关公开验证及文档/资源结算收口。功能goal保持暂停，不写implementation_log完成声明，不把上述约定或风险自动扩张成新的代码任务。

## 6. 历史整改的唯一保留摘要

以下不是本轮新增成果；详细来源与当时验证留在原证据目录和[执行记录](../2026-10-08_redesign_execution_log.md)。此前源码起止统计与后续功能增长不再并列充当当前规模。

| 历史编号 | 已解决的独有问题 | 证据 |
| --- | --- | --- |
| F1、F9 | 采集与处理终态分开；失败隔离、已完成处理复用和用量不重复累计 | `2026-10-14-completed-review`、`2026-10-14-review-followup` |
| F2、F7、F13 | 完整导出后保留、gzip共同读取和相同来源不得作留出 | 同上及`2026-10-14-final-review` |
| F3、F4、F6、F8 | KT到达稳定排序；公共/框架配置分层；文档范围与采样容量修正 | 同上 |
| F5、F11、F12 | 合并真实统计复用，删除timer占位，已记录成本不重复查询 | 同上 |
| F10、F15、F16、F18、F19 | scope异常恢复、合法空batch、真实enqueue probe及数值/flush合同 | `2026-10-14-final-review` |
| F17、F25、F26、F27 | 原子JSON发布及父目录遗漏、外部只读输入挂载、I/O路径上下文 | `2026-10-14-comprehensive-review` |
| F20–F24 | 完整CPU区域、实际目标切口、单次计费、删除单节点旁路和完整供体节点核对 | `s5-cpu-region-costs`、`2026-10-14-comprehensive-review` |
| F28、F29、F30 | 删除未接通原型与Shell转发；文档和语义/性能生命周期一致 | `2026-10-14-comprehensive-review` |
| F31 | 结构与性能共用原始FUNCTION作用域，删除aten名称筛选；已有事实明确重提取 | `s5-cpu-operation-segments` |

F28删除的是无消费者原型；后续操作观察已沿完整链路接通。当前库中存在的观察器不能因历史删除记录被误判为死代码。FUNCTION切口、完整CPU区域及串行队列区间的后续能力均在执行记录保留独立证据；本页不复制其逐轮采集数量和代码增量。
