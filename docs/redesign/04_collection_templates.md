# 采集与子图模板提取

本文件维护采集与模板合同；当前支持状态见[实施计划](../redesign-workflow/2026-10-08_redesign_execution_plan.md)，历史实验结论见[里程碑记录](../redesign-workflow/2026-10-08_redesign_execution_log.md)。旧数据已清空，历史计数和旧路径不作为当前覆盖证明。

按职责查阅：[代表建库与停止条件](#建库方案与结束条件)、[独立性能采样](#独立性能采样合同)、[统一插桩与跨语言交接](#23-block-定义wrapper-与子图获取的统一抽象)、[模板资产](#编译资产与预测加载)、[原生事实交接](#3-原生观测与来源交接)、[交付边界](#7-交付合同与验收边界)。本文编号是设计主题，执行顺序由主计划指定。

首个预测闭环保留 CPU 控制语义与必要依赖，排除 host 控制、准备、提交及完成处理的 CPU 服务时延；计算、通信、数据搬运和物理设备命令继续计时。host 同步复制仍是传输，未来 KT CPU 专家仍属计算。当前代码已删除 host CPU 服务采样、拟合、收费与申采链，实际部署及验证进展以实施计划为准。详细成本范围见[性能合同](06_performance_execution.md)。

以上为现行实现范围。后续分层子图、归一化及 CPU 工作/测量边界统一按[当前专项](../redesign-workflow/2026-10-17_steady_state_template_cpu_plan.md)先设计再实现；已有模型区域和局部共享不等于完整模板库已归一化。提案与待办只在专项维护，本合同按实际阶段交接更新。

KTransformers 的 Ascend BalanceServe TP=1 原生采集与后处理有历史成功记录，复用采集权限、累计预算、设备串行锁、Recorder、Ascend runtime journal 和 ServingNormalizer。原生 wrapper 单列调度 lookahead、prepare、execute、sync、sample、update 和 token 队列 publish，保存 mini batch 与上一批/下一批的实际身份。采集代码保留，清空前的 source facts 不再作为可读取资产；标准步骤、DAG/模板、专家路由及成本模型在主线稳定后扩展。

## 1. 两类采集

| 类别 | 运行内容 | 产物与用途 |
| --- | --- | --- |
| 代表性结构采集（base） | 系统从配置集中选择来源，执行有明确证据目标的短 request | 当前先提供节点/依赖、阶段语义、参数来源和模板变体；适用服务观测以后可供未变化工作直接迁移 |
| 独立性能采集 | 当前计时范围内的计算、通信、传输及设备命令 | S5 所需服务样本、部署与适用域；不能修复流程、节点归属或模板缺失 |

代表性结构采集与独立性能采集是两类建库活动。系统可以选择探索集中的一个或多个配置，用专门设计的代表 request 提取结构，不要求用户指定 base。完整目标 request 序列的原生验证属于独立评价，不能隐含导入建库。结构缺口先按下述分类处理；只有采集能够解决的证据缺失才进入代表采集计划。性能补采不能修复语义；缺规则则明确 unsupported。当前集合入口支持显式多来源输入，按配置、部署和请求池适用域确定首选来源；共同条件成立时可选择不同来源的完整批次并做有限局部替换，见下述组合合同。自动建库行为见[集合流程](07_integration_workflow.md)，当前验收范围见[实施计划](../redesign-workflow/2026-10-08_redesign_execution_plan.md)。

### 代表 request 与机制覆盖

参考 [Dooly 调研](../../reference/README.md)，系统为固定模型和声明配置机制选择短请求，复用共同操作，只取得必要差异。代表请求用于建立结构和参数证据，不充当目标请求或调度答案；无需寻找万能 base，也不实现最优采集搜索器。

### 当前来源配置分组

plan与predict的representative_collection报告经入口注入的TemplateAdapter分派；当前由SGLangTemplateAdapter提供配置分组与配方。公共Planner只汇总结果，不直接解释框架控制参数。只对当前lightweight/cold/modeled路径做配置分组：框架、控制路径和设备数不符的配置明确列入excluded_configurations；部署相同且仅在现有配置关系允许的准入/Prefill预算轴上变化时属于同组。每组选max_running_requests最大的配置，相同容量优先选择max_prefill_tokens更大的配置，两项都相同时保留声明顺序。依据是RequestPoolCapacityDomain只支持保持容量或有条件缩小，不能从小池增加容量；同容量下保留更充足的配方Prefill预算，不是按性能评分搜索最优base。

分组只读取配置，不读取目标 request 序列。增加 workload 不增加组；TP、部署或尚无变换的参数差异不被并入同组。`groups.configuration_ids` 只是共同来源的候选集合，不证明模板、内部参数及分支覆盖。`requires_mechanism_coverage` 表示还须核对已有资产覆盖与累计开销。

`candidate_recipe` 列出短请求关系、预期机制、停止条件和未覆盖项。`plan` 只生成报告；显式 `profile --representative` 执行分组配方，复用公共预算和恢复。`predict` 先消费已有资产，仅缺兼容来源时才进入每组一次的自动代表采集；规则或成本缺口不触发结构采集。分组数不等于实际原生运行次数。配方正文见[代表短请求候选](#代表短请求候选与执行前门槛)，实际支持与证据只在主计划维护。

Dooly的本地run-tracer使用短generate窗口并记录动态维度，resolver再合并重复操作；这些代码不提供Markov的serving过滤/退出/等待分支覆盖保证。这里只复用轻量发现与去重原则，不照搬其默认batch/token数作为本项目采集规则。

### 代表短请求候选与执行前门槛

plan/predict的candidate_recipe是生产端给出的机制候选，可以通过显式profile --representative执行，但尚不是完成覆盖判断的自动建库方案。P取真实worker的effective_server_args.page_size；prompt_page_offsets表示P加偏移，不能从探索request取长度或硬填本机128。令N为代表配置的请求池容量。当前候选要求2≤N≤32、P≥4、Prefill预算至少NP，并预留4NP token容量。预算考虑原生按页对齐输入及输出余量；4NP是本配方的保守充足条件，不是目标KV分配公式。上限32复用生产端已资格化的last-location Concat分支边界，避免为填满任意大池无界增加请求。容量1及超过该分支的候选报告配方未定义；这不等于禁止已有模板预测这些配置。

| 顺序 | 输入长度 / 输出数 | 预期机制及理由 |
| --- | --- | --- |
| 1 | P−2、P−1 / 1、1 | 两成员准入与last_loc、冷Prefill立即完成、逐请求直接释放、共享输出 |
| 2 | P−1、P / 4、2 | 首次Decode中第二成员跨页并结束；后续第一成员跨页、继续、结束，覆盖部分过滤、单成员Decode、零/非零新增页、空/单成员释放组 |
| 3 | P−2、P−1 / 2、2 | 双成员Decode同时结束的拼接释放；入口同时核对上一组完成后的清理 |
| 4 | N+1个P−1 / 前N−1个输出5，第N个输出3，排队者输出2 | 先占满请求池，较早结束一个成员后准入排队的单请求，再与仍运行的成员合批；不修改原生调度状态 |

四组共 N+7 请求、5N+12 输出 token，与目标 workload 数量或长度无关；在当前候选域内最多 39 请求、172 输出 token。每组一起提交，客户端返回且全部 rank 的原生清理确认完成后才提交下一组，不使用猜测的毫秒间隔；实际批次以原生记录为准。

`profile --representative` 使用批量 input_ids、逐请求 sampling_params 和原生清理屏障；普通 workload 采集保持原到达序列。页大小与配方预算在 Engine 初始化后从实际配置解析，批量返回时刻不作为逐请求完成时刻或节点成本。配方成功执行不等于全部目标变换、单来源初始化或机制覆盖已合格。

执行前先对照已有同配置资产删除已覆盖观测需求，再落实真实页大小、配置预算及公共采集生命周期。各预期机制必须在全部rank上实际出现且模板参数/端口合格，否则停止并报告，不能换workload自动重试。当前候选明确未覆盖周期性非最终输出、准入预算停止、内存压力和域外后端变体；这些项需结合既有规则/事实确定复用或缺口，不能凭配方或预期机制标记完整建库。已有资产优先；自动建库仅处理实际缺兼容来源的组，规则或成本缺口不触发profile。


### 来源事实与规则覆盖分开判断

建库需求不能直接由“缺少同形状来源调用”产生。来源清点回答观测过什么，实际模板实例化回答能否从已有结构生成目标；二者不是同一覆盖表。已有变换和独立结构证据能够覆盖的目标，不要求再采同人数、同长度的原生模板。新配方用于从零建立必要证据，其全部触发组不应在已有资产可复用时机械重跑；仍须按真实规则资格决定复用，不能只按机制名称消除缺口。

编译资产已有准入入口/检查工作、批次转换、过滤、合并及发布事实。预测在assets[].source.observations中集中报告，并分别暴露准入CPU、转换CPU和运行批更新的参数绑定资格。现还报告publication_regions、running_update_cpu_only及kv_region：KV记录直接来自生产端已有分支绑定，区分allocation/release_group、是否改变页以及释放成员域；null表示没有该区域资格，不等于没有原生操作。不改变资产格式，不要求重采。TemplateBank.qualify由来源选择和实例化共用，只检查来源身份缺口、准入策略及请求池，不能作为全部机制的覆盖证书。

predict经入口注入的TemplateAdapter将已加载资产交给具体适配实现统一选择；当前实现为SGLangTemplateAdapter，在representative_collection.source_coverage按configuration_id报告供体清单、可提供初始状态的清单及具体缺口；与groups.configuration_ids关联查看。实际预测消费同一选择结果，成本输入只在合格初始来源中决定优先项，不改变结构资格。配置只解析一次，bank及其配置由workflow持有，适配器借用只读视图；不重新读取原始采集。该报告的scope仅为来源身份、准入、容量及初始化，mechanism_coverage仍为not_qualified。独立plan命令只分组配置，不加载资产，不宣称完成这项检查。source_coverage.available只说明来源可选；目标机制与条件仍由实际实例化联合判断，不直接证明全部目标支持。

source_coverage.mechanisms现报告候选配方的14项机制，配方与报告共用机制清单。逐项保存合格供体中的manifest/rank/source_call/source_batch、缺失rank和已有producer_binding；同一调用的多个成员只记一个引用，不当作多个模板。KV跨页与释放成员域直接读取生产端已有有限分支，未增加页数推导或新变换；Prefill直接释放与Decode组末调用free分别解释。producer_binding为null只表示没有报告独立绑定标志，不等于没有原生工作或变换实现。

no_compiled_evidence要求先检查既有观测或规则，不能直接触发采集；有证据仍为requires_target_qualification。各rank分别有某个机制，不证明它们组成同一个完整cohort，也不证明跨来源拼接、参数域和目标状态联合成立。完整cohort及目标变换条件未联合通过前，保持总体not_qualified。

是否需要采集，须同时核对原生分支条件、已有观测和已实现变换。例如AdmissionWork允许已提交请求后的InputBudget结束通过参数化路径表达，不要求每个来源直接观测该停止原因；MemoryBudget拒绝涉及不同的候选检查和持续准入状态，当前这条参数化规则不支持它。不能因停止原因未出现在来源中就自动补采，也不能因另一个分支已有样本而宣称等价。已有规则的目标适用条件仍由实例化检查。

请求池可缩容不代表批人数可变换。`instantiate_step` 允许具有完整参数和成员切口的跨人数候选，仍联合检查全部阶段/rank、操作分支和资源端口；有限支持域见[主计划](../redesign-workflow/2026-10-08_redesign_execution_plan.md#3-保持的支持边界)。不能为更大请求池自动枚举从 1 到容量的全部批大小采集。

跨人数变换需一起处理请求级循环、共享输出/释放及模型连续性；只改 Requests 张量轴或截断请求不足以取得资格。所有目标均通过正式实例化，不保留绕过联合条件的局部诊断入口。来源机制齐备只证明事实存在，不能代替完整 cohort、参数域、切口及目标状态的联合判断。

### 轻量结构发现与有边界的建库

Dooly 通过短 prompt、小 batch、短输出、限定 generate 采集窗口，以及 dummy 权重和 fake-TP 降低结构发现开销，另行测量去重操作。具体依据见参考记录。Markov 优先使用短请求和重复结构复用；dummy/fake-TP 尚未实现或验收，空通信不能证明真实同步，dummy 数据不能证明数据相关路由。初始化、预热和归档计入采集开销；预热不进入预测窗口。

固定模型和已覆盖机制下，新请求应增加实例而非模板定义。每个真实结构分支先解释条件、参数、依赖和端口，再获取最少证据；完整目标序列只作独立评价。

profiler启停属于运行环境观察范围，不增加serving block。AscendProfileWindow记录start/stop调用的rank、进程、原生线程、monotonic起止及完成状态，由产物清单显式引用profile_controls文件。归一化仅在同rank/线程、完整时间包含且不与serving归属冲突时附加profiler_control来源；CANN时间使用已记录导出原点，native API使用自身monotonic坐标。源图保留这些活动，已确认的控制同步报告为scope，不能混入目标等待。失败、无证据、范围冲突和未覆盖线程不自动排除。采集成功不自动赋予其他线程、异步活动或历史无范围记录的活动以相同归属；时钟适用前提与待核对风险分别记录。

### 建库开销的分段口径

SGLang成功采集的`native_environment.json.collection_wall_time_ns`记录宿主单调时钟差值：engine_initialization、request_preparation、warmup、post_warmup_setup、profile_start、observation_and_cleanup、profile_stop_and_export。阶段相邻、不重叠；统一预热及其原生idle清理计入warmup，观测段包含代表请求组间及最终清理握手。profiler停止包含vendor导出，不能把整段当成模型运行时间。

这些字段只描述采集开销，不是目标CPU/NPU节点成本，也不进入成本拟合。范围从Engine构造前到stop_profile返回，排除模块导入、结果文件写出、engine shutdown及独立后处理；外层attempt总量和后处理发布时间继续单独记账，不能把两种口径相加造成重复计量。失败仍依赖已有attempt监控记录，未完成阶段不伪造分段值；历史产物不补写估计时间。字段的实际验证状态只在执行计划维护。

### 采集产物保留

Ascend运行环境适配层在项目的profiling窗口中设置Level2并启用torch-npu的 `data_simplification`。由vendor在导出完成后清理重复的summary/timeline及分析中间文件；公开trace、kernel CSV、通信数据库、profiler配置、host时钟起点和device信息仍由native_capture显式引用。原生观察、API journal、workspace记录不在这一清理范围。该配置目前由SGLang的profiling hook使用，不推广为所有采集后端都已启用。

上述仅为原生导出阶段。规范化事实及模板/性能资产交接后，默认释放本次拥有的 vendor 导出、调用/API journal、初始事件、workspace/profiler 控制记录和通信原始日志；借用来源不删除，诊断模式保留原始包。正式消费者读取保留事实和资产。`native_capture` 在 `raw_exports=released` 时保留 journal 定位符作为成本来源身份，用于样本隔离；它们不再是可重放的原始文件引用。原始字段重解释须在采集时启用诊断保留，不能从压缩成功推导删除安全。实际验证范围只在主计划和专项审查维护。

历史资产保持原样，不追溯删除或重分类来降低累计用量。估计的节省量与新采集实测分开，单次文件规模不外推为任意 workload 的常量预算；预算和当前验证结果见执行计划及执行记录。

### 建库方案与结束条件

代表性结构采集的完成条件必须覆盖声明的原生状态清理。客户端收到最后响应不等于 scheduler 已清空 running 成员；建库窗口应保留实际清理分支，以原生步骤完成作为结束证据，不用固定等待、额外请求或伪造 Idle 节点替代。目标仿真仍在最后 Publish 结束，不计入无限 idle polling。

SGLang base 在最后一次真实空 Schedule 持久化后，由各 rank 记录 source_call 确认；全部确认后才停止 profiler。组间屏障要求累计已清理请求 ID 覆盖本组已提交请求，全部 rank 确认后才提交下一组。中间组刷新但不关闭 native 窗口，最终组关闭，`group_completions.json` 保存实际确认。等待由既有采集 deadline 约束，验证和性能采样保持各自 scope；该生命周期不等于完整机制覆盖。

开始实现或扩大采集前，在现有计划中维护以下材料，不另建规划平台：

| 材料 | 必须回答 |
| --- | --- |
| 模板合同 | 语义边界、端口、状态效果、固定量与动态参数是什么？ |
| 机制覆盖表 | 原生分支如何触发，已有证据与尚缺事实是什么？覆盖目标控制可达分支，不能只列 base 的既有次序 |
| 有限变换表 | 哪些参数、节点、边改变，资源和端口如何保持，哪些范围拒绝？ |
| 取证/验收方案 | 最短触发输入、明确证据目标、累计次数/运行/后处理/存储上限、结束条件及独立输入是什么？ |

同一事实不重复采集，一次短运行可以覆盖多种机制。累计预算跨配置、请求、失败和恢复延续；初始化、预热计入运行成本。分别报告原始实例、结构族、变体、规则和成本样本，新定义必须对应真实机制差异。超出预计规模时先审查抽象，不自动上调预算。

必要分支的参数和端口证据齐备、支持域与域外处理明确后，结束建库并固定资产验收。预算耗尽、观测失败或抽象缺陷只结束本次尝试，保留未完成状态。未知分支条件先查代码或收窄范围，不能靠试采证明覆盖。

具体纪律由[工程约束](engineering_constraints.md#13-不以补采和特例替代参数化设计)维护；联合验收门槛见[验收合同](08_acceptance_delivery.md#固定资产下的泛化与规模验收)。

覆盖分析包含准入、合批、退出、KV 生命周期及模型分支；耗时变化可能改变动作顺序，不能以 base 未出现排除目标可能性。模型结构、原生分支与运行因果分别提供不同依据。每项新增证据说明所支持的关系及覆盖收益；具体变化类型见[语义变换](05_controller_semantics.md)，不在采集合同再维护一套支持清单。

### 模板数量有限的依据

| 复用关系 | 固定部分 | 目标变化与依据 |
| --- | --- | --- |
| 模型内部同构复用 | 操作结构和端口 | 层身份、参数、KV 位置；依据模型及实际后端，层名相似不足以归并 |
| 同分支参数化 | 节点角色和连接规则 | batch/token/context/资源索引；必须有参数来源与适用域 |
| 有限分支选择 | 已声明变体和规则 | 目标配置、请求和状态触发；未知分支返回缺口，不选“最接近”模板 |

短 decode 可以提供重复模型主体，跨页、退出和通信切换仍各需条件。控制器组织目标路径，模板提供有依据的局部工作，成本模型后置；不能用更多整段轨迹绕过关系缺失。

### 从建库转入预测的交接

输出声明支持域内的模板族、参数关系、有限变换、端口和证据引用，说明它们为何足以组织范围内的新请求。目标执行只选择、实例化和连接已定义模板；缺口返回调用方，不在模板查找中启动采集。新证据回到建库方案统一规划，组合间共享需求。

缺口分类与动作统一见[闭环处理](07_integration_workflow.md#4-缺口与补采)：先区分规则缺失、已有事实未提取、必要原生证据缺失和成本不足。采集未取得预期事实时停止诊断，不以不断换请求代替抽象修正。固定资产后的结构泛化、规模和证据用途按[验收合同](08_acceptance_delivery.md#固定资产下的泛化与规模验收)核对，独立性能覆盖另判。

### 原生机制关系的查阅入口

采集公共合同不重复列出每个原生 kernel。当前 SGLang 的通信、KV 分配/释放、采样/metadata、CPU 操作切口和索引写入等条件集中在[原生关系参考](sglang_template_relations.md)。修改相关机制时读取对应小节；开发顺序和尚缺条件见实施计划。

## 2. 插桩先定义语义，再收集事件

### 观察工作与原生工作分开

公共 `ProfilerMarkers.observer()` 使用独立的 `markov_observer` 原生范围，标记 wrapper 的参数准备、完成元数据、记录写出及必要的首次插桩准备；真实方法和内联主体始终在范围外。它不是 serving block，不改变语义身份，也不新增同步或设备值复制。

现有 runtime 提取器按同 rank、CPU lane 的显式范围排除其中的 FUNCTION 操作根与 CPU 服务，保留原始观察、线程顺序及真实提交/等待因果。同名函数在原生主体中仍保留；这项来源隔离继续服务于结构归属，不要求把每段 CPU 工作转成成本对象。

观察范围可以把同一语义主体分成多个 CPU 片段，不能据此取消其语义资格。共享输出仍按区域接口与原生状态绑定；单个来源 span 不是费用定义。首个闭环保留必要的线程顺序、提交、等待及公开端口，不为这些片段补采 CPU 时延。

历史数据没有该身份时不按名字或相邻事件回填。观察范围若实际引入提交、复制或同步，必须保留因果并报告对应观察限制，不能仅删 CPU 时间宣称等价。时钟、marker 进退及未单独记录的标量计数仍是测量限制；该范围不代表全部 profiler 开销已被消除。

为每个框架维护“标准步骤 → 原生位置”映射，明确函数/调用边界、上下文来源、提交与完成关联方式。优先使用官方接口、Python wrapper 或必要的原生 hook，避免每个算子单独手写一套采集器。

统一的是观测合同与消费方法，框架适配负责选择具体位置。每个映射条目要写清动作输入、原生观测点、实例身份、内部工作关联、完成端口、状态效果和缺失项；不能仅给函数贴阶段名。

需要轻量源码插桩时，补丁由自有采集适配器管理，应用于独立运行副本，并明确安装、移除和观测开销；third_party 工作树保持官方基线。插桩只取得边界与元数据，不改变原生调度和业务行为。优先在自有启动层安装 wrapper/hook，源码插桩与必要镜像构建适配分别维护。

插桩记录步骤开始/结束及业务参数，底层 profiler/hook 记录真实工作。函数返回不是设备完成；一个异步步骤可能在返回后仍有工作，通过提交身份、stream/event 和请求/批次关联归属。

### 2.1 统一记录的三类语义

| 记录 | 最小语义内容 | 用途 |
| --- | --- | --- |
| 阶段范围 | 动作种类、阶段实例/父实例、batch、请求集合、phase、rank/thread、输入参数、原生 CPU 范围 begin/end | 表达本次调用及嵌套关系；scope end 不自动宣告设备或业务完成 |
| 工作关联 | 阶段实例与 CPU 片段、提交/API/task、stream/event 或跨 rank 工作的显式关联 | 为真实节点确定唯一归属，传播异步上下文，建立内部及跨模板依赖 |
| 端口事件 | 阶段实例、端口、对应原生完成依据、实际结果元数据与状态变化 | 确定何时数据就绪、外部工作可启动或业务状态可发布 |

时间坐标、单位、部署和来源仍沿用明确的采集合同；此表规定语义，不新增内部版本或另存一套重复格式。Recorder 与 SDK 观察共同提供这三类记录，框架绑定解释端口与归属；是否足以支持目标由实际来源和变换资格决定，阶段状态只见实施计划。

独立目标时延默认只读取Submitted/Published返回端口，不生成上述内部工作关联；完整结构观察使用`observe-target --purpose structure`。两种用途的能力和物理取时边界不同，不能用轻量端口文件替代子图来源，也不能把完整profiler包络当作无扰动真值，见[独立评价边界](06_performance_execution.md#6-输出与误差)。

SGLang base 及现有独立采样的环境记录共用 RuntimeEnvironment，保存 Torch/torch-npu 的安装包版本，以及 ASCEND_HOME_PATH 指向的 runtime/HCCL 版本文件、版本和构建时间。ATB_HOME_PATH 声明 ABI 目录时，同时保存所属安装的 version.info 真实路径和原文（含分支及 commit）；未声明目录或缺文件分别保留为空。信息写入现有环境 JSON 的 runtime 字段，不另建版本握手或资产摘要。缺包、未声明 CANN 路径或无版本文件时明确为空；不扫描候选安装、按目录名猜版本或回填历史资产。读取不导入计算框架、不初始化设备，并在观测范围之外执行。这是当前进程可见的安装元数据，不证明各 worker 实际加载库、驱动行为或旧采集版本；历史缺失仍是未知。

阶段实例身份与源物理工作身份分开：一次批次计算服务多个请求，共享节点归属批次实例一次；跨 rank 的实例通过明确 batch/工作关联连接。父实例保留调用层次，CPU 工作按实际片段归属，不能把父包络与子范围叠加。

异步工作必须保留从提交到设备执行、event 和结果消费的关联。线程局部 scope 只解释当前线程；跨线程、进程或 rank 时由适配器传播/记录必要上下文，不能因为发生时间接近就认定同一阶段。无法关联的必要工作保留语义缺口。

运行时返回的线程最近任务 ID 不自动代表本次 API 新提交的任务。实际框架诊断已出现普通 CPU 内存 H2D 成功后仍返回前一计算任务身份的情况；关联仍须核对任务类型、连接、线程和作用域，不能将复制绑定成计算、因无独立任务身份而填零，或推断耗时全是 CPU 服务。Ascend 已观测的 PCIE_DMA_SQE 与 SDMA/SDMA_SQE 均归为传输，按各自实际来源保留；没有通信观测的 trace 不要求存在通信数据库表。

Ascend的vendor派生torch_to_npu连线需要与原生提交身份一致。适配端通过目标connection ID的唯一CANN提交、同线程包含该提交的Dequeue以及唯一correlation ID对应Enqueue核对来源；原来源不能包含这个已关联Enqueue时，使用该Enqueue作为来源并保留profiler原来源。包含关系只核对同步窗口，不以时间接近推测跨线程或设备归属；证据不足时不猜测改绑。

模板的`internal_thread_paths`保存同一block两端之间、仅经过无归属边界的原生ThreadOrder路径。提取时遇到任何有归属边界即停止延伸，不能跨越其他block；模板编译将该路径变成普通内部线程依赖，供状态资格、子图切分和执行共同消费。被跨过的CPU事件及其耗时仍排除。该字段来自重新extract；既有未编译模板需从原始事实重新提取，不维护缺字段兼容解析。

CPU 调用归属与设备任务绑定分别成立。复制与事件等待共用按 rank/thread/API 类型分组的完整序列核对；数量、已有作用域及既有关联的 CPU 包含关系必须一致。缺失的 source_interval 可恢复为该 CPU 调用，但不能据此生成硬件边或清除设备身份缺口。hardware_intervals 才声明已关联的设备工作，空列表不表示复制或等待无工作。

复制的完成语义还需实际 Host 内存属性及 API/等待边界：非锁页复制的成功返回与锁页异步提交不可互换；CPU 片段完整覆盖不等于其耗时全是主动 CPU 服务。

### 2.2 阶段边界如何落实

先核对职责、输入和状态效果，再选择 wrapper/hook 或隔离副本中的轻量 marker。内部事件只要影响模板外执行或下一次决策，就映射为端口；其余细节留在模板内部。设备完成使用已有原生事件及 profiler 因果记录，发布使用真实业务更新边界，不为观测插入同步。需要捕获阶段中途状态时增加实际观测点，不能用整个函数返回代替。

| 观测内容 | 消费目的 |
| --- | --- |
| 请求、batch、阶段和调用身份 | 哪次标准步骤拥有哪些工作 |
| token/KV 长度、shape、dtype、layout、实现 | 模板参数及其来源 |
| CPU thread、NPU stream、rank、submit/completion/wait | 子图内部及跨模板依赖 |
| 缓存/内存容量、分配、实际传输量、可见量 | 规则和状态效果依据 |
| 墙钟与线程 CPU、时钟关联、观测开销 | 服务测量与等待区分 |
| 软件、设备、后端和实际配置 | 适用域与部署复现 |

默认只记录必要标量和元数据，不快照中间张量、不额外同步设备、不用最近时间戳推断因果。输入 token 作为 workload 的正式输入保留，与采集激活值不同。

### 2.3 Block 定义、wrapper 与子图获取的统一抽象

本节规定统一实现合同。五种主线步骤共用核心定义导出、Python 公共 wrapper 和 C++ 公共子图提取；来源、参数、关键依赖与有限变换资格共同决定可用范围，阶段验收只见实施计划。这里的 block 指语义流程单元，子图模板指其真实工作结构；“模板化”指复用固定处理流程和有限差异实现，不要求使用 C++ 模板元编程。

三者必须强关联：**BlockDefinition → BlockWrapper → BlockObservation → BlockExtractor → BlockTemplate**。定义规定要观察什么，wrapper 提供相应原生证据，提取器根据同一合同取得子图。不能分别维护一套步骤名称、插桩名称和模板判断分支，再靠字符串约定拼接。

| 对象 | 负责内容 | 不持有的内容 |
| --- | --- | --- |
| BlockDefinition，C++ | block 身份、输入参数及单位、端口语义、允许的状态效果、模板适用条件 | 某次原生调用的可变状态 |
| BlockBinding，框架适配 | 将定义关联到具体 wrapper、原生锚点、所需证据与提取规则 | 第二套端口含义或模拟时钟 |
| BlockWrapper，Python | 包装真实调用，读取参数，记录范围、关联和端口证据 | 子图改写、目标调度与成本计算 |
| BlockObservation，交接数据 | 定义与绑定身份、调用/父调用、请求/batch/rank、参数、原生证据引用 | Python 对象、闭包或可执行代码 |
| BlockExtractor，C++ | 按定义和绑定，将规范化事实提取为子图及端口 | 重新运行原生框架或猜测缺失行为 |
| BlockTemplate，C++ | 定义身份、来源、节点/依赖、端口映射、参数来源和合法变换域 | 目标调用进度和源绝对时间驱动的执行计划 |

一次调用还需独立的实例身份。block 种类、框架绑定、原生调用实例、源物理工作和目标 WorkId 分开：多个真实调用可产生同类 block 的不同模板变体，不能把一种 block 限定为唯一子图。定义身份贯穿整条链；原生调用身份在采集窗口内结合 rank/process 消歧，目标实例生成自己的身份并保留来源引用。

#### 统一生产流程与统一模板产物

框架扩展同时遵守[模板生产端边界](01_system_framework.md#框架差异收敛在模板生产端)：产物合同统一，生产过程本身也复用。固定流程为“绑定 block/区域 → wrapper 取得事实 → 关联 CPU/设备事件 → 提取节点与内外依赖 → 建立参数及有限分支关系 → 输出模板或缺口”。不能为每个框架复制 recorder、切图器、模板装配器和目标展开器。

| 公共流程固定的职责 | 框架接入仅补充的差异 |
| --- | --- |
| 统一 block 身份、观测生命周期和事实记录 | 原生入口、参数/结果读取位置、异步关联锚点 |
| 子图所有权、内部依赖、跨图端口及来源追溯 | 原生区域解释与端口对应证据 |
| 参数关系、变体条件和有限变换的统一表达 | 参数从何而来、真实分支的选择依据和模板化映射 |
| 模板装配、合法性边界与公共实例化 | 当前框架可提供的模板与明确不支持的范围 |

统一模板中的状态效果是标准状态操作及其生效端口，不包含框架对象、私有状态机或可执行回调。不同框架可以有不同内部子图，但核心按相同输入、端口和工作描述消费；性能单元粒度仍独立于语义区域，不因换框架另建成本调用路径。

框架差异可以存在于 Python 采集绑定和 C++ 提取/模板化实现中，因此“生产端”是职责边界，不是单一语言或一个大类。Python 负责真实运行侧事实，C++ 的框架生产代码将事实解释为统一模板；C++ 公共消费代码只执行统一关系和图操作。有限变换的来源识别在生产端完成，目标参数求值与图改写通过公共机制执行，不把原生算子名识别延迟到核心预测循环。

当前 C++ 来源入口由 ExtractionWorkflow 读取 manifest 并显式选择已实现的框架绑定；AscendSourceExtractor 通过 [SourceBindings](../../src/markov/include/markov/production/source_bindings.hpp) 取得原生区域解释、路径资格、传输参数与 block 提取实现，自己保留事件关联、CPU 分割和图构建。共同 BlockExtractor 通过同一绑定解释区域，复用子图所有权、依赖和输出过程；SGLangBlockExtractor 的端口解析位于 adapters/frameworks/sglang。绑定在一次提取期间由工作流持有，提取器只借用；无动态注册或框架对象进入目标执行层。该接口已接入 SGLang，其他框架仍须实际实现和验收，不能仅凭抽象声明支持。

#### 公共流程与浅层继承

Python 的 BlockWrapper 父类固定“读取调用输入 → 建立观测范围 → 调用原方法 → 记录结果/异常 → 恢复父范围”的生命周期，复用 Recorder。SGLang 等具体 wrapper 只实现原生参数读取、结果读取和必要关联；原方法只调用一次，返回值与原异常继续传递。部分端口需在调用中途或其他线程观测，由绑定明确补充锚点，不能全由退出函数补齐。

C++ 的 BlockExtractor 父类固定“取得一次调用的事实 → 确定工作归属 → 保留节点/依赖 → 解析端口 → 建立局部身份和参数映射 → 输出模板或缺口”的流程。具体提取器只实现有实际差异的归属、端口证据解析和参数映射。公共流程由普通成员方法按固定顺序调用少量虚方法，避免每个子类重写整个提取过程。

BlockDefinition、BlockObservation、BlockTemplate 优先是有类型的值对象。仅有参数或端口集合不同的 block 通过数据定义，不强制每个名字建立子类；确有不同算法的 wrapper/extractor 才继承。框架绑定以组合关联定义与实现，避免“框架 × block × 模型 × 配置”多层继承。首轮显式构造所需对象，不建立动态插件发现、万能回调注册中心或规则解释器。

跨 block 的关联由公共提取流程协调：先建立调用与物理工作索引，再提取各 block，最后保留跨模板边和共享边界。具体提取器不能独立截取时间窗而丢掉外部依赖，也不能分别把同一物理节点复制为可计费工作。父子范围描述语义包含关系，节点所有权与性能计费另行确定。

#### Python 与 C++ 的关系

代码分层及修改归属以[当前目录与职责](development.md#c-目录与职责)为准：框架插桩、模型模块声明和运行环境解析分别归入适配层；统一 wrapper/观测合同、子图所有权与模板消费保持独立。采集方法随原生版本变化，不改变核心流程合同。

统一语义合同由 C++ 核心维护，扩充现有 ServingContract 的职责，而不是另建并行的步骤体系。现有 prepare 路径把本次所需 block 定义及绑定标识写入规范化采集输入；Python wrapper 消费这份数据，不能再手写一份端口及必需字段清单。Python 与 C++ 的实现分别使用各自语言的父类，通过同一合同与事实记录连接，不跨语言继承。

框架绑定描述在自有适配中维护一份，关联 block 身份、Python wrapper 类型、原生锚点和 C++ 提取规则；由输入准备流程显式选取并交接。类型到实现的装配留在各语言的适配入口，语义内容不重复声明。不把任意模块路径或脚本作为用户可执行配置，也不为了传递少量合同引入绑定库、常驻 RPC 或代码生成工具链。

Python 负责安装 wrapper、调用原生框架、保存观测，以及把 vendor trace 规范化成事实；C++ 的生产端负责语义归属、子图提取和参数/变换关系绑定，公共消费端负责统一模板实例化、有限图变换、轻量控制与执行。当前 SGLangTemplateBindings.produce 在生产期间完成文件/配置核对和各项原生绑定，PredictionWorkflow 选择生产端，TemplateBank 构造只接收 TemplateSource 值；消费者不再通过 Project 自行创建 SGLang 生产类。Python 可以依据显式身份关联事实，但不能提前生成另一套目标结构或按 block 名猜端口完成。C++ 不导入 Python，不在模拟期间加载原生框架。

逐请求组件可在 NativeRegionBinding 声明 request_id_path，公共 NativeRegionWrapper 按实际调用参数路径读取身份，与 CPU 标量及调用/父调用身份一同记录；路径的原生含义属于框架绑定。声明了请求路径的区域必须提供字符串身份，非逐请求区域不产生该身份。SourceNativeRegion 保留来源 request_id 供生产绑定使用，后续须编译为来源请求槽位；目标核心不按原生 ID、kernel 顺序或长度匹配请求。当前只完成记录合同和 ChunkCache 绑定，尚未取得新区域的原生证据或目标槽位关系；旧 prepared input 应重新通过 prepare 生成，不提供旧字段别名或回退。

合同与事实随现有 prepared_input、source facts、manifest 交接，不另设内部版本、摘要或重复状态账本。输入准备时核对已装配定义与绑定；事实进入 C++ 时核对身份、所需证据和参数类型。缺少必要锚点、异步关联或端口依据时报告对应 block 的语义缺口；不能用 scope end、最近时间或零耗时补齐。

#### Prepare 示例与扩展原则

当前实现由 [SGLangBindings](../../src/markov/src/adapters/frameworks/sglang/bindings.cpp) 共同声明五种步骤的原生 hook、可选 phase、wrapper 类型和端口锚点；[BlockWrapper](../../src/collection/markov_collection/instrumentation/blocks.py) 按核心定义取输入并复用 Recorder 生命周期。[BlockExtractor](../../src/markov/src/production/block_extractor.cpp) 统一节点归属、内外依赖、参数与模板装配，并对缺失标准出口报告 block_port_not_observed；SGLang 子类按绑定解析四种锚点：子 Prepare 进入、父 schedule 返回、当前范围返回及所属设备完成。父子锚点核对 rank 和 batch；同一 schedule 存在多个候选 selected 锚点时报错，不任选一个。新 scope 记录 binding（非绑定观测为 null）；旧捕获没有这项证据时不伪造绑定，需要重新采集，历史资产不覆盖或删除。prefill/decode 共享原生 run_batch，安装时按 hook 分组，只注册一次 wrapper 并按实际 phase 选择绑定；publish 在退出观测范围、写完记录后才关闭原生采集窗口。独立 CPU 采样继续使用自己的测量边界。真实验证结果记录在执行日志。

Prepare 的定义声明输入参数和 prepared 端口含义。SGLang wrapper 记录 prepare 调用、父 schedule 身份和实际批次参数；父 schedule wrapper 同时提供原生返回锚点。提取器按该绑定，将 prepare body 与父 schedule 的准备后处理连接起来，把父 schedule 返回映射为当前主线的 prepared，保留底层 flow/wait 依赖。实际 parent 调用链保持原样；SourceCall::serving_parent 跳过中间观察区域，统一提供最近的 serving 父阶段，用于语义继承、Prepare 批次交接、选择锚点、CPU 后缀和区域归属。新增观察层不能改变这些阶段关系。不同原生边界共同实现一个语义 block，不要求“一层 wrapper = 一个函数 = 一个完整子图”。

模板进入目标执行后，控制器按定义发起调用，运行时实例化子图、连接同义端口，并在给定节点耗时下产生完成事件。源端口时间仅作证据；目标端口时间由执行结构重新计算。成本单元仍可细于 block，不在 wrapper 或提取父类中绑定整块耗时。

新增框架优先提供既有 block 的绑定和必要差异实现；新增语义 block 则同步定义、观测、提取、控制调用与端口消费。无法满足既有语义的原生步骤明确作为差异处理，不为了接口相同伪造对应关系。

## 3. 原生观测与来源交接

### 原生输入清单与重新处理

`native_capture` 显式声明下列输入，normalizer 不靠目录扫描或时间接近猜测来源。重新归一化只引用该次原始捕获中实际保留的文件；缺失事实不从当前容器或默认配置补造。下面是现有输入合同，不是本轮拟定的输出目录。

| 输入 | 唯一事实与处理边界 |
| --- | --- |
| 调用记录、`native_api` 与 `profile_controls` | 调用/父调用、scope、queue/API 身份及 profiler 活动窗口；控制时钟和必要因果，不直接产生 CPU 服务成本 |
| `event_history` | 窗口前的 record/query/destroy 与单调入口标记，生成 `event_states`；已观察 record 且未 destroy 不等于 pending 设备任务或完整 watchdog 状态。缺记录保留 `entry_history_not_observed` |
| `communication_logs` | 明确 rank/pid/path 的原 worker plog；解释 communicator、HCOMM/设备信息、operation 对应 transport 和原生展开环境。缺字段为 null，不能把 `op_expansion_mode=0` 当作 host 展开证明 |
| `traces[].device_info` | worker 的 `PROF*/device_*/info.json.<设备编号>`，排除 `.done`；cube/vector 核心数来自真实设备记录，缺失保留 `device_geometry_not_observed`，不以 kernel Block Num 代替 |
| `traces[].database` | 只读 profiler DB，以 COMMUNICATION_OP/STRING_IDS 及 connectionId/操作名关联 trace，取得通信组名；数据库字符串索引不能跨 rank 比较 |
| `workspace_states` | 各 rank/进程的窗口初末容量及分配连续性；缺观测不填空缓存。目标容量状态与需求规则按[workspace 合同](sglang_template_relations.md#workspace-容量状态与设备级同步)消费 |

`rtEventQueryStatus` 的 handle、返回码及成功时的状态仅记录原生查询，不追加查询或同步。它没有 device 提交身份，不能替代 event record 关联；跨窗口和重叠调用仍保留缺口。原生设备同步通过完整同线程序列和 scope 关联，数量/归属不一致时不生成猜测前沿。历史 SDK 局部验证与当前 serving 资格分开，由主计划和审查记录说明。

重处理路径由变化位置决定：原始字段或归一化变化先重做规范化，来源节点/依赖变化用 `extract`，仅完整来源上的模板绑定或编码变化用 `compile-templates`。已有事实缺失原生语义时，重编译不能补出证据；是否需要新采集回到建库需求和预算，不自动重采。可执行方法见[开发说明](development.md#5-已有-base-的-source-提取)。

自有 [SGLang observer](../../src/collection/markov_collection/adapters/frameworks/sglang/observer.py) 通过官方插件的 AROUND hook 取得以下原生边界。记录的是原生调用树，须经提取、参数化和资格判断才能成为可执行模板。

| 原生观测 | 已接入位置与内容 | 模板提取需要保留的关系 |
| --- | --- | --- |
| schedule | Scheduler.get_next_batch_to_run；可见请求、调用身份及选中 batch | 函数内部调用 prepare；selected 在 prepare 入口，原生返回还包含准备后的 bookkeeping |
| prepare | ScheduleBatch.prepare_for_extend/decode；每请求实际输入/context/prompt/generated 长度 | 父 schedule 身份与独立 batch 身份；不能直接以整个父包络计 CPU 成本 |
| prefill/decode | Scheduler.run_batch；工作阶段、batch、请求和上下文 | CPU 返回不是设备完成，保留后续异步队列、stream/rank 和通信关系 |
| model_forward | TpModelWorker.forward_batch_generation；实际设备输入 shape/dtype/stride/device | 当前官方路径在 prepare 后物化输入；此子调用归属于外层 prefill/decode，不重复计费 |
| publish | Scheduler.process_batch_result；实际生成 token 数及完成标志 | 保留状态发布与内部同步/搬运依赖；host 完成处理不计时，真实传输不能随 CPU 一并排除 |

[Recorder](../../src/collection/markov_collection/instrumentation/recording.py) 保留调用/父调用、rank/pid/tid、单调起止和配对墙钟；已删除没有消费者的线程 CPU 计时，同一调用身份写入 profiler marker。[ServingNormalizer](../../src/collection/markov_collection/adapters/runtimes/ascend/normalization.py) 读取生产者明确列出的 trace、kernel CSV 与 profiler 时钟信息，通过原生 queue correlation、flow 和 connection ID 关联物理工作。微秒十进制时间戳直接转整数 ns，避免 epoch 浮点精度损失。缺少 task ID 的 collective 汇总单列保存，不填 0。

[原生 API 观察库](../../src/collection/adapters/runtimes/ascend/native/observation.cpp) 在对应 profiling 容器以 C++23 构建，通过 LD_PRELOAD 补 event、stream 和 memcpy 的标量身份。首个 serving 观测开启窗口，最外层步骤及停止 profiler 边界显式刷新；嵌套 scope 写入使用普通文件缓冲，不逐模块显式刷新 Python/native 日志，close 仍执行收尾。异常退出已进入的 scope 也记录结果，但进程被强制终止时未刷新记录可能丢失，此策略不提供事务持久化，也尚未测得开销收益；不新增设备同步，不收集预热和 import。native_capture.json 明确引用 native_api JSONL；归一化保留原生进程/rank、句柄、stream ID、字节量/方向、配对时钟、调用结果及原始行引用。这些记录不额外计 CPU 成本。

真实 Qwen3-32B TP=2 已验证拦截。SDK 的 rtGetTaskIdAndStreamID 在提交 API 返回后立即提供当前线程的 task/stream；归一化通过该身份和硬件 connection ID 关联 CPU 来源，不用 profiler 时间包络或最近时间猜任务。SDK 最后 task 不一定由 memcpy 新提交，类型、重复身份或 CPU 来源不符时保留缺口。Recorder 进入/退出阶段时向原生观察库设置/恢复线程局部 scope_call_id；同步等 API 直接携带调用身份，归一化核对原生 pid/tid 后解析阶段归属，嵌套 model_forward 的 owner_call 归入外层 prefill/decode，scope_call 保留其自身身份。不再用 host API 的时间区间包含关系补归属。异步线程未继承该身份时记录 null，提交工作继续通过 SDK task/stream、connection 和 profiler queue/flow 关联；显式身份与来源冲突时报告缺口。阶段身份不证明同步等待前沿，也不代表自动传播到所有工作线程。当前原生日志必须包含 scope_call_id；需要重新处理但缺该字段的历史捕获应明确补采，不增加兼容猜测。

ServingNormalizer 在 base 角色下的初始 manifest 引用规范化输入、有效原生配置、source_facts 和原生捕获描述，source_dag/templates 为 null。[AscendSourceExtractor](../../src/markov/include/markov/adapters/runtimes/ascend/source_extractor.hpp) 随后生成 source_graph.json 与源子图实例；只有无环检查通过才填写 source_dag。readiness 仍为 needs_data，gaps 保留采集缺口，extraction_gaps 由提取阶段替换维护。合法 source 图不等于依赖完整、模板可执行或成本就绪。下游不得从 performance 事实补生成语义模板，也不得把 observed_wall_ns 或 thread_cpu_ns 直接视为已去除等待、嵌套工作和观测开销的服务。

运行证据入口见[里程碑记录](../redesign-workflow/2026-10-08_redesign_execution_log.md)。本文保留交接规则；下文旧计数和旧数据地址均为历史观测，不作为当前进度或可用资产。输出收敛的实施顺序由主计划管理。

### 3.1 当前 source 图与源实例合同

[SourceGraph](../../src/markov/include/markov/graph/source_graph.hpp) 分别保存边界、span 和有依据的关系。边界坐标为源观测 ns，不是目标模拟时间。CPU 注解按原生 thread 对齐，CANN 的合成 process ID 不形成额外 CPU lane；设备与通信端点保留独立身份，相同时间戳不会自动合并它们。原生 flow 保留 API/queue 的原始起点，不强制改连到 CPU 返回。

原生 CPU 包络保留为非叠加注解。额外提取 schedule 前段、prepare 本体、schedule 后段、execute 与 publish 的控制片段；schedule 后段归入 prepare，prepared 才能对应原生开始执行模型前的就绪点。片段记录真实来源，但墙钟仍可能包含等待或观测开销，不作为已测服务。model_forward 为嵌套观测，物理工作归外层 prefill/decode。

原生 rtStreamSynchronize/WithTimeout 另形成 host_wait span，source_api 指向原生 API 记录，source_interval 为 null；其 CPU 线程、阶段身份及配对时钟来自该记录。成功返回只连接同一进程、stream 句柄及 stream ID 上、调用前已成功提交且能关联硬件的工作结束点，关系为 native_host_wait。它表示已知的必要依赖子集，不证明整个等待前沿完整；并发提交、未观测 kernel 提交以及 stream 生命周期变化仍需另有证据。失败返回保留观测，不生成成功完成依赖。

CPU control 片段在同一原生线程的 host_wait 边界处分段，保留等待前后的工作；重叠/嵌套等待按区间并集扣除，原始 CPU 注解和阶段端口仍保留。此处时间区间用于同一线程上的片段切分，不用于猜设备因果或工作归属。

在非等待片段内，已有 native_flow 的 CPU 源/目标边界也作为切分点，保留排队、跨线程交接与设备提交发生的位置；不把每个注解端点都扩展为服务节点，也不将提交改接到 API 返回来制造屏障。当前目标图保留这些关联点和必要顺序，纯 host CPU 片段作为不计费注解；同步 driver 搬运叶子仍是传输服务。历史包含 CPU 给定耗时的运行按原范围解释。剩余片段仍可能含未观测等待和探针开销，不能直接将墙钟差当 CPU 服务；历史 thread_cpu_ns 也不能按墙钟比例分摊成节点成本。

原生阶段主线程以外，已归属 CPU 注解的覆盖区间另提取为 dispatch 控制片段：按同线程区间并集处理嵌套，同一时段只保留一份工作及可追溯注解来源，再沿已知等待/flow 边界分段。不同阶段的归属若同时覆盖同一片段，报告 cpu_dispatch_scope_conflict，不任选一个阶段。未归属注解和无观测的线程空隙不补成服务；dispatch 的主动服务成本不再作为首个闭环的补齐任务，结构归属与依赖资格仍保留。

host_wait 不纳入 device_done 汇合，也不能按观测墙钟当作服务成本；原有同步缺口继续保留。若关联设备结束晚于原生同步返回，另外报告 native_host_wait_clock_order_disagrees，保留设备 interval、CANN 提交 interval、原生等待 API 引用及差值；不改时间戳，也不因图无环忽略该矛盾。

不同原生 task 按各自物理 stream 的 FIFO 关系连接；同一 task 的子任务不因时间顺序而全串行化。实机记录存在同一 task 内重叠的 SDMA。compound task 内部依赖须由后文 FFTS 关系等实际依据提供。SDMA_SQE/MEMCPY_ASYNC 属于传输观测；字节量与方向须关联原生来源，不能默认复制参数。

HCCL Notify 通过原生 collective 名、Notify 身份、生产/消费 rank 和各侧 task/subtask 顺序匹配复用代次，保留引用到原始明细。接收侧 src/dst 的方向与发送侧相反。关系连接 signal 完成与 wait 完成；不把原等待区间当服务，也不因时间接近猜配对。

硬件 EVENT_WAIT 使用原生进程内的 event 句柄、record/wait 调用及 SDK task 身份关联。record 已返回后进入的 wait 使用该代次，destroy 清除句柄关联；并发或缺失的 record 不猜代次。SourceGraph 的 native_event_signal 保留 producer 的 native_api 索引，连接 record 硬件完成与 wait 硬件完成。event 配对不能代替 host synchronize 的作用范围和等待前沿，后者使用独立的同步关系合同。

templates.json 的 calls 表定义源调用索引；instances 记录 source_call、rank、参数、实际输入布局、所属 span、native_api 索引和边界端口。source_state 保留 schedule 入口的 visible_requests 和 publish 返回后的 published_requests（request_id、generated_tokens、finished），供源结构和状态效果检查。可见列表保留原生次序与重复项；不是目标准入规则，也不是请求到达时刻。发布结果仅说明本次源调用后的状态，不将原生完成成员复制为目标终止条件。每个实例的 internal_edges 引用 source_graph 中两端都属于该实例的关系；cross_template_edges 保存归属集合发生变化的关系，unassigned_edges 保留两端均未归属的关系。共享边界通过 shared_boundaries 明确列出，边可以同时是某实例的内部关系和另一实例的外部关系；不能据此复制或重复计费工作。input/output_boundaries 按各实例实际进入/离开的归属变化确定，共同拥有两端的实例不会被误标为自身的跨模板输入/输出。三类引用共同保留整个源图的依赖，不丢弃未归属路径；空调用集合表示经过未归属源工作，是否可排除必须另有 scope 依据。它们仍是源关系索引，不是已重绑定身份的目标节点，CPU 注解也不能直接当作独立服务节点。submitted 使用原生 CPU 返回，device_done 是所属设备结束点的纯逻辑汇合，published 使用原生完成处理返回；三者没有强制等同。当前根据真实 model_configuration.architectures 与配置声明有限规则：Qwen3ForCausalLM、固定 Ascend/TP/后端、FCFS、无缓存/overlap/graph/mixed/分块时为 native_dense_module，实际消费者按编译后的参数关系和有限变换重绑定目标工作。其他源实例保持 observed_subgraph，不推定任意 shape 变换；只有模块入口或外层 shape 不足以取得整个子图资格。具体支持域及结构验收见实施计划。

base 窗口按[建库结束合同](#建库方案与结束条件)保留实际清理，validation/performance 仍到最后 Publish。早期窗口和关系计数仅属于历史提取记录；图生成或 flow 配对不证明同步前沿、时钟和通信依赖完整，当前资格以主计划为准。

### 3.2 历史 CPU 性能边界观测

旧 CPU 采样曾记录阶段、完整成员体、transition、函数/队列及最后一次原生同步的 CPU 时钟；这些工具和费用路径现已删除，历史记录与候选保留原口径。旧的联合采样完成、计数或 CPU 覆盖不授予当前范围资格，也不再形成补齐 CPU 服务的任务。

原始调用身份、控制归属、提交/等待因果和真实 driver 复制事实仍可用于其原有证据目的。新 transfer 路径只生产成功完整的 driver 搬运叶子及实际工作域，不将旧 CPU 包络、线程时钟或完成代理改成传输样本；具体实现与部署限制见[同步 host 传输采样](#51-同步-host-传输采样)。

### 已知 stream 同步前沿的传递压缩

原生 stream 同步与 CANN 同步记录按 rank/thread 的完整调用序列关联，同时核对 API 种类与阶段归属；数量或序列不一致时报告身份缺口，不按最近时间配对。设备提交通过同 rank 的 connection_id 唯一关联到 CANN CPU 调用，以同一 profiler 时钟中的提交返回与同步入口判断已提交工作，覆盖此前仅靠原生 event/memcpy 日志遗漏的计算任务。提交跨越同步窗口或身份不完整时保留前沿缺口，不声称完整覆盖。此实现遵循 [CANN stream 同步语义](https://www.hiascend.com/doc_center/source/zh/CANNCommunityEdition/910beta1/API/runtimeapi/aclcppdevg_03_0020.html)，具体观测关联仍须由当前采集验证。

成功的原生 stream 同步仍只覆盖已观测且在调用前提交完成的工作。提取器使用源图已经建立的任务顺序链，在每条连通链上保留最晚候选任务的全部已知子任务端点；较早候选通过 interval_extent、completion_join 和 stream_order 到达这些端点。重叠任务组之间没有已建立顺序时分开成链，不按最大时间戳补关系。

这只是等价移除冗余直连，不能补全未采到的提交、并发调用或 stream 生命周期。时钟矛盾仍按全部原始候选检查。原始 source facts 保留；源图中 native_host_wait_dependencies 表示压缩后的直接边数，不再表示全部传递覆盖的提交数。该处理为目标前沿重绑定提供较小的连接集合，目标跨迭代规则仍需独立落实。

### 内部 kernel 参数的事实交接

source_graph.kernels 按 source_span 保存原生 kernel 名称、operation、输入/输出 shape、dtype 和格式。AscendSourceExtractor 从 source_facts.kernel 提取，不从请求长度猜形状，不把区间墙钟时间加入参数。厂商 tensor 文本（含分号、引号及缺失可选参数槽）原样保留；目前它是观测值，尚不是可求值的目标形状表达式。

生产端按源 span 装配 SourceKernel，TemplateNode 借用对应只读描述；描述存储和节点由同一 TemplateSource 持有，移动交接保持引用有效。TemplateSource 将定义、参数域、来源与范围说明一并交给 TemplateBank，不携带原生解析器或生产回调。没有 kernel 元数据的节点保持无描述，不能视作标量或空张量。开启诊断时，details 的共享 source_definitions 中 observed_kernels 公开该身份与观测字段；目标实例通过 source_definition 引用，不再逐次复制，供核对内部形状和有限变换的覆盖范围。源图新增字段由 extract 正式生成，历史资产需要在明确派生目录重提取，不增加旧图格式回退。此交接不声明任意形状变换已实现，也不构成节点成本模型。

普通 DMA 使用独立的 SourceTransfer，保存 bytes、公共 host/device 方向及 source_api 来源，不能伪装成计算 kernel。通信合同已描述的物理任务先排除，避免重复描述或误报缺口。普通拷贝只从成功的 rtMemcpyAsync 与唯一硬件 DMA 的已有双向关联生成描述；缺关联、未知方向或一对多工作量分配不明时保留观察缺口，不填零或重复分摊 API 总量。源图 transfers、模板 observed_transfers 与实际目标图的 transfer 字段贯通同一只读资产。它本身只表示源工作量，不代表目标字节数关系或成本资格。生产端通过TransferParameterLayout声明工作量关系：目标请求元素数、输入token总量、过滤后成员或固定人数分支。插入时必须同时有SourceTransfer和匹配关系；冷Prefill的单请求last_loc Cat仍限定固定分支，Decode长度复制复用请求元素关系。其他 DMA 仍分别报告缺观测、缺参数关系或请求数域外，不能因已有 bytes 而直接取得跨形状资格。旧资产使用 extract 在明确派生目录重提取，不加字段缺失兼容回退。

kernel_parameter_coverage 现按源 span 取模型布局、attention 布局和实际 kernel_overrides 的并集，包含请求池、KV 分配/释放及成员操作，避免重叠计数或漏算 serving 参数关系。RequestPoolLayout 对已识别但目标形状未变的布局仍返回规则结果，nullopt 只表示不属于该规则；不能把未发生数值变化误判为无映射。parameterized_kernels 表示具有参数关系的节点数，unmapped_kernels 是其补集，partial_operand_layouts 继续单列。此统计不证明全部操作数、CPU/拷贝、实现分支或完整依赖均已合格，不能用归零代替全图资格验收。

ForeachCopy 已定位到 EagerRunner.load_batch → registry.fill_from → 按设备与 dtype 分组复制。原采集容器的框架文件与 checkout 一致。NPU 专用 allocator 的 alloc_extend/alloc_decode 均返回 out_indices.int()，因此 out_cache_loc 是 INT32，而 registry 目的字段为 INT64；它形成独立的转换组。四个 INT64 成员依次为 input_ids、positions 的 T 轴和 req_pool_indices、seq_lens 的 B 轴。当前 ForwardInputCopies 在明确 model_forward scope 下，另要求已有 aten::_foreach_copy_ 操作的 device_spans 成员身份，再核对完整 dtype 与布局。四个 INT64 成员使用 T/T/B/B 轴，独立 INT32→INT64 Cast 使用 T 轴；两者复用现有轴变换，不按数值相等或时间邻近猜字段，不新增角色、模板族或观测入口。原推导使用的源码快照及验证数据已清空，历史映射计数不作为当前覆盖证明。这里只绑定工作量轴，不将 base 的 KV 地址复制到目标状态，也不声明完整目标拓扑或地址语义已合格。

### 历史 base 工作负载补充方式及当前限制

以下记录已有单 base 采集方式，不构成缺模板即补采的规则。只有确认缺少已定义结构分支的证据，且纳入本章建库方案后，才使用明确记录的代表工作负载补充。长度或批次大小不同本身不能证明存在新结构变体。采集工作负载与待预测工作负载可以不同；前者提供原生语义证据，后者仍由轻量控制器按实际到达和目标配置动态展开。模型、来源配置与部署继续由模板导入边界核对，不能借此导入目标验证执行结构或目标耗时。

例如 max_running_requests=2 并不意味着只能观测双请求 decode：在同一 base 运行中让一个请求单独到达，可以取得单请求变体。延迟到达只是采集条件，不迁移成模拟等待或固定调度。是否真正取得所需 batch/context、发布分支及依赖，以原生记录为准；不能根据采集配方宣称覆盖完成。原始 capture 和 manifest 保留该工作负载来源，公开预测继续使用用户原来的 workload。


### 批次过滤事实与调度模板条件

SGLang 的 schedule wrapper 现在同时收集其作用域内真实 ScheduleBatch.filter_batch 调用。辅助 hook 仅在调用前后读取 Python 请求身份，按发生顺序记录 before/after，既保留部分压缩，也保留全部保留、全部移除和空批次调用；不修改 keep_indices、不添加设备同步，也不根据 shape 推断是否发生过滤。它是标准 schedule 内的辅助观测，不增加 serving 步骤或独立计时层。

原生 schedule 的 result.batch_filters 经规范化事实交给 AscendSourceExtractor，C++ SourceBatchFilter 保存身份列表，BlockExtractor 将其写入 source_state.batch_filters。提取合同要求该字段；缺字段不能自动解释为空过滤序列，应先检查已有原始记录能否重提取，确实缺失必要事实时才按覆盖表规划观测。请求身份用于追溯；模板条件已转换为输入槽数与保留槽位，并与目标控制状态匹配，不直接比较原生请求 ID 或照抄 base 调度决定。

控制与设备节点共用 BatchFilter 值类型。BatchLayout.filter 仅处理标准 Schedule 直属 scope 的原生 Index，要求该调用的记录中只有一次非空且实际删减成员的过滤；全部保留和清空在原生 filter_batch 中直接返回，不作为设备 Index 的来源。多次实际过滤但缺乏独立 scope 时返回归属不明，不能靠 kernel 顺序或相同 shape 选择其中一次。

BatchLayout 在生产端绑定原生批次行关系：过滤 Index、合并 Concat 及 Prepare 直属长度 Add/Sub。来源名称、scope、dtype、完整操作数及所需成员身份在建库时解释，输出 BatchParameterLayout 或来源缺口。通用合同区分过滤序列、合并和当前请求行数，显式列出输入/输出行槽；具体原生关系见按需参考。TemplateBank 消费已绑定关系，不再调用 BatchLayout 或解析原生操作。

目标求值保留过滤调用数量和有效分支限制；过滤索引直接来自目标 BatchInput，合并保持左批次后接右批次的顺序。数据行数、索引向量长度和合并总行数分别计算，其他维度及 descriptor 槽保持来源值。来源不合格时返回建库保留的缺口，不尝试通用 shape 猜测；该合同不增加任何跨成员分支改写能力。

规则依据 ScheduleBatch.filter_batch 及 SamplingBatchInfo.filter_batch 的按行索引：数据输入第一维为过滤前人数 N，索引向量长和输出第一维为保留人数 K；向量或矩阵的其余维度保持来源值。Index 两个描述输入分别保持 [1] 和 [tensor_rank]，不因 N/K 的数值相同而替换。节点 batch_filter 直接复用目标保留索引；过滤调用数量或空/非空分支变化需要结构改写，目前不由本规则支持。未改采集 wrapper，未增加第二套状态或采样模型。

当前短资产两 rank 共核对 14 个 Index，覆盖 INT64/INT32/FLOAT 向量和 FLOAT 矩阵，原始 before/after、目标索引及完整操作数一致。去除新增 batch_filter 描述后完整物理图与前轮相同；既有布局、覆盖值、执行摘要和缺口不变，未补采。证据在 data/redesign/s3-batch-filter/filter_audit.json。该结果仅建立已有分支的参数关系；合并或过滤的拓扑改写、多次过滤归属、跨形状 launch 与跨配置展开仍待资格化，精确输入和成员操作条件的全图门槛保持。

此前较大的覆盖 workload 已取得分开 prefill、部分保留和中途清空分支，并让其同配置与目标准入 1 完成条件展开；该历史结果只说明对应实例变体可用，不证明当前短资产可以泛化。该次短资产的 lower-admission 曾为 unsupported；后续状态以实施计划为准，完整源依赖及变换资格仍需单独验收。


空调度的原生 batch_id/phase 保持 null，模板读取后使用 idle 调度阶段，不伪造 batch 编号。跨模板资源关系改用同 rank 原生 schedule 调用序号区分前后，同一批次的四个步骤共享所属 schedule 顺序；该序号仅用于源关系归类，不作为目标时间或目标决策。采集在尚有后续请求时保留对已结束成员的清理，即使当时没有活跃请求；base在原生清理确认后停止观察尾部轮询，validation/performance仍在所有请求发布结束后停止。

Idle 的区域记录应能区分 transition 的 pending 清理与 update 的 running 清理；空返回路径没有 Prepare，Selected 使用 Schedule 返回。过滤前后成员、transition/admission 入口以及原生退出事实沿现有 wrapper/result → source facts → templates 交接，不另建 Idle 记录通道。细节见[空调度合同](05_controller_semantics.md#空调度决定与等待请求)。

Schedule 的 worker_controls 记录实际 waiting_timeout_seconds、running_timeout_seconds、已加载 scheduler 模块的 test_retract 和 worker 的 require_mlp_sync。超时从工作进程内同一 envs 读取器读取，后两项读取实际模块/对象；不拿父进程环境或当前默认值补历史记录。字段由统一 Schedule block 合同声明，沿现有 wrapper → SourceCall → source_state → 模板生产端传递，不新增开关专用 wrapper。生产端要求每个 rank 的 Schedule 都有事实：超时 ≤ 0、无强制回退、无额外 MLP 同步才满足当前控制路径；缺事实返回 observation 缺口，启用未实现路径返回 rule 缺口。该记录与判定链已通过真实联合观测的 32 条 Schedule 核对（raw → source_facts → templates），证据见联合采集记录；旧资产不因此获得资格。

### 批次合并的左右成员与布局

当前固定非 overlap 主线中，Scheduler 先过滤上次 prefill，再将其追加到 running batch；随后进入 decode 时过滤合并后的 running batch。仅当 Schedule 的 decode 路径恰有这两次记录、前次保留成员非空、后次过滤前成员以该列表结尾且有非空左侧时，才能从现有身份事实建立 source BatchMerge。它说明 running 在左、保留 prefill 在右，不按 Concat 的数值形状猜两侧人数。这是无显式合并区域的已有资产所能提供的身份依据。显式 merge wrapper 现已编码，由区域参数交接左右规模并与过滤事实核对；合并可先于下一批 Prefill，详细边界与尚未取得的运行证据见[Schedule 合并区域](sglang_template_relations.md#decode-首批复用的结构分支)。

轻量控制器在追加 pending_prefill 之前记录目标左右人数，之后才进行最终过滤。这两个数与最终计算批次大小可以不同。空侧接管批次不记作实际合并，不补造 Concat。BatchInput 将同一构造关系带给该批各动作，只有 Schedule 应用成员变换，不能在 Prepare/Execute/Publish 重复执行或计费。

BatchFilter 与 BatchMerge 共用 batch_membership.hpp，BatchLayout 统一提供 filter/merge；原 BatchFilterLayout 文件和类已替换，不保留并行实现。合并规则核对 Schedule 直属 Concat 的两个输入及输出，INT64/INT32/FLOAT 向量或矩阵沿轴 0 拼接，尾部维度保持来源值。节点 batch_merge、动作及来源模板报告均明确左右人数；模板选择同时核对过滤和合并。已知成员关系不同属于变换规则缺口：分别报告 batch_filter_sequence_rewrite_not_implemented、batch_filter_membership_transform_not_implemented 或 batch_merge_branch_rewrite_not_implemented，不把尚未实现的改写归类为缺观测。

现有资产核对两 rank 共 14 个 Concat，来源左侧为运行请求、右侧为新 prefill 请求，当前两侧各一行；目标四个批次动作保持同一构造关系。旧过滤/模型/attention 布局与覆盖值不变，移除新增 batch_merge 描述后整个物理图相同，执行摘要和缺口不变。证据在 data/redesign/s3-batch-merge/merge_audit.json，未补采。该进展不证明新人数下的 kernel 拓扑、合并与不合并间的结构改写、CPU/拷贝参数或缺少完整过滤记录的其他调度路径已合格，这是当时资产的限制；当前资格以模板规则与主计划为准。

### FFTS 复合任务的直接依赖观测

复合任务的 Subtask Id 只用于关联，不作为串行顺序证明。当前安装的 CANN 9.0.0 SDK 在 runtime/rt_ffts_plus.h 中提供 rtFftsPlusTaskLaunch/WithFlag，以及主机或设备描述地址类型；runtime/rt_ffts_plus_define.h 的公共 context 包含 predecessor 计数和 successorList。当前安装的 libhccl_legacy.so 实际通过 rtGeneralCtrl 的 RT_GNL_CTRL_TYPE_FFTS_PLUS 命令提交 [descriptor, stream]（由 SDK 枚举与实际调用指令交叉核对）。原生 observer 在这一入口调用前复制主机描述中的依赖字段，调用成功后记录 SDK 返回的 task/stream 身份，规范化原样保留该提交事实。其他 general-control 命令原样转发。探针不搬运设备描述，不改参数或插入同步。

当前只解析共享公共布局的 SDMA、Notify Wait/Record、Write Value 及无条件 LABEL context，保存 context ID、类型、前驱计数、thread 信息及后继列表。设备描述、其他类型和不完整范围记录明确缺口。自动线程或软件条件 context 的执行关系不能直接套用单实例后继规则。真实主线已观测到 FFTS 提交。AscendSourceExtractor 按唯一 rank/stream/task 匹配提交，再按 context ID 核对物理子任务及类型；单线程、前驱计数、后继范围、ready 根数均吻合后，才建立 native_task_dependency 边，边保留提交 API 索引作为依据。已接入无条件 LABEL 的依赖投影：检查完整 context DAG 的前驱计数、ready 根数及无环性，将每个物理节点到下一个物理节点之间的 LABEL 路径压缩为依赖边。分支保留多个后继，汇合保留全部前驱；LABEL 作为原生描述中的逻辑关系，不伪造物理服务节点或测得耗时。该路径已通过真实补采的逐边身份审阅、模板交接及给定耗时动态展开；独立目标的 FFTS 描述对照已覆盖 4680 次任务，context 类型、前后继、LABEL 和线程模式无缺失、额外或归属缺口；该结果核对所选模板的任务描述，不等同于完整目标图依赖已验收。其他类型或缺失的物理事件继续保留缺口。源图仍统一检查环，模板消费者保留该关系，不按 context 数字顺序或观测时间连边。


### 跨时钟矛盾的定位边界

host 同步等待关系来自提交身份和同步语义；不能用设备结束时间早于 host 返回这一时间条件替代因果证据。诊断需要区分原生 wrapper、厂商 CANN 和设备时钟坐标；只更换 wrapper 边界不能证明时钟一致。历史冲突及因果路径证据见本章同步资格说明，不混用不同来源的计数。

安装的 CANN 解析器分别使用 host/device 的计数器起点、频率及采集时间偏移导出 epoch 时间；这些时间字段来自厂商换算，不等于已验证同一物理时钟。缺口报告同时提供原生等待 API、profiler_wait_interval 和 profiler_observed_lateness_ns：后者为同一设备结束相对 CANN 同步结束的差值，允许为负，用于区分两种边界口径，不能作为补偿量。现有记录保留原值，后续须核对原始换算与完成边界口径；不从成功同步约束拟合偏移，不把最大矛盾值当校准量。该诊断既不删除已有提交依赖，也不证明全图结构或成本已经合格。具体证据见执行记录与 clock_boundary_audit.json。


结构资格与时间测量资格分开：当前read_host_waits以同CPU profiler域的提交区间选同步前沿，设备/host时间差仅产生诊断；SourceGraph.seal保留因果边并报告逆序，不据此删除边。已证实因果关系且不依赖跨域时间选边的部分，可继续给定耗时下的结构验收。源时长、跨域延迟与成本校准仍未合格；影响身份关联、资源次序或边界选择的时钟问题不能这样推迟。这里没有修改原始时间或放宽未解析依赖。

### 原生调用的时钟采样顺序

Python Observation 与原生 ApiObservation 保留配对 wall/monotonic 起点及 monotonic 终点，用于归属、因果切口和实际传输边界；不再读取或转存没有成本消费者的线程 CPU 时间。元数据查询放在原生 API 观测窗口外，减少探针操作对边界的影响；这不消除原生计时中的观测扰动或跨时钟适用限制。

原生时钟区间与 profiler marker 的生命周期不同：marker 在上述观测前进入、观测后退出。阶段身份靠 call ID，不能把两者起止差值视为可减掉的固定成本。CANN 同步记录也不能未经实现依据就当作原生 rt wrapper 的严格外包络；必须分别记录并核对。

### Schedule 成员来源与原生分支边界

SGLangTemplateBindings.bind_membership 在生产时将原生 before/after 请求身份转换为公共 BatchFilter，并按已声明的 non-overlap decode 路径解释两次过滤之间的 BatchMerge。TemplateBank 不再解释过滤调用顺序或请求后缀；消费者使用公共成员关系。上述推导只解释缺少显式 merge 记录时的历史资产，不替代显式 wrapper 的完整合同。

原生 ScheduleBatch.filter_batch 的空选择仅清空 reqs 并保留旧 tensor；全保留直接返回；部分保留才创建索引并执行逐字段过滤。空/全保留都不能用尺寸为零/不变的 Index 节点代替原生结构。seq_lens_cpu、input_ids 等字段是否存在也影响工作集合。merge_batch 先处理 sampling_info，再连接批次字段；input_ids 只有两侧都存在才连接，CPU 长度镜像也有两侧存在条件。

两条路径内部都会调用 SamplingBatchInfo 及 penalizer。它们的 Index/Concat 可能与批次字段同名、同形状，所以只新增 filter/merge 外层 scope 仍不足以证明字段归属。后续统一观测至少需记录调用身份、父 Schedule、过滤前后成员/合并左右成员、可选字段存在性和 sampling 子调用边界，生产端再建立直属操作关系。不得按 kernel 顺序或时间距离分配字段身份，不将现有唯一有效 filter 限制悄悄放宽。复用公共 wrapper 生命周期，原生身份与成员关联留在生产端。

原生依据位于 sglang/srt/managers/schedule_batch.py 的 filter_batch/merge_batch，以及 sglang/srt/sampling/sampling_batch_info.py 的同名方法。显式调用来源、分支替换和全图目标资格分别核对；具体已支持范围见主计划，不从单条参数关系推导整体 Schedule 泛化。

### 准入策略的来源事实

Schedule 的核心 BlockDefinition 同时声明 visible_requests 和 admission_policy，公共 wrapper 只保留声明的参数。观测记录 worker 有效准入参数和入口比例，沿 SourceCall → 模板 source_state 交接；字段及范围见[准入策略与内存压力](sglang_template_relations.md#准入策略与内存压力)。这些字段解释来源决策，目标比例与请求状态仍由控制器维护。旧资产的缺失不能用父进程环境或当前默认值补齐，warmup 后入口值也不能误作初始比例。目标消费与动态控制的实际验证范围见主计划。

## 4. 从采集到模板

1. 规范化 CPU/NPU/通信等事件，构建保持原始事实的 source DAG。
2. 按步骤身份和异步关联提取语义子图，标明每个有成本节点的所有者。
3. 将跨子图边转换为入口/出口端口，保留中途提交、设备完成和发布等可见边界。
4. 标记参数来源：模型固定维度、用户输入、批次聚合、缓存状态、实现布局。
5. 将具体子图整理为模板：内部节点/依赖、端口、可变字段映射、适用条件、允许变换和范围；性能单元独立绑定，不由语义切分强制决定粒度。
6. 记录未覆盖步骤、未关联事件和范围外时间，不能以“成功解析 trace”代替模板完整。

嵌套步骤可以保留语义层次，但同一物理工作只由一个成本单元计费。多个真实调用是否复用同一模板依据实现、边界和参数化结构判断，不仅比较名称或节点数量。

初期参数映射由框架适配明确定义，利用真实观测核对。自动污点传播和广泛的跨模型模板发现可在有收益时扩展，不作为首轮前提。

### 4.1 从源实例到可展开模板

需要区分三个层次：block 类型规定流程职责；采集实例保存某次调用的真实子图；模板变体规定可以复用的结构及其适用条件。采集实例数量不等于提取算法或模板变体数量。TemplateBank 按 step/phase/rank 组织实例，再按请求 token 条件、发布分支、过滤状态和同批来源选择。局部模型结构可以共享，serving 实例仍保留各自的来源与绑定；是否共享不改变目标参数及接口的资格要求，也不要求把所有来源强行归并。资产规模和验收证据只在[实施计划](../redesign-workflow/2026-10-08_redesign_execution_plan.md)及其执行记录维护。

#### 模板族、变体和参数关系的维护清单

模板族按同一语义职责与参数合同理解，真实执行分支才形成变体；它不是step/phase/rank分桶，也不是每次原生调用。下表整理现有生产绑定和消费者，不新增核心枚举或另一套支持判断。所有行最终都须通过节点参数、资源、端口及目标状态的联合资格。

| 语义职责 | 需要区分的真实变体 | 目标参数与现有实现落点 |
| --- | --- | --- |
| Schedule状态迁移 | pending Prefill接续、running过滤、非空批合并、空批 | transition/filter/merge区域；保留成员、左右成员及入口准入状态由BatchParameterLayout等消费，不能仅按人数选择 |
| 准入与请求初始化 | 零接纳早退、成功接纳循环、队列/容量/接纳后输入预算停止 | AdmissionWork记录候选、horizon、队列及统计工作；成员区域重组。内存拒绝与回退不因已有成功分支而支持 |
| Prepare输入与KV | Prefill扩展、Decode新增页或页内追加；页表布局分支 | KvParameterLayout绑定请求、token、空闲/新增页；BatchOperationVariant区分页表连续、单列和带步长物化 |
| Prepare局部操作 | relay写入、请求表写入、last-location Cat、页表除法、logits reshape | BatchOperationVariant按已绑定人数/布局条件选供体；BatchParameterLayout重绑行轴与操作数。原生分支条件留在生产绑定，不按请求长度枚举模板 |
| Prefill/Decode模型主体 | 两种phase；原生QKV准备及attention实现路径 | ModelParameterRule、AttentionParameterRule消费token/context、shape/layout与模型固定量；decoder层是语义容器，不能与内部物理工作重复计数 |
| 模型区域复用 | attention与feed_forward中实际不同的局部结构、资源和接口 | TemplateGraph共享严格相等的局部定义；attention_core嵌在attention内，不再重复持有节点。相同模块名不保证同结构 |
| Publish及KV释放 | 无/部分/全部完成，Prefill/Decode发布，空/单/多成员释放组 | PublicationParameterLayout与KvParameterLayout消费完成成员、释放页及输出工作；Published端口不等于所有后台清理完成 |
| 通信、复制与workspace | 原生通信算法/任务、传输方向/布局、workspace分配或复用 | 独立参数与资源绑定保留真实依赖；workspace由目标状态推进。改变TP、后端或通信实现不由这张表授予资格 |

源码落点为生产端`adapters/frameworks/sglang/template_bindings.cpp`及runtime绑定，共同消费位于`templates`与`execution`。表中变体是规则维护单元，不是新建独立类或全部物理变体的数量承诺。

数量口径分别是：来源manifest；流程调用实例；内部区域调用；共享局部结构定义；参数规则定义及其观测引用。`TemplateBank::describe`已有这些独立字段，当前不输出一个无法定义等价关系的“总模板族数”。共享定义保留来源映射；未共享的serving区域仍按来源实例保留，不能把区域共享说成整批模板已经完全去重。当前资产实测与剩余验收只在执行计划/日志维护。

模板族归并必须保留节点角色、内部依赖、端口、跨模板连接及参数来源。只有这些结构在明确的身份重绑定和有限参数规则下等价，才能共享定义；相同 block 名、节点数或输入 shape 均不足以证明等价。采集调用、源 span 及实测数据的来源仍需保留，不能因共享结构而丢失成本依据。首先明确当前实例的有效选择条件和结构差异，再决定有实际收益的归并；不预建通用图同构框架，不把逐个 decode 长度建立模板作为泛化方案。

共享结构的CPU路径比较不能简单去掉关系标签。若两执行端点之间的额外路径只经过同一CPU线程上的注释区间，且已有不穿过其他执行端点的纯thread_order路径，则该额外路径不增加执行顺序约束，可在结构比较中归一化。原始注释及来源边继续保存；这一规则不授权删除CPU服务、等待、跨区域端口或父层服务extent，也不证明CPU工作量或成本相同。MLP逐路径的历史依据见[架构审查](../redesign-workflow/history/2026-10-10_architecture_review.md#mlp重复区域复用核对)，当前支持范围与阶段资格以实施计划为准。

共享定义与来源绑定必须一起拆分。TemplateBank 按 source_batch 联合选择同批各阶段和各 rank；TemplateAssembly 根据 source_boundary、remote_source_calls 和源 CPU lane 顺序连接端点。不能把多个 TemplateDefinition 直接换成一个代表实例。可共享的是局部节点角色、依赖、参数关系和端口定义；每次观测保留局部槽位到源 call/span/boundary、资源及同批关联的映射，目标装配和成本追溯读取所选来源。

当前代码已分开局部结构与节点/边界观测：TemplateNode 保存类型、局部端点和 observation_slot，TemplateNodeObservation 独立保存源 span/scope 及 kernel、通信、传输描述的非拥有引用；TemplateBoundary 只含局部 lane，TemplateBoundaryObservation 保存源编号及 CPU 观测时间。插入节点通过供体定义及固定的供体槽位只读访问同一来源绑定，不复制绑定，也不通过接收方槽位查来源；目标描述符覆盖独立持有；插入边界只保留来源编号，不复制源时间。内部依赖只有局部关系和端点，原始边由源图及 internal_edges 保存；跨模板诊断所需来源编号保留。

局部节点和内部依赖已进入同一共享表示：SGLang生产端选择同层互不重叠的attention与feed_forward区域，嵌套attention_core随attention主体保留，不重复声明所有权；TemplateGraph按资源内记录顺序编号，将节点类型、端点别名、局部观测槽及带关系类型的内部边组成严格相等的定义，统一存放于TemplateSource。

共享定义同时保存资源域、局部资源槽及端点到槽的映射，区分同一资源与不同资源，不以原生线程号/流号作为结构身份。每次区域调用保留端点映射和资源槽到实际lane的绑定；节点和边恢复原端点与原索引，边界通过共享资源槽恢复所选调用的lane，节点同时恢复来源观测槽。重复节点、内部边及唯一归属的边界记录随之释放；归属不唯一的端点继续独立保存。

资源槽包含必要的逻辑完成域，但不因此创建服务成本或额外同步；这不等于CPU主线程/worker等职责已获得跨配置重绑定资格。读取返回值，不能跨循环保存临时节点或边的地址。未共享记录仍独立保存，核心不按框架或模块名称分支。

内部边的归属要求两端属于同一区域；物理任务汇合点只有在所有贡献者均属该区域、没有外部输入时才纳入。外部服务的extent、跨区域边、跨rank通知和serving完成导出不因端点别名而转为区域内部工作，继续按来源关系绑定。CPU注释仍逐项保留，不因前述投影等价证明而在此处删除。

模型计算的参数规则与来源注释分开：ModelParameterRule按角色、张量轴、工作量种类及部分操作数标记等类型化字段共享；来源kernel描述和原生注释仍逐次保留。共享区域按局部节点槽引用规则，目标求值从图中取得规则，再结合所选来源描述与目标BatchInput计算参数。来源名称和注释不参与规则等价判断。

通信参数也与scope分离：CollectiveParameterRule包含可选的计数规则以及已资格化的CommunicationWorkLayout或原有缺口。计数按token/request行数乘宽度计算；字节规则保留原有效域、任务索引、目的rank分片、对齐及错误原因，按类型化字段共享。共享区域通过collective包络节点槽引用规则，目标计数、字节求值及联合rank替换读取同一规则；原生任务和scope仍从来源观测读取。分片规则读取已验证任务的destination_rank：生产端已保证数据任务的目的rank为本rank或通信对端，等价于既有本地/对端分片选择，不把绝对rank编号固化为另一份规则。该共享不改变通信算法或给通信任务赋予耗时。

局部资源和外部接口的来源关系已共享，但跨目标资源/端口角色资格和其余注意力/KV、批次及传输参数合同尚未统一完成，源采集文件也未改成共享资产格式。region_structure_sharing只统计已纳入共享表示的局部图；model_parameter_rules分别报告全来源的规则定义数与观测绑定数，不是完整模板族数，也不授予新的目标配置支持域。

重复层仍需重绑定层位置、KV 身份及外部消费者。MLP 的线程/设备流前后序、同层跨 rank 通知和 serving 完成汇合已有来源证据；普通层与末层的消费者差异属于调用位置绑定，不据此复制内部定义。跨界父层 CPU 控制/提交片段不归 MLP 所有，其边界与依赖仍保留，目标不追加这部分 CPU 服务费用。共享拓扑与参数规则分别表达，调用位置重绑定仍须满足端口及资源合同；不能由一个共享定义推导任意层位置或配置迁移已支持。历史依据见[整体审查](../redesign-workflow/history/2026-10-10_architecture_review.md#mlp重复区域复用核对)，阶段验收由实施计划维护。

共享区域的装配接口与有限区域替换的前提不同。现有 TemplateRegionPort 表达已资格化替换所需的资源入口/出口，并非所有模型区域的完整接口；region_ports 会拒绝一般跨 rank 通知和完成导出。MLP 共享定义必须分别保留局部执行端点、线程/设备流前后序、跨 rank 通知槽、所属 serving 的完成导出以及不归区域所有的父层服务边界。来源实例将这些槽绑定到实际层位置、rank/cohort 和外部消费者，目标装配保留各自的关系类型与生效端点。不能放宽现有替换检查、跳过通知或增加统一结束屏障来让共享区域通过；通信分支替换继续使用已有联合 rank 规划，不能按 rank 独立提交。详见[实际消费接口审查](../redesign-workflow/history/2026-10-10_architecture_review.md#共享区域与替换接口的边界)。

共享接口的合同以局部端点槽、方向、关系类型、外部端点槽及资源域表达；外部端点槽保留“多条关系指向同一端点”的别名约束，资源绑定同样保留与局部资源相同或不同、同rank或跨rank的关系。实际远端boundary、call、层位置和原生lane属于调用绑定，不进入共享定义。不能只比较端口数量，也不能将所有完成出口合成一个无来源屏障。现有MLP资产按上述关系可归为与局部结构一一对应的13种接口形式；这是来源等价证据；接口存储和读取已接入共享图，目标角色匹配仍须独立验收，详见架构审查。

TemplateInterface统一表达同serving切口与跨定义连接：关系、方向、局部端点和远端槽位组成共享定义，两类接口共用远端表。远端槽位保留端点别名、与本地区域端点的重合、资源域/资源别名及rank别名。rank槽0表示本定义rank，其余对端按首次出现编号，不将原生rank、线程号或流号写入共享结构身份。

调用绑定保留实际远端source boundary，以及该端点在本serving中的局部索引（若存在）。跨定义的对端可能没有本地lane，不能为它伪造本地资源。TemplateGraph通过同一远端表恢复依赖与跨定义连接；TemplateConnectionObservation只保留原生边、远端调用归属、资源前沿及顺序，不再重复保存远端编号。目标实例仍独立持有可修改连接。

两侧区域都可以保存同serving切口，但原始依赖槽只选择其中一侧解析，正式遍历仍产生一条边；两端同属区域而由外部服务持有的extent继续独立保存。该读取路径也供既有区域替换使用，不增加第二套装配器。以上实现共享来源的结构与绑定，不自动授予新的目标角色映射或扩大配置支持域。

接口描述不授予工作所有权。区域切口可能来自同一serving定义内的依赖，也可能来自跨定义的TemplateConnection，两者需要共同描述；跨区域依赖允许两侧接口各自引用，但正式执行只保留一个关系实例。父层CPU服务extent继续由父层持有；线程/流前后序、跨rank通知和所属serving完成出口分别绑定，不按统一入口/出口替换。新增共享表示应通过既有依赖和连接读取路径消费，不能只新增诊断表而保留另一套独立装配机制。

workspace需求同样将规则定义与观测分开：按units、bytes_per_unit、alignment、完整partition及有序buffer参数的值等价共享，包含原生float32参数，不按当前求值结果相同归并。pool、操作/worker、来源API/marker、边界、原始容量和分支继续逐使用点保存。共享局限于来源内规则，不合并物理工作、不授予新参数域，也不是完整模板族归并。 需求绑定保存规则或具体缺口原因，原因从运行时适配器经编译资产传递到执行核及来源报告；未知ATB实现、缺设备几何、未覆盖操作/参数和观测需求不符不会退化成同一个空值。release不需要需求规则。

attention 参数同样分离规则与来源：`AttentionParameterRule` 按完整张量轴关系、操作和缓存几何参数共享，`AttentionParameterObservation` 保存规则引用及每次原生调用的注释。目标 request 的序列、slot 和 KV 页在实例求值时生成，不进入共享规则；不能按一次求值后的 shape 相同合并规则。共享规则由 `TemplateSource` 持有，观测只借用只读引用，来源跨度与区域身份不合并。此归并不扩大 attention 的拓扑或配置支持域。

#### 多来源组合的身份与资源边界

TemplateBank持有一个首选来源，并借用其他合格bank的不可变定义；PredictionWorkflow持有全部bank直到组合运行结束。首选来源不是整张目标图必须唯一使用的来源。共同来源资格与初始化资格分开：首选来源必须满足统一预热及目标容量条件；后续整批候选要求workspace绑定完整，按共同条件、整组接口与实际目标状态消费，局部供体另按区域合同消费，均不导入它的初始容量。缺预热记录的历史来源不能单独启动仿真，也不补造初始化字段。整批及带状态的共同候选要求模型、原生配置、部署、rank数、页大小、序列容量、语义解析方式和准入策略一致。另设受限的操作片段输入：在适配器已允许的准入配置变化内，仅消费生产端声明可按目标请求池重新绑定的操作。LastLocationConcat、选中批次页表的 PageTableDivide 和 logits 的 LogitsReshape 不依赖池总容量；RelayWrite、RequestTableWrite 则要求全部设备成员已有目标容量、批次轴和槽位的参数关系。局部写入消费目标池，不导入来源池内容；不据此放行该来源的整份模板、初始状态或任意其他操作。

- **完整批次与局部替换**：每个目标批次先选一个来源内的原生batch，联合验证全部阶段和rank，再提交装配；后续批次可以换来源。局部操作/区域从既有语义分组的候选中选择，组内本来源优先，继续执行参数、端口、分支及拓扑资格检查。整批缺失与变换规则缺失保持不同结果，不以同形状或资产可加载代替覆盖。
- **设备完成端口**：从区域内部因果关系确定设备资源的唯一末端工作，不以来源是否存在外部 host wait 决定是否暴露完成点。已有完成导出必须对应此末端；实际完成点与流出口不同，只接受唯一前驱的 CompletionJoin。供体末端没有流出口别名时，复用目标已资格化的别名及零工作连接，不添加服务节点。反向别名合并仍明确拒绝。
- **片段适用条件**：`BatchOperationVariant.request_pool_rebindable` 随编译模板保存，默认不声明；适配器只接入自身来源资格成立且配置差异在既有规则内的候选。`operation_sources` 是工作流所持来源定义的只读视图；核心只在对应阶段/rank的批次操作替换中消费，并再次要求实际所选操作带有该声明。后续端口、依赖、资源角色与成本查询沿用原路径。这些来源不进入整批、KV、workspace或初始化供体集合。
- **来源身份**：节点、边界、调用及耗时用manifest加局部编号限定；Assembly的边界匹配与CPU顺序投影也按来源限定，不拼接不同采集的时间戳。插入工作保留实际donor来源，目标端点独立创建，不能因复用同一donor而共享执行身份。
- **资源角色**：TemplateResource保留原生编号与可选角色，有角色时按domain/rank/role匹配。lane、外部连接、等待、区域替换和资源前沿共用该身份；未赋角色的资源只能保留来源内身份。跨来源局部替换要求参与定义的资源角色完整，整批来源切换还要求首选来源与候选来源全部定义的资源角色完整。这是当前保守支持域，不声称未参与目标的未知资源必然影响执行。任务完成边界不合并为执行资源。
- **状态与成本**：请求、KV、控制状态和模拟时钟保持唯一，来源切换只改变所实例化的工作。服务节点按实际来源查询给定耗时；缺值报告该manifest与span，不借用另一资产的同号值。来源图、原生编号及原始资源别名保留，目标执行不读取原生框架对象。

SGLangResourceBindings从serving入口绑定control，从原生CPU flow绑定submission，从有归属的计算和CPU提交关系绑定model，从collective submission与双向原生event依赖绑定collective；候选不唯一或角色冲突时不赋角色。不按线程编号、kernel名称统计或出现顺序配对。当前codec保存角色，来源报告resource_bindings列出结果。

公开组合证据为 `data/redesign/s4-multisource-donors/composition_verification.json`：已验证局部外部服务节点及跨批次来源切换，逐批同源、来源成本约束和依赖时间成立；原有两配置完整图回归不变。这里只证明记录范围内的条件化结构，不是任意批人数、任意原生配置共享、独立原生结构验收或真实成本精度。详细来源取证与运行索引集中在执行日志。

#### 编译资产与预测加载

提取阶段完成原生绑定后，将 TemplateSource 写为 compiled_templates.cbor，manifest 通过 compiled_templates 引用该资产。纯预测读取已编译的结构、规则、来源绑定及支持域，不读取原始 source_graph/实例 templates，也不再次调用框架模板生产器；原始事实与原生解释留在生产和取证路径。CBOR 使用现有 JSON 库编解码，不增加依赖或内部版本。图/实例 JSON 是生产阶段的派生物；已有文件满足合同且只改变绑定时可复用，清理后须能从保留事实重新生成。实际命令、已接入的清理/重建行为及其尚未验证的生命周期边界只见[运行说明](development.md#5-已有-base-的-source-提取)，不把局部代码存在当作完整验收。

生产器仅接收采集manifest，模型、配置、部署和资源容量均来自来源记录，不依赖待预测的Project。统一资产保留模型与规范化配置用于身份核对，配置内容不交给执行核解释；预测入口负责检查来源是否匹配声明的输入，生产端负责原生事实及支持域解释。

资产须保留共享定义与调用实例的区分、端点/资源/rank别名、来源节点身份、真实描述符、原有缺口及适用域。内存中的非拥有指针编码为所属表的局部索引，加载时先恢复拥有者再建立引用；不能序列化地址，也不能把整份原始图包进新文件冒充编译资产。给定节点耗时继续按来源身份匹配，来源CPU时间仅用于已有关系解释，不成为节点成本。

`TemplateSource.workspace_rules` 与 `attention_rules` 按完整规则值在来源内共享；使用点/定义保存只读引用，`AssetReferences` 编码为表索引，加载先恢复规则所有者再恢复引用。workspace 报告的 `demand_rules` 统计已绑定使用点，`demand_rule_definitions` 统计独立定义；attention 报告同样区分 definitions 与 observations。角色绑定、`filter_regions`、序列容量及这些规则表变化属于编译资产合同变化，应从完整来源重建，不保留旧格式默认值或兼容分支。

验收同时检查行为与隔离：使用同一真实资产编译一次，在预测容器不挂载source_graph、原始实例文件和vendor trace的条件下完成正式展开；与编译前逐项对照节点描述、依赖、端口、状态和来源，并报告资产大小、编译/加载开销及定义规模。缺编译资产明确返回缺口，不能回退到原始图或隐式补采；旧本地资产通过明确的重提取迁移，不维护双格式、版本协商、校验指纹或另一套执行器。当前实现与隔离验证证据见[架构审查](../redesign-workflow/history/2026-10-10_architecture_review.md#编译资产与原始图依赖审查)，不能仅凭内存去重或容器启动成功验收。

模板保留阶段语义、原生来源、内部工作/资源属性、跨图端口及状态效果的对应；源事实保持不变。实例化时生成新调用/节点身份，并替换请求、batch、rank 映射。指针和源绝对时间仅保留取证来源，不成为目标输入或调度规则。

参数关系需要明确来源，例如输入 token 总量到节点 shape、已发布进度到 context、固定模型/TP 到布局。不能只在模块参数上写新 batch/context，却保留旧物理节点并宣称已经变换。缺少必要 shape、依赖或布局映射时返回缺口。

只为当前规则会修改的内部工作补足语义角色和参数关系，其他节点按真实结构保留；不要求逐 kernel 手写插桩或一次建立全算子语义库。底层名称可作来源信息，不能单靠名称猜测可变维度或实现分支。

模板复用按实现、内部结构、端口和有效参数域判断。遇到 shape 引起原生实现分支变化，选择已有且适用的真实变体，或应用已声明的结构规则；未观测变体不自动生成。单次 base 不能证明所有目标分支覆盖，主线采集需要覆盖声明变化实际所需的 prefill/decode 和 batch 变体。

模板可以粗到一次 forward，内部仍保存算子、通信、拷贝和必要 CPU 工作及关键依赖。当前给定耗时只作用于范围内物理服务节点，CPU 控制片段保留结构但不查询耗时；成本模型粒度与语义区域粒度分开。语义边界不预先决定成本模型粒度。

### 4.2 参数化及内部语义锚点

模板参数区分模型固定量、配置相关量、request 变量与运行状态。记录来源到节点 shape/layout、工作次数、分支和端口的映射；先落实主线需要的显式映射，再按维护收益采用维度来源传播，不预建全自动污点框架。用不同代表输入核对变化规律和未覆盖分支，不仅凭名称或维度数值推断来源。

五类顶层 block 内部提供必要的 attention、KV、通信等语义锚点，支持局部替换和复用；不要求每个算子单独插桩。kernel 因 shape 或 autotuner 改变时，性能签名可复用不等于物理 DAG 等价。实例数量是来源证据，不能代替结构族及变体划分，也不以枚举decode长度替代参数化。

Prefill/Decode 内部采用“模型结构 → 原生模块/调用绑定 → 实际执行子图”的语义映射。先读取当前模型及框架实现中的层/子模块组织，再为确有复用或变换需要的区域设置 wrapper/hook；记录模型模块路径、层实例、phase 与调用身份，并沿既有提交身份传播到执行节点。避免只凭 kernel 名称反推模型结构，也不为每个模型层复制提取算法。

模型结构不是物理 DAG 的一对一划分：算子融合可能跨语义边界，一个模块也可能展开多个 kernel 或异步工作。遇到融合跨界，保留共享/组合区域及唯一物理归属，不虚构切开的 kernel 或按比例拆耗时；通信和 KV 等执行行为按实际实现附着到模型语义区域。Prefill/Decode 共享可复用的模型结构，phase 相关路径分别保留适用变体。性能单元仍按独立测量与外部依赖确定，不强制跟随模型模块边界。

源图通过 stream_waits 单列原生同步对象、HostWait span 与设备 lane 映射，避免把“源图无入边”误当成等待语义为空。模板消费者必须读取该记录；已有保留 scope 的事实可重新 extract，历史源图不提供兼容推断。等待模板记录等待对象与作用范围。例如“等待某 stream 中此前提交的工作”是语义；采集时 stream 为空不代表未来调用也为空。实例化时根据目标提交前沿接边，而非只复制源图碰巧存在的边。内部语义、资源绑定和外部端口共同交给 C++，Python 不生成另一套目标图。当前non-overlap实现同时纳入此前批次和同批前序调用的已提交工作；当前调用内部的等待保持区域依赖，不能连接该调用尚未发生的工作。同批连接必须同步处理两端，具体合同见[等待与资源](sglang_template_relations.md#等待与资源)。

同步资格需区分提交/等待的因果证据与 CPU/设备时间戳的一致性。2026-10-11 的 run-20261011-053447-612772 历史来源中，156 项 native_host_wait_clock_order_disagrees 涉及 62 次同步，全部设备完成端点都可经设备区间、stream 顺序、任务/event/notify 依赖及完成汇合，到达该 API 对应的 native_host_wait 边和 HostWait 结束；验证不依赖 CPU thread_order 或 native_flow 绕行。证据 s3-host-wait-audit/synchronization_paths.json。历史资产的同类核对见执行记录。

成功同步的接口合同是等待指定流中的任务完成。当前已安装 CANN 9.0 头文件给出该语义；[官方 FAST_SYNC 说明](https://www.hiascend.com/doc_center/source/zh/CANNCommunityEdition/910beta2/API/runtimeapi/aclcppdevg_03_0069.html)说明主动查询替代被动等待，但该公开页版本为 9.1 beta2，不作为历史运行证据。当前固定源资产的两个原始 profiler_info 均记录 CANN 9.0.0、torch_npu 2.10.0，并保留起止时钟锚点（s3-host-wait-audit/historical_clock_metadata.json）；此前仅检查环境 JSON 而称历史版本缺失的判断已纠正。接口合同及版本记录仍不能单独证明实际驱动执行或时钟准确。

成功的事件等待不能仅凭 rtGetTaskIdAndStreamID 返回值认定存在一个新硬件任务。当前 ACL_EVENT_SYNC 诊断中，事件完成后等待返回身份不变；这只限定任务身份关联规则，不授权忽略等待语义或猜测历史完成端点。若缺硬件等待成员，提取器按唯一且无并发改写的 Event Record 代次、同一 CPU 线程上的消费流提交顺序和已资格化任务链，将 Record 完成连接到后续工作前沿。复合任务保留所有独立根，由原生内部依赖覆盖后继；不新增硬件节点或虚构服务耗时。关联歧义、多线程提交、调用重叠或提交/设备顺序矛盾仍报缺口。具体运行与切口范围见整体审查的来源条目。

以上只排除“这 156 项冲突等于缺少等待路径”，不证明完整提交前沿、所有关联或时钟校准。保留原始缺口及时间戳，不以阈值、拟合、删边或默认完成状态消除矛盾；给定耗时的结构执行与真实时间资格分别验收。不为已存在的因果路径重复采集相同 workload，剩余完成语义和校准需求必须明确所缺证据。依据与限制见 s3-host-wait-audit/synchronization_semantics.json。


验收使用未参与建库的 request 序列，覆盖合法新长度、到达和批次组合，检查工作量、shape、关键依赖、状态与终止，并验证范围外拒绝。短 trace 发现算子集合不自动证明 serving 控制分支或 CPU/设备图已覆盖。

当前实现先保留已有 model_forward 的区域身份：归一化沿原生 queue/flow/task 关联传播最内层已观测 scope，再同时输出 scope_call 与所属 serving owner_call。C++ 源图保留两种身份，BlockExtractor 从同一图中识别区域成员、内部边与切边边界；成员归属保存在源图 scope_call 中，区域记录只保存身份、CPU 范围、切边和数量，不重复保存整套节点/边索引；TemplateDefinition 将成员和切边映射为所属模板的本地节点/边界索引，预测诊断通过 semantic_regions 展示。区域引用既有工作，不新增服务节点、区域总成本或隐式完成屏障。cpu_scope 的 native_returned 只表示原生 CPU 返回。

此交接已扩展到下述模型层、attention、MLP 等绑定；部分内部矩阵形状关系已经接入，完整参数化规则仍待完成。源图派生的 CPU 控制片段尚未细分到这些区域，缺少 scope 的工作也不会按时间就近归入。因此区域只是已观测子图视图，不能据此宣称完整模型语义或任意 shape 变换已支持。旧 source_facts 需要从原始采集重新归一化再 extract；不能仅对已经丢失 scope 的旧事实重提取来恢复区域身份，不增加兼容推断。

### 模型区域的绑定与嵌套子图视图

当前代码通过核心 SGLangBindings 声明 model_region 的字段、模块类型、方法、张量输入及整数属性来源，prepare 以 native_regions 交给 Python。十项模型绑定覆盖模型主体、embedding、decoder layer、attention、native/NPU 两条 QKV preparation、radix attention、MLP 、logits 和 logits selection。它们是共享 wrapper 的绑定项，不是新增 serving block，也不等于同等数量的已资格化的模板结构。

NativeRegions 遍历本次实际模型对象的 named_modules，保留模块路径和层实例，按绑定安装 NativeRegionWrapper。原始绑定方法由 wrapper 持有且只执行一次；仅在 Recorder 的活动观测范围内记录，未跟踪调用直接转发。安装生命周期为原生 worker 持有该模型的期间；不修改官方源码，不在模拟阶段加载原生模型。输入只读取 shape、stride、dtype、device，维度从绑定声明的原生属性取得，不复制激活或同步设备。

规范化保留最内层 scope_call，再沿显式父链取得 serving owner_call。C++ 区域保留 parent_call、serving_call、绑定和模块参数，成员及切边映射为所属服务模板内的节点与边界索引。祖先区域包含子区域的视图，但底层物理工作只存在一份；父子区域不能分别收费，区域退出也不新增设备完成屏障。模块内未另设 wrapper 的归一化、通信及融合工作仍归父区域，不能声称已完成全部细粒度分割。

这些字段提供参数化的原生来源；目前仍是观测布局和维度，尚未生成每个内部 kernel 的目标 shape 表达式。真实观测与消费者验证见执行日志，未验收项不因绑定接通而自动完成。

历史短采集已取得模型模块的嵌套区域、Prefill/Decode 覆盖及给定耗时消费证据；调用实例数不代表去重结构族数。该观测不能授予所有目标结构资格，清空前的数据也不能作为当前可复核资产。历史结果见[架构审查](../redesign-workflow/history/2026-10-10_architecture_review.md)，现状只由主计划维护。

### 4.3 模型内部参数关系

Prefill/Decode 内部按模型实际 embedding、attention、MLP、归一化及 logits 路径解释。融合节点保持实际物理归属，参数轴由来源合同决定，不能只凭数值相等推断。Qwen3 的具体维度、已报告槽位及 partial 限制见[模型关系参考](sglang_template_relations.md#主线-qwen3-模型结构与参数化落点)。

## 5. 独立性能采集

结构资格满足后，独立采样只解决当前纳入计时的工作成本，不能修复语义或通过枚举 decode 长度取得结构资格。以下统一规定入口、对象一致性和特征覆盖，随后列出具体测量方式。

计算按实际工作描述测量，传输保留执行方式、方向、字节及介质，通信保留物理工作与拓扑，设备命令保留任务类型。测量同步可以确认完成，不能把等待再计为服务；base 直接迁移仅适用于边界和工作量相符的未变化工作。评价资产不进入训练。

### 独立性能采样合同

“独立”指性能数据的用途、测量边界和生命周期独立于结构建库，不要求每个算子脱离 serving 执行。首个预测闭环只为计算、通信、数据搬运和物理设备命令取得成本；host 控制、准备、提交及完成处理的 CPU 服务时延排除，控制状态、必要提交、等待与发布依赖继续保留。host 同步复制属于传输，不能因来源节点位于 CPU lane 就一并排除。该排除不适用于未来 KT CPU 专家的实际计算。

当前代码已按此范围收紧。`sample` 只接受 transfer、device、communication，默认 transfer；transfer/device 使用显式工作域。host CPU 服务不再生成性能对象，旧 CPU 资产不修改，也不证明新范围已完成真实闭环。实施状态见[主计划](../redesign-workflow/2026-10-08_redesign_execution_plan.md#4-阶段路线与当前交接)。

#### 采样方法的四项约束

| 约束 | 具体要求 |
| --- | --- |
| 与子图对象一致 | 样本对应模板中的明确工作，复用公共描述、单位和计费边界；相同名称或 shape 不证明成本等价 |
| 适配简洁 | 项目组织请求与观测窗口；原生请求、组批、KV、执行更新及回收由框架管理 |
| 跨框架有共同实现途径 | 依赖提交请求、原生执行和取得观测，不假定每个框架都有 SGLang batch、ModelRunner 或单批次 benchmark |
| 易构造特征范围 | 少量参数化请求覆盖可达工作域，不为每个节点手写配方或无界试运行 |

#### 统一接口与两类插桩

正式[性能需求分析](06_performance_execution.md#性能需求推导与覆盖分析)先依据计时范围筛选对象，再从模板和目标工作推导特征需求、复用数据并跨组合合并。CPU 区间可以作为结构、归属及端口证据，不自动成为成本对象、采样任务或首个闭环的缺成本门槛；其中实际执行的传输另按传输合同判断。

框架适配器把已定义需求转换为有界原生请求，报告实际批次、工作特征、测量边界、部署、来源和缺口，不维护原生状态，也不保证任意特征点都能命中。公共编排复用预算、统一预热、失败和资产生命周期；Python 运行原生采集，C++ 消费公共工作描述与样本。

采样点和重复之间的重置必须由框架确认实际空闲后完成；生成响应不证明上一批已清理。优先使用原生有界等待接口，读取真实成功/失败，不用固定sleep、忽略false、重试循环或自行维护KV/请求状态。等待仍受父attempt的剩余总预算限制；收尾清理不成为目标节点成本，也不证明全部设备异步尾部已排空。

语义插桩取得阶段归属、参数与依赖；性能插桩取得纳入计时的工作特征及服务测量。两者独立启停，共享必要的批次元数据与异步身份，性能采样不要求生成 source DAG 或安装整套模型区域插桩。结构 trace 保留原生 RecordScope；历史缺失不能按函数名猜测。队列身份仍用于原生因果与同步传输关联，不再生产 CPU 操作、成员或函数 gap 的成本样本。

#### 从请求构造到特征覆盖

模板关系表达“配置 + 实际批次状态 → 工作特征”，不等于任意特征的反向求解器。模型/部署固定量、请求动态量和需观测的运行状态分别处理，不枚举各轴的笛卡尔积。

| 需要覆盖的工作特征 | 请求构造及实际核对 |
| --- | --- |
| Prefill token 维度 | 调整请求数与长度分布；相同总 token 不保证 attention 工作相同 |
| Decode 与 attention 上下文 | 真实 prompt 建立 KV，再少量生成；按实际活跃成员及长度记录工作 |
| 请求池槽位访问 | 采样域携带与成员对应的`pool_slots`；框架可用短请求形成早期预留并自然释放，逐rank核对真实槽位，不能将数组位置当作已分配槽位 |
| 批次成员过滤 | 短请求按指定成员先后结束，由原生执行过滤；读取调用前后实际成员顺序，核对输入人数与保留索引，不用下一批人数反推 |
| 搬运及通信量 | 由真实批次与配置产生，保留方向、字节、内存类型、rank、拓扑和实现 |
| 设备命令 | 保留实际物理命令类型，CPU 发起与依赖等待不再增加一份服务费用 |

常驻模型上用短输出和少量请求点共享启动成本；批次仍由原生 scheduler 形成，不伪造页表或静默切换 backend。采样前声明特征域、配方、重复/留出及运行、处理、存储预算。覆盖以每个 rank 的实际工作和发布身份为准，配方声明、重复层与大量记录不能代替覆盖；同次运行的重复层不能随机拆作独立验证。合同不足、不可达、预算到限或无新增证据时停止，不循环补点。

过滤工作使用独立的`batch_filter`触发域，不冒充Prefill/Decode模型执行域。当前SGLang配方从真实prompt取短前缀，先让保留集合的补集完成，再让保留成员依次完成；同一原生生命周期可得到后续缩小批次，不枚举所有成员子集。性能hook只记录原方法的实际选择，不维护原生请求/KV状态；成本键仍由真实张量、索引及启动描述决定。预热按实际成员进度与选择核对，历史缺失的过滤索引不能从shape或完成数量补造。

请求池访问的采样约束不按read/write角色分开。`pool_slots`随成员一起排序、生成与比较；预热、模型执行、发布前的实际槽位保留在对应成员上，缺失仍是未知。当前SGLang在统一reset后利用原生递增预留顺序，用真实单token短请求填空位；Decode有效请求须同时提交且生成进度一致，短请求结束后仍由原生释放槽位。带空位的Prefill会改变模型批次shape，混合延迟提交会改变预留历史，均不由此配方承诺。最终成本仍按完整`RequestPoolWork`及张量描述查询，触发域覆盖不代替节点费用或独立精度。

Prefill配方在适配端统一检查已确定的准入前提：若前缀未取整prompt已耗尽输入预算，不能再加入成员；末成员本身可以越过预算，不能用整批总量限制误拒。连续`pool_slots`复用同一判断，Decode filler仍保留自己的cohort容量限制。未取整检查只是必要条件，分页及实际组批仍由原生观测核对，准备成功不代表特征覆盖。

runtime 适配按真实 API/任务身份关联传输，解释 CANN 数值方向并保留来源。同步 driver 本体与前置等待分开：复制完成时间继续计时，等待由依赖表达，不用外层 API 包络或线程 CPU 推断传输时间。性能粒度可为节点或合格局部子图，不能父子重复计费。

性能产物使用 manifest 显式引用的紧凑 JSON/gzip，保留事件字段、顺序及原始行号。trace/CSV 压缩不形成新独立样本；成功规范化后才按本次待清理状态收尾，失败与历史捕获保留。导出体积、最终保留量和临时峰值分别报告。

#### 原生入口的选择边界

现有 SGLang transfer/device 工具使用常驻 Engine 的 `generate()`/`async_generate()`，多次调用不重启模型，为范围内工作取得独立性能证据。独立通信设施按自身适用条件复用。同步 host 传输记录实际完整 driver 叶子，不恢复整套 CPU 服务建模。

serving 内部经 `Scheduler.run_batch → TpModelWorker → ForwardBatch → ModelRunner` 执行。`bench_one_batch.py` 由官方脚本构造和管理状态，`ModelRunner.forward()` 接受已准备的状态，二者都不自动满足本项目采样合同。首轮不引入需要项目管理原生状态的驱动，也不照搬 Dooly 的逐算子重放与元数据重建体系。

### 5.1 同步 host 传输采样

`sample --kind transfer` 由框架适配器记录真实批次范围，原生队列身份关联成功 enqueue/worker 与 driver 叶子；不启用 profiler，不测 CPU 阶段、成员或函数服务。`TransferMeasurements` 发布 `cpu_worker_units.json.gz` 中的 `transfer_units`，保留执行方式、方向、字节、完整成功的双 `wall_ns` 端点、时长及来源。前置设备同步等待不并入 driver 本体，driver 内部 I/O 停顿保留。

manifest 延用 `measurement_kind=native_serving_timing`、`cpu_worker_units/cpu_workers` 等载体字段，供统一 transfer 拟合读取，不再交接 CPU 服务模型。新的两参数 queue ABI 要求原生观察库与 torch_npu 队列头文件成套重编译；历史 enqueue/scope 镜像不能直接用于此采样。当前 `markov-transfer-20261016-sglang:ubuntu22.04` 已完成配套部署及真实 transfer 采样；这不授予独立留出或整次预测精度资格。部署限制见[开发说明](development.md#队列-abi-与镜像)，运行证据和阶段状态见实施计划。

### 5.2 设备与通信采样

`sample --kind device` 通过官方 profiler、步骤身份和 SDK task/stream 关联取得计算、传输、通信及命令观测；`DeviceCostNormalizer` 与结构归一化共用底层身份关联，但性能数据不生成 DAG 或模板。样本保留实际工作、部署、shape/dtype/layout、物理 span 和原始来源；collective 汇总、等待与父包络不重复计费。

`sample --kind communication` 沿独立 HCCL 入口取得明确多 rank 工作与物理任务，不加载模型权重；适用操作、dtype、拓扑和部署由真实记录限定。工具能运行、产物成功或候选可查询都不构成精度资格。

## 6. 交接、预算与错误

profile_manifest.json 交接 role=base 的模型/部署、窗口、步骤覆盖和引用文件；独立传输/设备/通信采样由 role=performance 的 performance_manifest.json 交接测量域、环境、样本及成本缺口。下游只读取明确引用，不自行扫描目录。模板由 manifest 关联的 base 事实生成，保留源引用和实际参数条件；不新增文件摘要或内部版本握手。

真实采集串行执行，工作流管理 base 与 performance 两类累计预算。启用前需有正预算；启动、预热、失败、无效采集、重试和实际停止延迟均计入。资源锁排队、原生任务结束后到恢复之间的等待不计入。恢复不重置，测量需求在候选间共享。SGLang base 与独立 transfer/device sample 共用容器 PID 1 的预算/锁，SDK 初始化也在预算内；KT 原生 base 已复用共同采集设施，其性能适配后续开展。

采集失败与局部资产明确区分；纳入计时的工作缺成本时返回成本缺口，缺少必要关联或步骤时返回语义缺口。host CPU 服务不计时不等于控制与依赖可以缺失；计算、通信、传输和设备命令的成本缺失不能被这一范围约定掩盖。

## 7. 交付合同与验收边界

采集交付动作到原生工作的映射、统一语义记录和可参数化模板。节点与关键依赖可追溯，阶段/共享工作归属明确，异步端口与参数来源完整；禁采时不启动设备，缺口不伪造。结构交付不要求成本模型先完成；KT 后续能力独立说明。

每个 block 的定义、框架 wrapper、记录与模板端口相互可追溯；公共生命周期与图装配不由子类重复实现。Python 消费核心导出的合同，目标预测不要求原生框架参与。具体实施顺序及已验证范围只在[主计划](../redesign-workflow/2026-10-08_redesign_execution_plan.md)维护，联合验收见[验收合同](08_acceptance_delivery.md)。
