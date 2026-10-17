# 2026-10-13：五批修改后的代码结构审查

> 历史审查/观测记录：结论限定于文中日期与当时实现。2026-10-16 清空数据后，旧 `data/redesign` 地址不再可复核；本文的“当前”“待完成”不作为现在的任务或授权状态。实时状态见[主计划](../2026-10-08_redesign_execution_plan.md)。

审查配置输入迁移、初始化适用域、局部操作来源、双侧合批参数化、采集产物保留这五批修改，并沿现有输入、生产、控制、执行和采集生命周期复查。当前任务和退出条件只维护在[执行计划](../2026-10-08_redesign_execution_plan.md)。本记录不证明所有原生规则逐行正确。

| 方面 | 代码核对与结论 |
| --- | --- |
| 层次与目录 | CMake仍为workflow→adapters→core。ProjectReader通过CollectionAdapter解释框架配置；PredictionWorkflow通过TemplateAdapter选择来源及取得公共控制参数。公共templates/controller/execution没有具体框架分支；Python公共instrumentation/artifacts没有反向导入适配实现。结合方法及调用方阅读核对，不以扫描零命中代替审查。 |
| 生命周期 | PredictionWorkflow持有全部bank及配置，来源选择只借用定义；局部操作指针未进入整批或初始化供体集合。每个配置重新绑定候选，每个workload创建独立控制器和runtime，没有跨组合复用可变KV/workspace状态。 |
| 规则归属 | request_pool_rebindable由生产端根据真实参数关系声明；核心使用公共变换入口。matches_transition复用MergeLeft/Right/Total，保留全保留、非空分支及目标状态关系，没有按样例名称或人数写特例。 |
| 重复逻辑 | plan_batch_operations会从整批来源与局部来源重复加入同一定义。已在公共候选列表按定义身份去重；普通来源先保留，较窄的局部资格不覆盖它。没有新增状态、注册器或第二套资格判定。 |
| 状态与错误边界 | SimulationRuntime按整批暂存workspace变化，装配和图接纳后提交；控制器持业务状态，执行核持唯一事件时钟。NodeDurations按来源隔离输入并在完整校验后发布，未知耗时不填零。未为可能错误增加新的防御分支。 |
| Python职责 | SGLangCapture处理框架启动、统一预热与真实组间完成；AscendProfileWindow处理运行环境配置和窗口。数据简化配置落在后者，仅在项目窗口覆盖vendor参数，未写入控制器或添加自有文件删除器。Python处理入口仍只为当前已实现的SGLang模板链启动编译，不能据此宣称KT预测已接通。 |
| 风格与规模 | 修改的方法按选择、绑定和提交分段，注释说明生命周期及限定条件。没有新增兼容分支、版本号、test工程或.clang-format修改；没有为本次审查继续拆类、增加接口或遍历并整理无关文件。 |

容器内C++23构建通过；两个公开输入分别覆盖原配置和请求池缩小，完整报告除宿主加载时间外均与修改前相同。Python全包Ruff、修改文件语法、C++格式及diff检查通过。证据为 `data/redesign/s3-profile-retention/review-verification.json`。

采集保留选项另经已安装vendor实现及真实文件引用核对；尚未执行启用选项后的新原生采集。没有删除历史数据，累计超额仍保留。功能进度、建库缺口和采集额度继续以执行计划与执行日志为准。本次周期审查结束，代码批次计数归零。

## 第二周期：代表配方到预算轴

本周期五批为四组配方、Ascend flow来源修正、模板内部线程路径、变换能力说明及代表选择。沿公开入口和状态提交链复查，结论限于职责、所有权与下述机制，不替代S3/S4最终验收。

| 范围 | 实际阅读与结论 |
| --- | --- |
| 入口与配置 | `ProjectReader::read`通过CollectionAdapter解析配置，`Planner::collection_plan`汇总TemplateAdapter分组；代表选择留在SGLangTemplateAdapter。容量优先来自可缩不可扩关系，同容量预算优先来自配方需求，没有加入核心控制分支。 |
| 来源与组合所有权 | `PredictionWorkflow::run`加载完sources后创建借用视图，每个配置绑定供体，每个workload独立创建controller/runtime。坏耗时输入不使已加载结构失效；没有以成本缺口触发结构采集。CMake依赖仍向内，公共消费层未引入具体框架类型。 |
| 生产与路径 | `BlockExtractor`仅沿已记录ThreadOrder跨无owner边界，遇到其他owner即停止；`read_structure`在区域切分前导入普通依赖。没有给排除CPU工作添加服务节点、成本或workspace专属允许分支。 |
| 环境适配 | `ObservationNormalizer._flow_enqueue`以connection/queue身份关联，唯一性不足时不猜测；原flow保留供取证。运行环境解释没有移入SGLang控制器。移除方法间额外空行，保留重要关联注释；同线程Dequeue窗口假设仍属于当前Ascend观测合同，不宣称任意环境支持。 |
| 控制与状态 | `LightweightController::admit_prefill`接纳后触发InputBudget，不设置持续关闭标志；`AdmissionWork::parameterized_cpu_path`消费相同语义。`SimulationRuntime`暂存workspace，装配及图接纳成功后统一提交，执行核仍持唯一时钟。没有增加另一套状态账本。 |
| Python编排交接 | `PredictionWorkflow._continue`仍未接通无来源时建库，是已确认功能缺口。`CollectionWorkflow`已有预算、attempt、采集和后处理恢复；后续须复用这些生命周期。`resume`目前允许显式再尝试，自动预测不得无条件调用它反复采集。此处记录接入限制，不提前新增执行平台。 |
| 文档与风格 | 模板机制参考残留“输入预算不在参数域、人数必须不变”过期正文，已按现有规则改写并合并历史流水；当前状态只在计划维护。没有新增版本、兼容、test工程或格式配置改动。 |

验证：Docker C++23构建及修改文件格式通过；公开plan、prepare-representative和双配置predict通过。预算256/16384使用同库分别展开6/5批，是开发结构证据，不是原生性能评价。宿主全包Ruff通过；modeling容器没有Ruff，未安装新依赖。`git diff --check`通过。证据为`data/redesign/s3-prefill-budget/verification.json`。本周期结束，代码批次计数归零。

## 进入S5前的阶段审查

此次按阶段切换要求提前审查，覆盖最近三批Python改动，并回读ServingContract、BlockWrapper、ExtractionWorkflow、NodeDurations与ExecutionGraph的调用边界。

- 自动采集预算与attempt仍由CollectionWorkflow持有；PredictionWorkflow只选择代表组、交接资产并发布组合结果。representatives用单一映射记录已选组及原因，active_representative保留恢复位置，没有追加重复尝试账本。
- 组配方失败已隔离并有三组公开证据；已终止原生失败分支会继续选组，但处理器抛出的异常仍由外层恢复，不能据此宣布全部失败隔离已完成。此为S6明确剩余交付，不新增通用重试器。
- SGLangCapture新增的七段计时使用相邻单调时钟边界，仅成功时写入既有环境产物；不改变预热、profiler或请求顺序，失败继续由外层监控。validation已取得实测，历史base不回填。
- C++生产/消费依赖和每组合状态所有权未变化。NodeDurations只保存显式条件耗时，ExecutionGraph仅对Service加时延，Wait/Annotation保留依赖；没有把原生墙钟或目标评分引入成本。S5须增加目标描述查询，不能把旧逐span输入更名为性能模型。
- 修改方法保持职责分段和关键生命周期注释，无新测试、兼容层或格式配置改动；文档把历史证据与当前计划分开。已运行的Ruff、容器语法和公开生命周期证据继续有效，本轮无代码修改，不重复构建。

周期在阶段切换处归零。结构验收与剩余S5/S6事项以实施计划及结构阶段验收为准；本审查不宣称性能准确性或多组原生失败验证完成。

## S5工作交接与S6失败生命周期周期

本周期五批为ServiceWork查询、CPU阶段编译交接、目标准入上下文、后处理失败隔离、启动失败记录。按入口→生产/资产→控制/执行→采集/恢复核对实际调用链，重点如下：

- C++成本查询使用目标描述，SourceCpuPart解释集中于SourceGraph；模板编码/解码与普通、插入节点同时交接。ServiceWork不持有框架类型，尚未将来源耗时或旧阶段总量变成实测模型。
- SimulationRuntime先提交Work到稳定map，再构造只读InvokeStep索引。索引仅供一次schedule借用，不复制控制状态；发布参数仍借用目标实例。等待/注释不查询服务成本，阶段上下文不代表每个CPU段重复执行整个阶段。
- Python依然由CollectionWorkflow拥有attempt和预算，PredictionWorkflow只处理组选择/资产导入。确定的后处理失败由进程持锁记录并通过专用异常交接；无终态记录的宿主错误不被吞成组失败。已有成功结果优先，已失败处理不自动重试。
- 公开输入发现启动失败漏洞：Docker可留下Created且Error非空，旧代码错误地进入未知状态且丢失启动耗时。已保存launch_result并依据真实启动退出码和容器状态识别失败；恢复只结算记录的区间。没有用恢复时间、日志mtime或零值补造旧attempt耗时。旧诊断任务保留不可恢复计量事实。
- CMake仍为workflow→adapters→core；核心controller/execution/performance与公共instrumentation未新增框架业务分支。异常类有实际隔离消费者，_process分离锁/发布生命周期与归一化工作，无重试器、兼容格式、test工程或.clang-format改动。修改方法按逻辑分段，注释解释借用和失败证据。

完整公开回归证明一个实际启动失败组不污染现有资产组，后者complete且指标/摘要保持；重复resume不增attempt、round或预算。它没有证明后处理失败后继续新采集、多成功组或实测成本。Python全包Ruff和Docker语法检查通过；本周期C++改动已有Docker构建/公开展开证据，Python末批不重复构建C++。证据`data/redesign/s6-native-failure-isolation/verification.json`。周期计数归零，功能剩余项仍以执行计划为准。

## 局部重算与性能单元交接周期

五批为无新资产结果复用、按组局部重算、完整准入单元绑定、准入采样与类型化查询、通信物理任务导出。审阅公开CLI/ProjectReader→适配与提取→控制/执行/成本查询，并检查Python预测恢复、采样和归一化职责。

- C++核心成本查询借用目标InvokeStep；AdmissionServiceWork只生成查询值，初始化标志由成员位置推导，没有新增控制状态或模板格式。等待/逻辑注释不收费，物理设备命令按既有Service角色计费，NodeDurations仍是条件输入。模板源Span只追溯，不用于拟合键。
- Python预测保存一份已发布结果，局部轮次保存实际子集，再替换受影响组合；失败未导入资产时不重算。没有第二份可变结果缓存。当前成功组证据来自实际单组导入，不能扩写为多成功组已验证。
- CPU观测绑定复用native_regions，采样与语义AST计数分开。审查澄清：阶段只扣观测器内部测得的wrapper时间，不包括外层HookRegistry进出；已修正文档，不能据此宣称所有观测开销已去除。未为此增加更多微小探针。经验中位数及独立误差留在数据证据，未硬编码或反向调参。
- 通信明细解释在Ascend适配器，物理任务关联与C++结构提取使用相同原生身份合同；分别服务采样数据与结构事实消费，没有将原生字段放进调度器。归属缺失不能统称setup后静默排除，已修正为只有已知setup才不报告缺口。公开旧采集重提取保持400条任务及原rank时长，未出现新的关联缺口。
- 已确认的后续集成缺口：`workflow/prediction.py::_simulate`审查时仅挂载 performance manifest，没有挂载其引用样本；C++ 预测尚未加载性能资产。成本接入必须同轮完成生产/引用挂载/消费，不能只增加模型类或报告字段。现阶段没有消费者，因此不预建通用递归资产加载平台。
- 开发文档仍残留旧模块联合耗时可消费、组重算未实现和workspace分支未接通的过期表述，已改为当前入口行为并指向唯一计划状态；通信产物说明归入采样章节，避免散落末尾。
- 风格按逻辑分组，新方法有实际归一化职责，无新test、版本兼容、全局注册器或.clang-format改动。历史导入显式引用真实文件，缺失记录不填造。此次不改变控制、模板规则或S3/S4资格。

本周期相关C++已通过Docker构建和真实查询；末批Python通过全包Ruff、Docker公开归一化与原始trace核对，不重复构建无变化的C++。本审查不证明成本精度或最终闭环。计数归零，当前任务仍只在执行计划维护。

## 成本候选与worker观测周期

本周期为经验候选生产、固定候选评价、物理节点分类修复、CPU ABI下移和worker观测。按实际职责、计费边界及所有权复核：

- C++拟合/评价只消费公共工作样本；文件和部署来源由workflow解释。拟合与留出不互相调用，候选不自动成为预测成本。物理SourceSpanKind由模板传到同一执行核，没有把设备事件误当逻辑注释或CPU默认值。
- 框架适配器只确定业务观测范围；ctypes ABI、线程计数和原生队列实现归入Ascend runtime。queue补丁作用于镜像内源码副本，不修改官方submodule；控制器、模板格式和成本匹配未引入队列类型分支。
- worker的pending集合仅传递实际入队身份，不复制调度状态；记录对象由RAII拥有，线程局部当前记录只借用活动对象，结束恢复父记录。缓冲锁不覆盖原生任务调用；文件输出在显式刷新时进行。范围进入/退出置于直接CPU计时之外，准入单元不额外包一层队列范围。
- 等待/探针累计值只记录，不将所有差值宣称合格服务；现有拦截点不冒充完整计算提交覆盖。入队参数类型可发生原生转换，关联只用原生ID。没有逐span模型、按样本拒绝表、兼容格式或新增test工程。
- 审查当时，新队列补丁尚未部署进正式采集镜像，原生关联与提交覆盖、目标工作匹配及成本消费未交付。后续进展以执行计划为准；本条保留当时的验证边界，不作为当前待办。

本批原生库及原生队列翻译单元均在容器编译通过；Python绑定、AST、Ruff与diff检查通过。没有更改.clang-format或采集新数据。周期计数归零。

## worker 测量到目标工作的交接周期

本周期覆盖原生提交入口、提交端口精化、worker 归一化、目标 CPU 工作描述及候选拟合。沿 CLI → workflow → 模板生产 → 实例化/执行 → 成本查询审阅，并核对 Python 采样、原生观察器与 CMake 依赖。

- `AscendSourceExtractor::bind_worker_cpu` 只给已有完整三段服务附加描述，不添加等待、删边或拆分成本；profiler 队列名称和运行时提交解释留在适配端。`CpuWorkerUnits` 以队列身份传递业务范围，线程时钟只在同一线程相减；采样配对/范围不完整明确报告，没有最近时间匹配或补零。
- `ExecutionGraph::bind_cpu_submissions` 从实际展开后的 NativeFlow 取得目标工作；重复供体通过目标返回端口区分。`CpuSubmissionWork` 借用模板库的来源描述及运行实例的目标描述；`SimulationRuntime` 持有实例，模板库覆盖运行生命周期，没有复制第二份控制状态。核心 controller/execution/performance 未引入框架名分支，CMake 保持 workflow → adapters → core。
- `WorkerSampling` 的 pending 只负责跨线程观测关联，RAII 管理活动样本，输出缓冲在被测区间结束后更新。内部提交点仍有未扣除的观察开销，这属于模型验证限制；不据此增加逐 API 补偿常数。
- `CostFittingWorkflow` 原将准入读取嵌在公共来源装配中，已改为并列的 `read_admission`/`read_workers`；拟合器共用 `CostDistribution`/`CostErrors`，未建立无消费者策略层。队列事件名称映射只在 torch_npu 适配器，拟合/评价不解释原生任务类型。候选评价不重新拟合，来源交叠检查包含原始 worker 文件。
- 审查时，正式成本加载与引用文件挂载未实现，属于当时的 S5/S6 交付项；候选命令未宣称可用于预测。历史审查的“尚未部署”已标明当时状态，开发文档补充实际 worker 命令，当前任务仅在执行计划维护。

本周期已有结构对照和目标 shape 查询证据见执行记录；最后整理通过 Docker 构建，既有准入拟合/评价公开运行结果完全保持。Python 全包 Ruff、diff 检查通过。不修改 `.clang-format`，没有新增 test 工程、兼容格式或按样本硬编码。静态职责审查不证明 worker 成本精度。周期计数归零。

## CPU 与计算成本消费周期

五批为提交点成对探针、CPU候选消费、kernel候选生产、成本配置关系归属和kernel候选消费。沿公开输入、生产、控制、执行及Python采样/恢复检查职责和所有权，保持现有模板规则及唯一事件时钟。

- `PredictionWorkflow`拥有不可变候选，`NodeCosts`只借用覆盖整个组合运行的视图；每个组合独立创建controller/runtime。`CostModel`已有CPU与kernel两种实际实现，没有为后续类型预建注册器。未知成本沿既有缺口返回，显式给定值单独计数，等待与逻辑注释不重复收费。
- 成本来源配置只在workflow读入时解析，关系由`SGLangTemplateAdapter::supports_cost_configuration`复用既有控制路径与配置关系。删除只用于JSON相等检查的旧接口；核心性能/调度层没有具体框架参数或名称分支。路径相容只允许查询，不授予成本迁移资格。
- `KernelCostFeatures`与匹配归入`kernel_cost_model`，拟合器直接复用同一模型评价，删除重复的候选读取与匹配。原生字段解释仍在Ascend归一化端；模型按真实特征取值，没有来源span、请求身份或逐kernel名称例外。`SourceKernel::has_semantic_work`直接读取类型化字段，撤下内部JSON转换方案；同时删除无消费者的CPU模型`unit()`访问器。
- Python继续只读挂载显式拟合文件；预测不读取原始样本、不重新拟合，资产修订与分组重算沿既有生命周期。成对探针来自实际测量，没有固定补偿值，也没有沿局部误差追加逐API探针。CMake保持workflow→adapters→core。
- 剩余风险明确保留：唯一观测启动变体不是目标启动证明，缺少额外语义标签也不证明数据无关；worker忽略shape及运行环境迁移尚未资格化。这些是成本模型限制，不能以静态审查或有条件运行完成它们。

Docker构建、Python全包Ruff、Shell语法及diff检查通过，双配置公开预测实际消费计算候选；CPU对照的完整指标、执行摘要及结构报告与改动前相同。固定worker/kernel评价也与原结果相同，证据为`data/redesign/s5-kernel-consumption/{verification,evaluation-regression-verification}.json`。本次接口整理没有新增模板、控制规则、兼容实现或test工程，也未改`.clang-format`。周期计数归零，功能进度只在执行计划维护。
