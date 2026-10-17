# 2026-10-15：周期代码层次与任务生命周期审查

> 历史审查/观测记录：结论限定于文中日期与当时实现。2026-10-16 清空数据后，旧 `data/redesign` 地址不再可复核；本文的“当前”“待完成”不作为现在的任务或授权状态。实时状态见[主计划](../2026-10-08_redesign_execution_plan.md)。

依据：[工程约束](../../redesign/engineering_constraints.md)和[实施计划](../2026-10-08_redesign_execution_plan.md)。[上次整体审查](2026-10-15_code_structure_review.md)后五批改动涉及 worker 完整前段、Prepare 函数间成本、kernel 成本输入、资产修订恢复和独立性能资产交接。

**结论：关闭一项重复职责、一项错误日志缺陷及两处头文件依赖整理；没有发现需重开既定 S3/S4 验收的新结构缺陷。** 成本资格及完整 S5/S6 交付不由本次审查证明。

追加审查：后续周期复查及整改见[第6节](#6-正式性能需求与采样交接后的周期复查)、[第7节](#7-语义边界与测量职责的周期复查)和[第8节](#8-2026-10-16cpu范围收缩后的周期复查)。前五节保留前一次审查的证据口径。

## 1. 审查范围

起点为当前未提交工作区：194 个 C++/Python 文件、33129 行，另含正式构建、部署入口、配置和设计文档。静态依赖、语法/格式及构建项检查覆盖全部活动源码；人工从公开入口沿合同、插桩、模板生产、有限变换、控制、执行、成本和结果恢复核对职责与状态流，重点复核五批变更的共同边界。

人工审阅聚焦接口、关键分支、所有权和消费者，不是逐行形式证明。未改变的原生机制沿用已有证据，没有重跑历史采集或逐事件结构验收；扫描命中只作定位依据。

证据目录：`data/redesign/2026-10-15-lifecycle-code-review`。源码/构建起点在 `before/`、范围在 `source-index.json`，依赖检查在 `dependency-review.json`；派生上限 64 MiB、原生及 raw 新增额度均为 0，在执行前声明。历史采集、第三方依赖和 `.clang-format` 未修改。

## 2. 职责链审阅

| 范围 | 核对结果 |
| --- | --- |
| CLI、输入与构建 | `ProjectReader`解析集合和路径，框架参数由`CollectionAdapter`解释，CLI负责装配；CMake保持workflow→adapters→core，没有独立test target。 |
| 统一流程与wrapper | `ServingContract`提供五类block及端口/参数；Python公共wrapper消费绑定，原生方法、模块和AST位置留在框架适配器。没有为每种block新增空类。 |
| 原生执行与预算 | `NativeAttempt`持有设备租约、进程及预算，清理worker后释放；`CollectionWorkflow`区分原生终态和后处理终态。原生状态没有移入仿真器之外的新状态机。 |
| 框架与runtime | 框架读取请求、批次和模型布局；Ascend/PyTorch解释SDK、队列、时钟和物理事件。公共instrumentation/artifacts不反向导入适配器，runtime不导入框架。当前Ascend装配不授予其他环境资格。 |
| 子图与有限变换 | 模板参数、区域替换、供体重定位及资源接口仍由共同模板层消费；框架条件留在生产端。没有按样本、节点编号或目标成绩增加规则。 |
| 控制与状态 | 轻量控制器持有请求/KV进度，workspace由执行状态拥有；每组合创建自己的控制和执行对象。来源、工作模板及成本候选可共享，可变目标状态不共享。 |
| CPU区域计费 | worker前段、host/gap和完整操作复用连续成员、目标锚点及外部切口；无模型的内部单服务延后绑定，避免阻挡完整实测区间。未给内部切片分摊包络成本。 |
| 目标工作与成本 | `CpuGapWork`按实际调用重绑定step/phase/工作量；固定遍历属性由runtime声明，核心不识别kernel名称。未知循环/索引工作仍缺成本，没有未知→0或目标残差。 |
| 拟合与预测隔离 | 独立样本在拟合进程生产自含值候选，预测仅取得候选；显式候选优先，空候选不触发采集。固定评价不回写模型。 |
| 持久化与恢复 | 任务状态持有原生attempt、代表组和拟合收据，具名CPU容器持有实际执行状态；完成结果原子发布。明确修订仍在已有代表交接后生效。任意中断时刻和多新成功组不因本次审查获得运行资格。 |

静态结果：覆盖135个Markov C++文件的依赖清点中，核心直接/传递依赖未触达adapters/workflow；51个Python文件的公共/适配依赖检查通过。正式Markov C++源文件均列入构建。这些结果只证明检查范围内的依赖关系。

## 3. 发现与整改

### 已关闭：两类CPU任务重复维护生命周期

[prediction.py](../../../src/collection/markov_collection/workflow/prediction.py)和[performance.py](../../../src/collection/markov_collection/workflow/performance.py)分别维护只读容器配置、输入挂载、具名进程复用、等待、日志及退出判断；活动状态检查也各自实现。后续恢复修正容易只落到一处，属于已存在的维护债务。

新增实际复用的[ModelingJob](../../../src/collection/markov_collection/workflow/modeling_job.py)，统一上述CPU容器生命周期。调用方继续负责自己的输入依赖、处理身份和结果发布，不引入注册中心、回调管道或通用后端。性能导入通过组合持有job，避免重复保存相同执行上下文；预测和拟合的容器名及持久化合同保持。

### 已关闭：子进程错误输出未进入处理日志

`PredictionProcessing`及`CaptureProcessing`的C++子进程同时捕获stdout/stderr，但只消费stdout；异常只留下命令和退出码，具体报错丢失。改为只捕获JSON stdout，让stderr继承容器日志。

使用真实项目和真实预测worker，故意省略其配置挂载，确认C++输入错误和退出信息进入`processing_launch.log`。旧测量资产的必填字段错误也在拟合日志保留。提取调用点采用同一stdio方式并完成静态检查，本次没有为验证它重处理历史capture。

### 已关闭：两处直接声明引用

`execution_graph.cpp`和`templates.cpp`显式包含`<variant>`，使`get_if`/`holds_alternative`不依赖其他头文件的传递包含。只整理对应引用，没有格式配置或业务变化。

### 已排除的怀疑

实际创建一个启动失败的CPU容器后，`docker wait`立即返回127，未出现怀疑中的无限等待。恢复同一容器也明确失败且不重启。因此未增加额外created状态检查或重试，证据见`launcher-failure-verification.json`。该诊断没有设备、网络或原生采集。

## 4. 验证与规模

- Docker C++23构建、144个C++格式检查通过；51个Python AST/Ruff/格式、9份Shell、7份JSON及modeling容器内TOML检查通过。宿主Python 3.10不提供tomllib，检查改在现有modeling环境执行，没有加依赖或修改运行要求。
- 固定候选预测与独立样本导入分别完成2配置×2workload，全部请求指标、图规模和成本来源摘要保持；导入产生的各类候选与原固定结果一致。具名容器仍无设备/网络、根只读且只有单一输出目录可写，预测不取得样本。
- 暂停真实拟合容器后终止宿主，status仍识别原活动容器并拒绝修订；解除暂停后恢复同一container ID，重复resume不改收据、候选或轮次。没有新原生attempt。
- 旧测量缺少`tensor_traversal`仍明确失败，不加入兼容解析、不退化为重新采集；该失败任务保留可修订状态，处理容器已退出。
- 当前源码195文件33133行，净增4行：删除两套重复任务组装，新增一个共享实现及两处直接include。没有为行数压缩排版，也没有新增test工程、模板、成本特例或拟合数据。

主要证据：`review-code-changes.patch`、`code-size.json`、`static-verification.json`、`prediction-verification.json`、`interruption-verification.json`和`prediction-error-verification.json`。资源结算见`accounting.json`。

## 5. 保留的边界

`fit-costs --unit all`仍按单元重复读取同份归一化文件，属于有界后处理开销；本次未证明其构成瓶颈，不预建缓存系统。CPU任务使用`CollectionWorkflow`提供的镜像、路径和Docker状态工具，依赖只在workflow内部，未进入核心；新增实际运行环境时再收窄这一装配边界。

模板选择、区域资格和累计图调度的规模风险，成本的观测/状态/部署资格，以及多组原生建库和整次独立评价均沿用原边界，不把本次静态检查或条件回归当作完成证明。后续任务只在实施计划维护。

本次周期审查收口，实质代码批次计数归零。

## 6. 正式性能需求与采样交接后的周期复查

本次覆盖随后五批工作：成员探针归属、CPU阶段边界、性能需求目录、共享采样计划及真实性能交接。人工沿公开入口审阅全部活动目录的职责、所有权及消费者，静态检查覆盖143个Markov C++文件、9个原生观测C++文件和51个Python文件。没有逐行形式证明或重跑未改变的S3/S4采集。

| 范围 | 结论与整改 |
| --- | --- |
| 输入、CLI与构建 | 统一入口注入框架适配器；Markov正式C++源均进入构建，核心未反向依赖workflow/adapters。外部预测报告的request_id在访问prompt span前检查一次，错配报告返回未解原因。 |
| wrapper、事实与模板 | 公共instrumentation/artifacts不导入具体适配器；原生方法和AST位置、SDK/物理事件分别留在框架/runtime层。graph/contracts/production与控制/变换依赖方向保持，没有发现应整块删除的无消费者接口。 |
| 成本与需求 | CostQuery仍为覆盖及执行的共同查询。历史已观察与配方已尝试分别报告；按具体点而非整组停止，避免[A,B]变[B]后重复采样。完成域保留成员角色，不把异质成员完成数量相同当作同一域。 |
| 采样与恢复 | 原生状态仍属框架，父任务持有唯一预算与attempt。真实宿主进程丢失后继续同一容器，完成独立拟合和三轮预测；终态resume不重复采集、拟合或记账。 |
| runtime边界 | 删除三处通信runtime对SGLang configuration.tp_size的读取，world_size改取公共部署的devices数量；没有新增接口或验证层。未重跑通信采样，不因此提升其资格。 |

Docker C++23构建及全部152个自有C++格式检查通过；51个Python AST/Ruff/格式、脚本语法和diff检查通过。镜像未安装rg，容器文件枚举使用find，未为检查增加工具链依赖。公开输入负例返回48条缺口中的13个归并需求未解，没有越界访问；真实CPU的64条scope重放保持原工作点/批次特征覆盖，并生成明确完成成员事实。异质部分完成及多点部分命中仍只有实现和审查证据，本轮没有补采来证明这些分支。

仍需处理的功能缺口在实施计划维护：完整CPU候选区域缺模型时可能仅作alternative，导致能够覆盖未定价服务的完整区域不能主动请求测量。最小整改应复用现有切口资格、模型查询和区域顺序，只选择覆盖缺口的完整区域，不盲采全部父子区域。

可后置的收紧：fit-costs --unit all重复解压同一测量文件；SimulationRuntime每批重建全部历史invocation查询视图；SourceCommunicator在公共图中存储少量Ascend原生事实、实际解释仍留runtime适配器。这些分别是有界后处理、装配规模和未来第二runtime的边界债务，不据此新增缓存平台、状态管理器或提前重做结构合同。

证据在 `data/redesign/s6-demand-native-handoff` 的code-review-scope、invalid-origin-plan、completion-domain-replay、host-interruption及verification记录。原生两次观测用于正式链路验收，周期静态审查本身未增加设备任务。完整S5/S6和精度资格仍未完成。本次周期复查收口，实质代码批次重新计数。

## 7. 语义边界与测量职责的周期复查

覆盖随后五批：完整CPU测量选择、观察工作隔离、数据相关kernel合同、注释线程桥接及transition稳定边界。人工沿活动C++/Python的接口、消费者和生命周期审阅，静态覆盖143个Markov C++文件、原生观察代码及50个Python文件；未逐行证明或重跑未改变的结构采集。证据统一在`data/redesign/s5-cpu-semantic-regions`。

| 审查项 | 整改与边界 |
| --- | --- |
| 核心与适配层 | 核心直接/传递依赖未反向触达adapters/workflow；原生AST、SDK和张量解释仍分别归框架/runtime。控制、KV及workspace没有第二份可变权威状态。 |
| 并列查询入口 | 删除无消费者的`NodeCosts::model_cost(ServiceWork)`、`estimate(ServiceWork, …)`和`CostQuery::describe()`；实际路径统一显式查询，对象报告保留`CostObject::describe()`。 |
| 重复测量索引 | 将CPU语义区域及device操作数字段摘要收回`SamplingDomain`，原遍历直接提交事实，删除两份聚合及一次CPU记录重读。未新建注册器、历史平台或匹配规则。 |
| 语义与成本边界 | 结构与独立性能观测共享原生transition切片；主体、外围gap复用公共完整区域和目标切口。没有稳定端口时不退回旧整阶段费用；idle保持真实phase与零工作量，不能序列化成decode。 |
| 原始数据生命周期 | 压缩发布与破坏性清理分开，成功规范化后才消费本次pending清理状态；旧已结算捕获和外部引用不写不删。旧原始数据正式重处理保持111084条样本和全部覆盖；新待清理路径尚无新device/communication运行证据。 |

Docker构建、全部自有C++格式、Python AST/Ruff/格式和脚本检查通过。完整目标图对照保持199607节点、254776边界身份、458117条边及20端口；新成本消费后原实测费用分区和值保持，全部服务恰好计费一次。公开需求驱动的一次CPU采样及终态恢复用于功能验收，审查自身没有另加原生尝试。同源transition评价退出2；未据此宣称精度或测量资格。

保留的已承诺接口与风险：`RuntimeView`仍是稳定控制合同，当前轻量控制器未消费其中视图；不以传空数据伪装优化。position单token移除仍仅诊断，静态presence条件未证明B2→B1正式变换或拒绝，不能据诊断宣称支持。混合transition内部gap、空调度及实际同步时钟仍有限制，任务状态在实施计划维护。没有为了减行数删去真实依赖、支持缺口或已有合同。

本次周期复查收口，实质代码批次重新计数。后续持续关注逻辑与代码复杂性，发现冗杂及时纠正，不等下一个周期；完整S5/S6继续保持未完成。

## 8. 2026-10-16：CPU范围收缩后的周期复查

这是上次审查后第五个实质代码批次。复核覆盖当前139份Markov C++文件、47份Python及5份原生C++文件，共32732行；人工沿公开入口、采样/归一化、模板生产/变换、控制/执行、成本及恢复检查职责和消费者，静态扫描覆盖构建与本地依赖。不是逐行证明；未改变的结构机制沿用既有证据，没有增加测试工程、采集或审查平台。

| 核对项 | 结果及限制 |
| --- | --- |
| 成本与结构边界 | 删除CPU模型、成本专用工作类型/区域/codec、需求与采样路径；保留transition、admission和publication成员区域，供真实子图变换和资格判断。源结构角色与目标计时角色分开，同步host传输继续收费。 |
| 执行核 | 删除完整CPU费用分组、二次切口索引和分组given路径；一个物理服务节点对应一次查询、需求与费用。CPU节点不删除，未知物理成本不填零。 |
| 采样归属 | scope→原生correlation→worker关联完整driver叶子，双端点只计单调墙钟。删CPU时钟/probe/逐操作observer和无消费者构建；保留结构trace与journal。跨语言审查发现的kind及worker环境字段不一致已按最小合同修正。 |
| 状态与依赖 | 请求/KV仍由每组合控制器唯一维护，workspace在整批结构成功后提交；没有新增状态机、预算账本或CPU可选后端。CMake及Python依赖保持向内，无已删模块悬空引用。 |
| 任务生命周期 | 原生attempt、ModelingJob及失败交接保持；失败候选不导入、不重置预算。真实终态complete与needs_data任务的status/resume保持结果、轮次和用量；自动终态拟合失败等未实测项继续留在计划。 |
| 风格与规模 | 删除失效职责而非压行、拆空类或引入兼容分支；所改流程保留逻辑空行及生命周期注释。`RuntimeView`沿用已确认稳定合同，不因当前轻量实现未消费而重开接口设计。 |

干净Docker C++23 Release构建及最终原生观察库构建通过，47份Python的AST/Ruff/格式与安装脚本检查通过。真实详细输入保持199607节点、254776边界身份、458117边、20端口、请求槽位/KV及控制转换，既有物理成本保持；75360个纯CPU服务转为目标注解，仍保留节点和依赖。另两个配置的复合请求条件运行完成；无given保留三类同步H2D缺口，合成一个transfer请求域，未伪造闭环。

部署限制：新两参数队列ABI须与torch_npu头文件成套重编译，未在旧镜像启用新采样。最终Publish后的flush只导出已完成队列记录，后续控制RPC不证明所有后台worker已排空；不完整配对仍报告gap，不增加原生同步。新的传输采样、观测开销及精度没有因此取得运行资格。证据统一在`data/redesign/s5-device-scope`的structure/workflow verification及构建日志；无新原生attempt或raw。周期归零，完整S3–S6继续未完成。
