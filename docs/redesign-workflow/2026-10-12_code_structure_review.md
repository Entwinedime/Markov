# 2026-10-12：代码层次、风格与逻辑整理

本文各节为对应批次的历史代码审查，当前阶段状态只见实施计划；2026-10-13 补充纠偏见文末。

本次落实用户明确的周期审查方向，检查活动代码的职责、依赖、状态所有权、可读性与重复实现；workspace 功能缺口另见同日 workspace_state_pipeline_review。本报告不把代码整理计作 S3/S4 结构验收。

## 检查范围与结论

检查了 CMake 三个库的依赖和活动目录，C++ workflow、模板/拓扑/拼接、controller/execution、给定耗时及 runtime 绑定的主要职责链；Python workflow、公共插桩、框架 wrapper 和 Ascend 产物交接。源码扫描用于定位，结论经相关方法及调用方阅读确认。本次不是每个原生规则的逐行正确性证明。

| 项目 | 发现与处理 |
| --- | --- |
| 分层 | CMake 为 workflow → adapters → core。公共 C++ 模板、控制、执行及 Python instrumentation 没有反向依赖具体适配器；运行环境未导入框架层。原生常量和 hook 留在适配端，未为此新增接口。 |
| 职责集中 | templates.cpp 混有源码模板、候选选择、拓扑 mutation 与跨批资源拼接。将已有 TemplateOperationRewrite/TemplateTopology 实现移入 template_topology.cpp；TemplateInstance/TemplateAssembly 移入 template_assembly.cpp。保留原类、签名、算法和 target，删除旧位置的实现，不创建转发层。此为定位和阅读改善，不宣称代码总量压缩。 |
| 内部类型 | NodeDurations 已在输入边界将 assumptions 校验为字符串列表，却仍转回 JSON 保存。改为 vector<string>，JSON 仅用于读取/报告边界，未改变输入合同或增加校验。 |
| 表达与风格 | 资源等待查询删除临时引用后的重复值复制，optional 资源访问用直接箭头表达；新文件补齐直接使用的标准头。Python inline 的长行分组；output wrapper 的前后成员配对明确 strict=True，避免静默截断。 |
| 异常边界 | prediction_workflow 的资产、耗时、组合错误各自隔离；Python attempt 的宽泛捕获先记录终态并清理 worker；KT worker 记录失败并通知父进程后重抛。这些有真实恢复/报告职责，未机械删除。 |
| 重复与包装 | AST 比对定位到两个 wrapper 的短构造重复；它们只组织既有 BoundBlock，没有独立生命周期，新增父类或工具方法收益不足，保留。方法引用检查发现的 comparator 和 plan 均有实际调用，不按名称计数误删。未增加通用 factory/registry。 |
| 状态 | 控制器持请求/KV业务状态，runtime 持事件时钟，TemplateAssembly 持资源前沿，ExecutionGraph 重算完成时间。此次没有复制状态。后续 workspace 应在这些职责边界内接入，不能为每一轮缺口增设并行状态账本。 |

## 验证与限制

Docker 正式 C++23 构建完成；CMake 纳入两个新实现文件。已有四请求给定耗时场景经同一公开 predict 入口重跑，组合结果与资产报告逐项相等，含结构、端口、指标、假设及缺口，证据 data/redesign/s4-workspace-growth/code_review_verification.json。它是已有开发输入回归，不算独立结构验收。

Python 全包 Ruff 通过，修改的 C++ 文件 clang-format 检查及 git diff --check 通过。核对原生 output_streamer 不修改 reqs 成员序列；既有 serving 的10条 output_work 前后身份和长度均一致，证据 output_member_review.json。没有为排版和配对约束启动新采集，未验证新的原生运行；没有 test、fixture、向后兼容路径或 .clang-format 修改。

## 保留的维护问题

TemplateDefinition/TemplateBank 仍承担较多结构选择与报告工作。此次只分离已有独立类职责，没有按行数继续拆方法或引入额外架构；后续修改应优先消除实际重复，而非不断加新文件。workspace 后续修改已将使用点绑定到目标拓扑；继续要求初始化、目标使用点、容量与分支保持一条数据流，避免按功能缺口增加独立诊断包装。

本次代码质量周期检查结束，计数归零。功能进度和完整 S3–S6 门槛仍由执行计划维护，当时 goal 为 active（历史状态）。


## 随后五批修改的复查

复查了CMake依赖、C++输入/控制/模板/执行/成本边界，Python公共插桩与产物的import，以及固定预热生产和新状态调用链；全包Ruff通过。框架名称扫描仅在输入合同解析中命中允许的框架枚举，不将其误判为核心算法耦合。无公共插桩反向依赖，CMake方向未变。

WorkspaceState归执行层并由SimulationRuntime独占；TemplateBank提供只读来源及替换计划，不持第二份容量。控制器等待Published后才推进下一批，ExecutionGraph只重算时序。容量副本在整批图接纳后提交，局部规划失败不污染已提交状态；该所有权适用于当前non-overlap范围，不扩展到未实现的并发控制。

代码收紧：allocator与语义区域的共同装配提取为plan_subgraph_replacement，未复制端口/依赖装配器；跨来源资源资格检查归并到共同方法，删除两处重复。补齐workspace_state的utility直接包含，去除多余空行。审阅发现仅操作名匹配不足以资格化donor，已加入真实目标输入下需求字节及分组等价判定，避免另建名称白名单或特例。

仍需处理的逻辑边界：共同替换器的等待前驱依赖区域资源端口，而allocator只拥有CPU端口；实际设备查询需要当前目标动作内的提交前沿。现有拒绝保留，不能为跑通增加固定边。插入子图的二次区域替换目前明确未资格化。这两项是结构实现任务，不以本次代码审阅或构建成功关闭。正式验证与证据详见同日执行日志；本次周期复查完成后代码批次归零。


## 后续五批代码的周期检查

覆盖上一轮审查后的workspace状态/等待绑定、来源资格与完成端口、采集清理握手、共享需求规则，沿workflow装配、生产绑定、模板codec/拓扑、控制执行及Python采集生命周期复核；并重新扫描CMake和公共目录依赖。此为代码职责/风格/逻辑检查，功能支持域继续由实施计划及workspace功能审阅维护，不称为逐行正确性证明。

- **依赖与目录**：CMake仍为workflow→adapters→core；公共C++ contracts/graph/controller/execution/performance/templates无反向adapters/workflow include，公共Python instrumentation/artifacts无框架适配导入。原生分核常量和观察结束条件分别留在Ascend/SGLang适配端。
- **所有权**：TemplateSource禁止复制，持有规则集合；TemplateBank移入来源、PredictionWorkflow持有bank直到组合终结。workspace需求借用set内稳定对象，codec先恢复规则再恢复使用点；没有增加shared_ptr或第二份规则状态。集合按完整类型化值比较，观测容量/来源/端口不参与规则等价。
- **状态职责**：WorkspaceState独占目标容量，Runtime暂存整批后提交；供体不初始化目标容量。采集端_finished_ids与_cleanup_completed分别表示已发布及原生清理完成，不能合并为一个“完成”标志；两者均留在worker，launcher只等待已落盘确认，deadline仍由NativeAttempt唯一维护。
- **逻辑收紧**：workspace规则原先每个使用点持完整可变布局，现复用现有AssetReferences和source-owned set，删除重复按值存储与逐使用点codec记录；没有新增规则管理类、通用注册器或兼容格式。缺规则仍为显式空引用，不填零。
- **拓扑边界**：替换计划只有原始节点删除能力，已插入片段不能凭供体身份重复定位；该风险已在功能审阅解释，保留拒绝。没有通过复制allocator图或扩大框架专属分支隐藏限制。
- **风格与文档**：检查修改方法的输入、选择、状态提交和产物发布分组；C++修改文件容器clang-format及git diff检查通过。workspace功能报告已合并过时结论，运行文档说明共享规则、资产重编译与实际范围。

Docker构建通过；三份活动资产从实际来源重编译，公开延迟到达组合与修改前逐项一致，详见s4-native-cleanup/workspace_rule_sharing.json。未新增test、原生采集、.clang-format或third_party改动。共享规则只减少56357字节，完整图/模板族归并及建库开销仍未通过；不以局部压缩代替维护性或泛化结论。本次周期检查收口，代码批次归零。


## attention归并至profiler生命周期的五批复查

本次复查CMake依赖、CLI→workflow装配、controller发布状态、执行层提交、模板所有者/codec，以及Python采集deadline、公共wrapper、SGLang组间握手和Ascend产物/归一化链。公共C++目录无adapters/workflow反向include，公共Python插桩/产物无适配导入，runtime无框架导入；扫描后读取实际调用方确认职责，未按扫描零命中宣称全部功能正确。

- TemplateSource拥有model/attention/collective/workspace规则，观测借用set内稳定对象；workflow持有bank到组合结束，codec先恢复所有者。workspace需求现在用expected保存规则或原因，删除空值丢失原因的旧合同，未并存第二个有效性标志。
- 请求及KV状态仍由controller在Published更新，workspace由runtime暂存到整批图接纳后提交。采集的finished IDs与cleaned IDs分别表示发布和原生清理事实，组间消费累计清理确认；两者职责不同，不合并成“完成”布尔值。
- profiler启停范围归AscendProfileWindow，SGLang只包裹原生调用；公共Recorder和serving block合同未增加profiler伪步骤。失败时finally写终态，原异常继续传播；原有NativeAttempt持设备锁、拥有deadline及worker清理，不增加第二套超时或重试。
- 新归属只在适配端按真实调用范围和同线程确认，核心仿真不识别profiler名称。审阅发现同步复制内部等待入口也要排除已确认控制范围，已补齐，避免显式同步与内部同步走不同scope规则。源图保留原始活动，未知/冲突仍保留缺口；未按空owner或尾部时间删节点。
- 修改方法按读取、归属、报告分组，补必要生命周期注释；不新增管理类、registry、版本回退或test。大型normalization/source_extractor仍承担较多适配职责，本轮新增方法只分离实际启停记录读取与归属，不按行数继续拆层。

全包Ruff、容器Python AST、Docker C++构建及diff检查通过；第71次事实重提取的源图、模板及编译资产逐字节保持，见s4-group-cleanup/lifecycle_reextract_verification.json。随后补齐的内部等待条件仅针对新增profiler_control归属，已有未标记事实分支不变，最终构建另行确认。未对新启停归属做真实采集，不以该回归或审查关闭功能缺口。审查批次归零，S3–S6退出条件不变。

## 2026-10-13：合并变换至来源选择的五批复查

本次检查活动入口、CMake依赖、workflow资产生命周期、模板只读来源与装配、controller/execution状态边界、NodeDurations，以及Python Recorder→SGLang observer→Ascend产物/归一化和原生event journal。重点是职责、所有权、错误隔离与代码收紧；以下结论不是全仓逐行正确性或功能验收证明。

- **层次**：公共C++ contracts/graph/controller/execution/performance/templates未发现反向adapters/workflow include；公共Python instrumentation/artifacts没有具体适配导入，runtime没有框架导入。CMake仍为workflow→adapters→core。来源配置解释和配置变化关系集中在SGLangCollectionPlanner；TemplateBank只提供只读来源，不增加框架判断。
- **重复判断收紧**：删除PredictionWorkflow中独立维护的部署、配置、供体和初始化筛选循环，实际预测与source_coverage消费同一SGLangSourceSelection。供体与初始来源分别有真实用途，不把二者合并成一个available布尔值；缺成本仍由NodeDurations路径处理，不转成结构补采。
- **所有权和错误边界**：workflow完成资产加载后才创建SGLangSourceCandidate只读视图，持有bank与解析后的配置直至所有组合结束；selection仅保存借用指针。审阅中保留配置在加载try块内一次解析，避免把坏资产的异常移到组合循环外；没有新增吞错、默认值或逐层解析。资产身份、准入与初始化继续复用现有qualify方法。
- **状态与采集职责**：controller仍消费Published更新请求/KV，runtime拥有模拟时间，来源选择不写这些状态。NativeJournal只在显式观察环境保留入口event记录；EventHistory解释record/query/destroy并保留歧义，未加入目标控制或后台服务计费。Recorder的外层动作flush与SGLang profile启停分工保持；跨窗口并发及完整serving交接仍是已记录的待验证风险，不用SDK局部观察关闭。
- **风格和规模**：新逻辑按资格、初始化、报告分段，注释解释借用生命周期及成本/结构边界；没有新注册器、缓存、兼容入口、测试工程或重复状态。plan仍只做配置分组，predict才消费已加载资产，此边界已写清；完整机制覆盖判断仍缺，不用报告字段掩盖未实现的自动建库。

Docker C++构建和修改文件格式检查、Python全包Ruff及diff检查通过。四条公开CLI路径复用真实编译资产：原完整组合摘要保持，缺成本保持来源资格，缺资产明确报缺口，池缩容保留供体但拒绝未资格化初始化；见s3-source-eligibility/verification.json。无新增原生采集、test、.clang-format或第三方改动。本次周期检查收口，代码批次归零；S3–S6阶段资格不变。


## 2026-10-13：审查方法纠偏

此前多次确认核心无反向 include，但 PredictionWorkflow 仍直接持有 SGLang 配置和选择逻辑。因此目录/CMake方向正确不能等同于换框架只改适配端，后续审查必须沿实际业务调用和改动落点判断。

审查继续以代码层次、风格、逻辑整理和收紧为主；不把所有原生事件差异转成实现任务，不按文件行数、拒绝条件数量或检查通过数作质量结论。MechanismEvidence 是现有来源诊断，不能把它逐步扩成独立于实际变换器的资格系统。约定直接应用于开发，不创建额外检查平台。外围解耦等代码任务在实施计划维护，本次未执行代码整改或新采集。


## 2026-10-13：机制报告至模板生产装配的五批复查

覆盖 CLI→规划/采集准备/预测/提取编译→适配器→模板/控制/执行的实际调用和拥有关系，复核 CMake 依赖；未将本轮不变的底层事件重新作为验收任务。

- **分层与职责**：workflow源码及头文件不再引用具体framework/runtime；CLI显式组装TemplateAdapter与CollectionAdapter，后者允许KT真实采集而不伪造预测实现。SGLang模板适配器组合原生提取和生产，编译资产发布仍属于共同workflow。ProjectReader及配置值解析仍有具体框架逻辑，是剩余实际耦合，不以扫描零命中关闭全部外围任务。
- **所有权**：入口持有适配器直至命令结束，workflow仅借用span；来源bank和已解析配置由预测workflow持有，加载结束后才构建借用视图。提取方法中的SGLangBindings覆盖AscendSourceExtractor整个调用；编译source拥有规则，编码完成后销毁，无跨调用借用。
- **状态与错误**：Controller和SimulationRuntime的请求/KV、时钟及workspace状态职责未改变，NodeDurations仍独立于结构来源。公共编译在实际生产前撤销旧资产引用，成功写入后再发布；不支持的框架在进入生产前报错。无共享可变注册器、第二控制器、缓存或兼容解析。
- **代码收紧与范围**：已有预测接口扩为模板生产/选择接口并同步更名，删除旧文件及名称，没有叠加新的生产接口。配置/原生观测元数据从Planner迁到有实际消费者的采集适配器；JSON仅用于现有外部输入和报告交接。接口不新增实例化特判，机制报告未扩为第二验证器。
- **跨语言和风格**：公共Python instrumentation/artifacts导入扫描未发现反向适配依赖，本轮未改Python。修改的C++方法按解析、选择、执行及发布分段；容器格式和diff检查通过，未改.clang-format、third_party或增加test工程。

Docker正式构建通过，五批公开回归及其范围集中于执行日志。此次现有base完整extract包含编译，图/模板/编译字节保持；没有原生采集或独立目标验证，不新增支持域。周期审查结束，计数归零。
