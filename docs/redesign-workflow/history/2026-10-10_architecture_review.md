# 整体架构审查：历史证据与遗留边界

> 历史审查/观测记录：结论限定于文中日期与当时实现。2026-10-16 清空数据后，旧 `data/redesign` 地址不再可复核；本文的“当前”“待完成”不作为现在的任务或授权状态。实时状态见[主计划](../2026-10-08_redesign_execution_plan.md)。

2026-10-13 整理。本文保留 10 月 10–12 日审查的关键结构证据，不维护当前进度或下一步。原逐批检查合并；[当前实施计划](../2026-10-08_redesign_execution_plan.md)决定任务，[代码质量审查](2026-10-12_code_structure_review.md)记录职责/风格整改。以下数据均限定于当时资产，不据其推导最新实现资格。

## 主要发现及处理

| 发现 | 已采取的处理与边界 |
| --- | --- |
| 模板生产和来源解释耦合预测入口 | 生产绑定与预测分离，编译资产可隔离原始数据加载；外围 workflow 的 SGLang 类型仍须解耦 |
| 重复层结构与观测身份混在一起 | MLP局部图、模型/通信规则和资源接口共享，保留每次来源绑定；共享不等于跨结构变换 |
| 给定耗时与结构缺口互相污染 | 耗时输入失败不丢弃结构资产，组合缺口隔离；正式成本模型未因此实现 |
| 主机等待、资源撤销与复制存在缺口 | 明确等待范围、资源前沿、真实复制参数和必要端口；已有修正证据，不据此宣称任意布局成立 |
| 局部预览绕过完整人数资格 | 删除 single_request_topology 预览及专属替换，实际预测走共同实例化 |
| 审查越来越依赖原生全事件比较 | 改为统一流程范围内的变换、接口与状态验证；模板外服务排除，未归属总数不是门槛 |

## 历史结构证据

以下保留原审查中的重要依据，文中的“当时”均指对应资产的开发检查；旧独立输入若已参与修正，其最新用途以执行记录为准。

### MLP重复区域复用核对

证据目录：data/redesign/s3-runtime-wait-capture。覆盖30次模型调用的1920个MLP区域；以下均为当时来源证据，不授予独立目标或成本资格。

| 核对项 | 已证实结果 | 证据文件 |
| --- | --- | --- |
| 局部连接 | 14段外部CPU路径经完整非线程连通分量检查后投影；按资源内编号为3组连接结构，30次调用内各64层一致 | mlp_projected_topology_summary.json及其路径证明 |
| 关系标签投影 | 保留路径上的关系序列后有10种表示；相对三组主要表示，10个区域新增59种路径形式，无缺失形式，差异只含thread_order/interval_extent，集中在第0/1层。逐条路径归一化见下一行，不能据此新增模板族 | mlp_labeled_topology_summary.json、mlp_labeled_topology_inventory.json、mlp_labeled_topology_differences.json |
| CPU路径归一化 | 59种额外形式在10区域中共80次，全部精确匹配同线程CPU注释extent，已有不穿过其他执行端点的纯thread_order路径；只消除这些冗余投影后恢复3组（384/768/768），30次调用各层一致 | mlp_cpu_projection_proof.json、mlp_normalized_relation_summary.json |
| 计算描述与参数 | 5760个kernel同次调用各层描述一致；观测T=1/2/16/127/128/255均符合模块H/I_rank关系，仅参数化激活token轴后一套描述 | mlp_kernel_inventory.json、mlp_kernel_parameter_verification.json |
| 通信分支与工作量 | 每区域一个collective；全部count=T×H及物理任务字节吻合已有规则，三组连接对应大消息/小消息rank0/小消息rank1 | mlp_collective_inventory.json |
| 通信依赖与完成 | 大小消息两种内部依赖；1920个唯一完整任务汇合点；4608对通知逐槽唯一匹配batch/layer/count | mlp_collective_dependency_verification.json |
| CPU角色序列 | 136320个服务片段，每区域43个execute、28个dispatch；30次调用内各层顺序一致。逐项验证所属serving marker后移除调用编号，保留phase为Prefill/Decode两种标签序列，worker操作序列相同 | mlp_cpu_inventory.json、mlp_cpu_role_verification.json |
| CPU异步提交 | 每区域同一组9个原生操作，各有一个worker；17280条host→worker、33024条worker→设备成员的区域内可达路径全部成立 | mlp_operation_inventory.json、mlp_submission_path_verification.json |
| 来源资源角色 | 每区域由真实host/worker、计算及通信成员唯一确定四类资源，全部执行lane覆盖；每rank全部区域共用一套实际资源映射 | mlp_resource_interface_inventory.json、mlp_resource_interface_summary.json |
| 外部边界计数 | 原始五类计数中的额外三类对应已证明的3/4/7段CPU路径；计入这些路径投影后只剩大小消息两类（384/1536区域） | mlp_resource_interface_summary.json |
| 外部端点职责 | 33024条完成导出全部到所属serving的device_done；3840条跨界extent引用父层CPU服务，不能当注解删除；15360条线程/设备流连接按实际模块父子与层位置逐项匹配 | mlp_external_endpoint_inventory.json、mlp_external_position_verification.json |
| 局部接口槽与外部别名 | 1920个区域按局部端点、方向、关系及远端资源域形成13种接口；加入远端端点别名、资源别名及同rank关系后仍为13种，与未含参数规则的13种局部结构一一对应；无重复端口描述造成的编号歧义 | mlp_interface_slot_inventory.json |

当时正式消费：SGLang生产端选择feed_forward区域，通用TemplateGraph共享节点、内部依赖、模型/通信参数规则、资源槽和接口。1920个MLP区域使用13份局部图，保留注释及未归一化连接，不等于诊断中的三组执行连通投影；全来源21566处模型参数绑定共用44条规则，3900次通信观测共用4条计数/字节规则。来源span、kernel描述、CPU观测时间、调用及实际资源独立保存，插入按供体定义和固定槽读取来源，目标描述符独立持有。稳定合同见[采集设计](../../redesign/04_collection_templates.md#41-从源实例到可展开模板)。

接口已统一：TemplateInterface同时供serving内切口和跨定义连接使用，共用远端表；共享定义保存端点/资源别名、相对rank及远端与局部端点重合关系，调用绑定实际编号。TemplateGraph的依赖与连接读取均消费该表，远端编号不再重复存于逐连接观测。两侧切口只解析一次原边槽，父层服务extent及跨rank通知仍保留，不新增屏障或服务成本。旧TemplateDependencyPort及单侧连接读取接口已删除，无兼容实现。

通信规则：CollectiveParameterRule把行数/宽度计数与CommunicationWorkLayout组成类型化合同，scope及原生任务仍归来源观测。共享图按包络节点槽交接，联合替换与诊断均读取该规则。保留绝对分片编号的中间表示曾得到5条规则和17份局部图；已有取证说明是4组大消息结构各按rank分裂。HcclMeshLayout已保证数据任务目的rank为本rank或对端，原分片表达式等价于任务destination_rank；参数化后得到当时4条规则和13份图，未增算法分支或放宽有效域。证据mlp_collective_rule_sharing.json及两次正式验证；中间表示不再保留于产品代码。

归属与资格：物理任务汇合点须全部贡献者属于该区域且没有外部输入；端点共享不转移外部工作的所有权。局部资源域/别名及rank关系相同，不证明跨配置CPU职责、缓存状态或成本相同。模型计算规则按类型化字段比较，来源注释不参与等价判断；其余注意力/KV、批次和传输参数合同尚未统一共享。

当时验证：全新build/markov-shared-collective-rules目录Docker Release及默认构建均无警告，后续目的rank参数化重建通过。s4-relative-collective-rules双配置完成24/28阶段；对照s4-shared-collective-rules，全部物理图（515368/615680节点、1195718/1436276边）、阶段诊断、指标、缺口及原有来源字段一致，仅产物路径与共享统计scope变化，见对应verification.json。这是固定资产行为保持，源采集文件尚未轻量化，独立目标与成本资格不因此成立。

剩余项：补齐其余参数合同，并在固定规则下独立验证目标资源/端口角色绑定；编译资产交付见下节。当时来源端点职责不能自动推广到其他资产；CPU角色序列一致不证明真实CPU工作等价。诊断中的三组连接、一套计算描述和两种CPU标签都不是完整模板族数；45种源顺序形式、4组中间投影和15种带调用编号CPU标签仅作为历史取证，不新增变体或采集规则。

### 编译资产与原始图依赖审查

已关闭此前每次预测读取675095200字节生产输入并重新绑定的问题（历史取证compiled_template_boundary_audit.json）。ExtractionWorkflow现在写出compiled_templates.cbor；PredictionWorkflow直接加载统一资产，Python仅挂载manifest和编译文件。生产参数来自采集记录，模型/配置/部署匹配留在预测入口。缺引用或引用文件不存在均返回compiled_templates_missing，不触发补采或原始图回退。

TemplateAssetCodec显式保存合同字段，将共享规则/结构、kernel/传输和通信任务的借用引用转换为所属表索引。加载先恢复来源拥有者，再恢复局部引用；定义移动不改变内部容器所拥有对象的地址。通信任务表按定义重建，不能借用解码临时对象。私有参数布局仅向这一编解码类开放访问，不增加可修改getter或版本/指纹机制。大拓扑使用位置记录、顶层定义使用命名字段；参数和结构合同变化须同步修改唯一读写实现，并明确重提取资产。

正式隔离证据为data/redesign/s4-compiled-template-asset/verification.json及isolation.json：容器只读根文件系统、无网络，未挂载原始图、实例、source_facts、环境或vendor trace；进程启动时检查前三种原始文件不可见。24/28阶段、515368/615680节点和1195718/1436276边与s4-independent-template-production完整一致，诊断、指标、缺口、来源及共享统计均保持。1920个MLP区域仍用13份局部图，模型/通信规则仍为44/4条；不是通过新分支获得运行成功。

编译文件142074701字节；本次生产绑定及编码41.298秒，加载11.335秒。重提取进程总墙钟159.894秒（包含上述编译），由Docker exec事件起止证明，见extraction_process.json。数据为本机单次实际开销，期间有编译任务，不作为基准性能结论；原始取证文件继续保留，不能把输入文件体积下降称为总存储或峰值内存下降。给定耗时与加载墙钟分别报告，目标指标不含宿主处理时间。

本次同时沿脚本→Python编排/恢复→Project→提取/编译→加载→控制与执行核完成接入审查：归一化/提取仍属于CPU后处理，已完成任务的resume复用原结果，旧资产由用户显式extract迁移；仅缺整个base_manifest可启动初次采集。发现并修正公开extract相对路径与Project绝对路径不一致的问题，生产端按Project同一方式统一保存绝对规范路径；本次正式验收使用绝对路径。控制器/执行/性能层未增加框架分支，Service独占耗时计费，Wait/Annotation仅贡献约束。完整字段读写属于资产维护职责，不新增调度或变换规则。Docker新Release及默认构建通过，无警告；Python语法和脚本检查通过，未新增test工程。

交付边界见[采集设计](../../redesign/04_collection_templates.md#编译资产与预测加载)。本项通过不授予独立目标结构、CPU/来源时钟或成本资格；其余参数合同、最终配置×workload集合入口与S5/S6仍未完成。

### 共享区域与替换接口的边界

当时消费链为 TemplateBank::instantiate_source → plan_materializations → 各有限替换 → TemplateTopology → TemplateAssembly/ExecutionGraph。TemplateDefinition::semantic_region通过TemplateGraph读取重绑定的节点和依赖，再按原索引完成内外分区；模型/通信参数规则与含远端别名的接口已纳入共享定义，其余参数合同和独立目标角色资格仍待完成。plan_region_replacement 调用 region_ports，后者只接受线程/设备流和已观测的部分等待接口，拒绝一般跨 rank 通知、完成导出及未暴露的父层服务端点。这是现有替换规则的支持边界，不是已确认的执行丢边缺陷，也不能以放宽拒绝条件解决共享定义。

MLP现有来源包含4608条跨rank通知和33024条到所属serving的完成导出；父层服务extent还必须保留。collective_operation_ports及collective_replacement已有独立通知槽和联合rank提交机制。当时共享定义通过局部端点槽与调用绑定交接，后续目标角色匹配仍须保留层位置和外部消费者，沿用原有关系类型；不将其强行转换为每资源单入口/出口，不增加统一结束屏障，不另写通信控制器。共享结构复用与跨结构分支替换分别处理，只有后者需要对应的有限替换资格。

新增接口取证复用当时原始图，按与生产共享相同的资源内节点排序与端点别名清点完整切口。接口槽加入远端端点别名、远端/局部资源别名和同rank关系后仍为13种，每种局部结构仅对应一种接口；未比较模型参数规则，也未据此删除注释或合并CPU成本。通知计数为4608条入加4608条出，即4608条跨rank依赖的两侧引用，不能误计为9216条执行依赖；完成导出33024条，父层extent入/出各1920条。该证据确定共享接口可采用槽与调用绑定，不需因层位置复制定义；正式接口读写已接入，目标角色匹配仍须独立验证。

证据：src/markov/src/templates/templates.cpp中的semantic_region、region_ports、plan_region_replacement、collective_operation_ports、instantiate_source；collective_replacement.cpp中的联合端点重绑定；来源数量见本页MLP表。节点、内部依赖、模型计算参数规则、资源及接口已进入正式消费，完整模板族与独立目标资格仍待完成。

### 独立原生对照：事件依赖保留，CPU执行缓存资格仍缺

这是历史标题。对应 s4-current-contract-holdout 及后续同步复制资料已用于开发修正，不作为最新实现的独立验收；该标题中的“仍缺”不产生新任务。

以下是 `data/redesign/s4-current-contract-holdout/verification.json` 的历史核对：97/47/37输入、3/4/2输出、0/0/400微秒到达的三请求序列，两配置使用当时API、复制和时钟记录合同。输入后续参与了规则修正，现属于开发证据。下列数量依次为当时base配置/输入预算配置；不代表当时独立验收。

| 已核对范围 | 结果 | 同目录证据后缀 |
| --- | --- | --- |
| serving调用与kernel | 48/56次rank调用顺序、10436/12592个kernel逐阶段签名匹配 | `kernel_comparison` |
| 已建模复制 | 20/26项异步设备复制及地址匹配输入上传，方向/字节/数量一致 | `current_copy_comparison` |
| host wait逐次对应 | 44/54个单线程职责组内的API类型、scope和顺序匹配，共162/222次 | `wait_sequence_comparison` |
| 必要设备等待 | 138/200个原生前沿端点均有设备路径到对应等待结束，无CPU绕行；24/22个空前沿无直接等待入边 | `wait_frontier_inventory`、`wait_frontier_paths` |
| 显式等待输入 | 展开无工作且释放条件已满足的汇合/旁路边界，210/312项均有同stream同步前提交依据；重复Concat身份由原生任务顺序及目标设备全序核实 | `extra_wait_inputs` |
| 外层CPU操作 | 19260/23294个操作在3232/3774个语义组内名称/数量/实际顺序匹配，16028/19520对相邻操作有路径 | `cpu_outer_execution_comparison` |
| 服务/等待分割 | 来源474004个CPU服务与380次等待无重叠，47984个外层操作全覆盖；目标189860/226202个已识别服务无跨等待切口，324/444条邻接路径成立，含零时长等待 | `source_cpu_partition`、`cpu_wait_partition` |
| 跨调用CPU资源 | 四条来源可识别资源的82/98次调用切换无重叠且有依赖；原生每rank各一条control和worker线程 | `cpu_cross_call_resources` |
| 区域语义flow与接收方CPU端口 | 60918/72346条flow的serving身份、binding/module_path及端点别名集合匹配；512/1536个接收方入口及同名返回边界匹配，无归属冲突 | `submission_recipient_comparison`、`recipient_return_anchors` |
| 通信逐节点依赖 | 15588/19218个物理任务按serving、接收方区域、payload和context形成无重复的双射；同一映射下17676/22858条内部任务依赖及4152/5188条跨rank通知逐边一致，无缺边/额外边 | `collective_scope_notifications`、`collective_occurrence_dependencies` |
| 通信完成汇合 | 1560/1820个目标通信子图的全部物理任务结束均接入唯一精确成员汇合点，另有共同的上层完成汇合；未用CPU返回或通信注释结束替代设备完成 | `collective_completion_membership` |
| 通信外部完成连接 | 1560/1820个通信汇合点的直接stream后继均为与原生对应的EVENT_RECORD；15588/19218个物理任务均沿完成汇合/控制连接到所属work/rank的device_done端口，无CPU服务绕行 | `collective_stream_successors`、`collective_serving_completion` |
| 跨流事件信号 | 6240/7280个物理事件端点按serving、scope、操作和资源职责唯一对应；3120/3640条记录→等待边逐边匹配，无缺失硬件等待或并发代次歧义 | `event_signal_comparison`、`event_resource_roles` |
| worker派发与内部区间 | 38/46个调用中的21294/25282次Dequeue名称/顺序匹配；61086/72592个服务片段各属唯一实例。56/82次等待位于原生对应实例，服务/等待并集完整覆盖每次派发，无重叠或空段，零时长等待仍核对归属 | `source_worker_anchors`、`worker_dispatch_comparison`、`worker_interval_partition` |
| worker运行时记录差异 | 原生24408/29090条Runtime记录，目标24400/29070条；22/28次派发有差异。原生多14/20条Binary记录及2/6个同步结束点，目标多8/6条Binary记录；差异分类无未解释项，成本不等价 | `worker_runtime_comparison`、`worker_runtime_differences` |
| worker内部必要顺序 | 完整Runtime顺序核对有0/2条失败，预算配置均涉及FloorDivides的Binary查找位置变化。检查全部Binary记录的端点职责后，将其显式排除于依赖投影；23058/27352条其余匹配记录的67396/79970条必要顺序路径全部成立，无重复身份歧义 | `worker_runtime_order`、`worker_runtime_dependency_order` |
| 未归属flow边界 | 1298/1558条记录的两端均在范围外，考虑共享起点别名后也无跨入/跨出 | `unowned_flow_boundary` |

**复用后的身份解释**：`source_span`保留供体观测，不代表接收方模型层。最初直接读取供体scope得到768/2304条通信差异；现从目标实际映射的CPU起点及同名原始操作识别接收方，只在native-flow连通组内、仅对已识别供体scope应用唯一且无冲突的接收方语义，并核对对应CPU返回边界。1536/4608个节点得到接收方解释，全部细化flow签名匹配。未更改执行图、规则或来源标签，也未删除scope条件。原诊断保留在`submission_semantic_comparison`，入口和返回取证保存在上述两项最终证据中。

**逐节点对应依据**：context编号只用作候选身份，不作为执行顺序。两侧包含完整payload及接收方语义的身份均唯一且集合相同；原生内部边由FFTS描述中的successors沿无物理工作的LABEL路径导出，通知按原生notify ID和代次配对，再用同一双射比较全部两类边。因而本组通信重复成员的内部依赖和通知疑问已关闭，不仅是数量相等。

跨采集不能直接比较物理stream编号。当时事件流职责由实际kernel与Notify Wait工作确定，每rank各一条计算流和通信流，两侧角色均唯一；在此资源对应下事件端点形成双射，记录/等待边全部一致。直接比较进程局部编号得到的差异保留为诊断，未据此改资源规则；该结论不推广到其他流布局。

worker内部Runtime记录并非逐项相同。差异仅涉及二进制加载/函数查找和零时长同步结束点：原生差异记录不是flow端点或已记录API的父区间；同步结束点均位于已经匹配的等待内。目标多出的Binary记录均为annotation，不承担已检查的提交/等待/事件端点。预算配置的两次FloorDivides还存在相同数量Binary查找在kernel提交前后的位置变化，完整Runtime序列不能宣称等价。进一步检查全部Binary记录：原生1348/1732条均无已记录flow/API父端点职责，目标1342/1718条均为无已检查因果端点的annotation。显式排除这些记录及已分类的额外零时长结束点后，其余Runtime进入/返回和派发边界的必要依赖路径全部通过。该结果不要求按Binary注释增补等待/模板，也不授予其CPU开销、缓存状态或额外依赖等价资格。

其余同步复制已清点为具体缺口：当时来源134次中仅10次输入上传已绑定；两份留出观测56/82次中仅4/6次已绑定，其余52/76次分为Prefill准备40/60次、前向元数据8/12次、过滤4/4次。方向/地址/驱动区间记录均已存在，缺的是逐复制语义与参数关系，不以再次采集代替绑定。前向元数据没有命名binding，但保留实际scope_call及serving归属；不能将binding为空误读为scope为空。原生候选及绑定约束见[同步复制说明](../../redesign/sglang_template_relations.md#输入解析与cpu上传的显式区域)，逐项事实为`source/base/budget_synchronous_copy_inventory.json`。 来源134次复制的唯一worker/队列/aten::copy_关联已核对，全部同形同类型张量的计算字节等于驱动字节；证据`source_copy_provenance`及`source_copy_tensor_provenance`。观测分组不等于逐项身份资格，也不授予相同人数下所有字节固定的通用规则。

后续来源整改`s3-synchronous-copy-work`已接入全部同步driver工作的描述：134项与原生驱动区间/方向/字节完全一致，新增124项；原有42项保留，现共166项。kernel、等待、通信、操作、资源、边界、span及边逐字段不变，Docker构建与公开extract完成，编译资产142076054字节。来源绑定证据为该目录verification.json及extraction_report.json。后续生产端已沿原生队列/张量链加入120项请求数组和4项过滤索引参数；重建资产中166项transfer均有显式layout（transfer_parameter_bindings.json）。公开predict使用新资产完整展开24/28阶段，72/102项复制在逐serving调用的次数、方向、字节上与已有原生观测一致；与原预测比较，除transfer描述外目标图全部字段相同。证据为parameter_expansion_verification.json及base/budget_parameter_copy_comparison.json。此次是参数整改后的开发验证，上表仍对应原固定资产，不能替代全部关系固定后的最终独立验收。

**主线程直接复制的服务边界**：另有112次`AscendCL@aclrtMemcpy`不属于上述rtMemcpyAsync/driver工作描述，其中72次位于aten::copy_、40次位于aten::_local_scalar_dense；Prepare/Prefill/Publish/Decode分别22/20/50/20次。来源全部完整落在单一CPU服务中，没有已记录的非thread/interval因果端点或HostWait覆盖。新资产的两目标图保留50/58次直接复制annotation，逐serving请求签名和region binding的计数与原生一致；每项均有唯一同work/rank的来源服务实例包围，模拟时间包含关系与入口/返回可达路径均通过。证据：旧对照目录的source_direct_copy_inventory、source_direct_copy_coverage，以及s3-synchronous-copy-work的base/budget_direct_copy_comparison和direct_copy_service_coverage。因而这些已核对调用不要求为结构展开新增复制节点或追加采集。该结论只覆盖记录到的范围与依赖，未证明方向、字节、未记录的内部同步或真实服务耗时；若S5独立选择复制性能单元，须先取得其工作量和测量边界，不能与外围CPU服务重复计费。

**主线程CANN差异与入队顺序**：两配置原生/目标记录为17168/17132、20755/20452，按serving请求签名和名称分组有149/231组计数不同，主要为workspace、tiling、注册、资源查询和零时长同步结束标记。差异组原生2794/4977条均非已记录flow端点或native_api父区间；全部在显式wrapper范围内，内部无外层ATen、wrapper、CPU flow、已记录同步或driver复制切点。目标对应2758/4674条均为annotation，无独立服务或非thread/interval因果端点，且全部被唯一同work/rank的CPU服务或等待实例包围，包含关系及两侧可达路径通过。仅以ATen为包围范围时仍有2340/4160条无法归入，因此该早期诊断保留，不将非ATen算子内部工作误报为遗漏。来源41938条主线程CANN注释全部落入已提取CPU服务/等待覆盖，41358条位于单一服务，580条覆盖含等待；这不是CPU成本证明。

入队按已核对的每serving worker序列配对：21294/25282次Dequeue各有唯一原生主线程Enqueue和唯一目标native_flow前驱；每rank主线程上相邻的原生入队起点组，在目标图中的21292/25280条必要顺序路径全部存在，覆盖跨serving调用，无身份歧义。证据在s3-synchronous-copy-work的base/budget_control_api_comparison、native_control_difference_cuts、control_difference_service_coverage和control_enqueue_order；source_control_api_coverage位于原对照目录。当时不因上述注释差异补模板或重采；未证明缓存/分配开销相等、未记录依赖不存在、入队返回及其与同步的完整相对次序，不能把起点顺序核对当作完整控制端点验收。

**主线程联合端点顺序**：进一步用唯一async queue关系识别目标Enqueue的进入/返回，加入已按API/语义scope配对的106/140次主线程显式同步进入/返回，再加入48/56个Selected、Prepared、Submitted、Published端口。Selected定位子Prepare开始（空返回则定位Schedule返回），Prepared包含父Schedule后缀，Submitted/Published定位对应原生scope返回；DeviceDone不混入CPU时间排序。每rank控制线程上相邻严格有序原生端点组的42846/50898条目标路径全部通过，无身份歧义，包含跨serving边界。证据s3-synchronous-copy-work/base/budget_control_joint_endpoint_order.json。此项补齐当时两组数据的必要联合先后关系；相同原生时间不强造次序，不证明不存在额外约束、未记录行为或真实CPU成本。

**仍未授予的资格**：通信直接stream后继、所属device_done导出和跨流事件信号已核对；worker派发名称/顺序、片段归属和服务/等待区间覆盖已匹配，必要提交/返回顺序投影也通过，但完整运行时序列、逐区域身份及上述范围外的设备/CPU依赖仍未完整对应；当时数据的主线程联合端点次序已补齐；完整支持域的独立验收、未记录/非flow依赖和来源设备时间资格仍须分别处理。已核对的直接复制服务边界不重复列为缺失节点；CPU成本与跨时钟时长不能由上述结构通过推导。源/目标区间覆盖也不是纯CPU服务耗时或成本模型。后续复用现有事实检查这些关系，不按名称、请求长度或供体标签补模板。

旧 `s4-independent-joint-native` 已是开发证据：第58层缺独立EVENT_WAIT但依赖路径保留，历史通信payload多重集匹配，输入上传工作与内部等待后来已通过当时记录合同接通。旧API wall_time与profiler直接比较的判断已撤回；原始时钟可恢复，但缺driver copy/地址记录，不能补空字段迁移。workspace/tiling差异被包围于单一CPU覆盖候选且无额外切口，只能排除按注释数补模板的理由，不能证明executor缓存行为或成本相等。历史细节、取证文件及范围见[执行记录](../2026-10-08_redesign_execution_log.md)，不重复列为当时缺陷。

## 2026-10-12：资源、等待与成员序列的周期审查

历史沿 workflow→模板来源→控制器→变换→执行→报告核对数据流，修正了来源身份、请求成员序列、跨来源资源角色及等待前沿交接。后续 Cat 输入列表、释放成员域、代表配方和通知排序沿现有公共合同实施，没有恢复第二控制器或模块成本后端。

这些检查的已知局限是：依赖扫描和局部回归不能证明外围业务完全解耦；所有待验证分支也不应成为阶段门槛。最新代码职责证据见代码质量审查；具体输入/结果见执行记录的证据索引，不重复滚动下一步。

## 遗留边界的使用方式

外围装配解耦是明确代码任务。跨配置初始状态是对应配置复用的实际缺口。更多线程/分配生命周期及任意嵌套替换是条件性扩展，先看承诺场景是否需要。模板外后台工作是范围约定，不能翻译为待实现模拟器。S3/S4及成本阶段的完整完成条件只由当前计划维护。
