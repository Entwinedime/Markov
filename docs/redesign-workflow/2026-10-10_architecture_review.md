# 整体架构与约束审查

更新：2026-10-12。最新结论见[资源、等待与成员序列的周期审查](#2026-10-12资源等待与成员序列的周期审查)；此前各节保留当时的检查范围和证据，不作为当前状态副本。阶段状态见[实施计划](2026-10-08_redesign_execution_plan.md)，运行记录见[执行记录](2026-10-08_redesign_execution_log.md)。

## 2026-10-11：时钟与复制范围审查

本周期五批为输入解析观测、上传工作参数、同步驱动边界、复制执行分类、CPU复制切点；随后在审查中修正时钟域交接。沿采集引用→native记录→归一化→SourceExtractor→SGLang参数生产→TemplateBank→执行图核对。旧周期联合资源证据保留于下文；不能替代新观测合同的完整运行。

- **已修正的时钟缺陷**：normalization曾将native父API实时钟配对直接用于profiler区间比较；安装版导出器实际使用host/start_info固定起点。诊断中两者差10070–10080 ns，导致同步驱动结束看似越过父调用5520 ns。现由capture显式引用clock_start_info，归一化生成统一trace_interval/copy_interval，HostWait消费同一坐标。依据是安装导出器公式及实际元数据，不是拟合残差。四API、两驱动子区间按该坐标均在父内，证据export_clock_domain_audit.json和copy_clock_verification_current.json。
- **历史全量关联核对**：source_export_clock_audit.json只读复核两份来源各19666个已关联CPU父调用。统一坐标后全部包含；此前分别仅288和89项包含，没有出现原先包含而修正后不包含的项。原有设备/HostWait冲突156/105项仍全部存在，最大已知迟到297720/251748 ns。该核对只重算已知冲突，不代表不存在新增冲突，也不把CPU坐标修正扩张成设备时钟修复。
- **职责与生命周期**：DriverCopyScope只在父rtMemcpyAsync生命周期内收集同线程嵌套drvMemcpy，退出恢复此前上下文。框架输入身份和INT64 token关系仍在production/sglang；公共控制/执行不认识原生函数。SourceTransfer沿既有模板实例覆盖传到输出，没有第二套复制执行器。
- **内部等待整改**：Level2四格诊断新增47条runtime记录，两个同步H2D各有唯一父内同步区间、两个异步对照没有；正式归一化保持2/2分类及4次CPU关联。SourceExtractor已复用HostWait和流前沿消费内部同步，保留前后CPU服务，Docker构建通过。生产profile通过observer限定作用域启用Level2，s3-runtime-wait-capture完成完整采集/提取，134次同步H2D都有内部等待/流资源且无CPU服务重复覆盖，10次输入transfer精确匹配驱动；新来源给定100 ns的双配置目标展开24/28阶段通过，56/82个复制等待均保留并完成资源绑定，4/6次上传字节及等待→复制→kernel路径通过；统一100 ns下等待均为0；保持CPU为100 ns、设备改为10000 ns后，分别12/18个等待为正，最大17900 ns，完整展开和同一依赖检查通过。该结果仅验证给定耗时反馈。140项设备/等待时钟冲突保持；同步包络的CPU成本也未资格化。证据s3-copy-runtime-level/verification.json，未用区间差拟合成本。
- **内部同步观测接口**：安装MSPTI的同步回调候选已检查。一次有界诊断因遗漏libmspti启动预加载而在启用回调时失败（返回6），没有执行复制场景，不能据此判定内部同步不可观测。后续静态检查确认回调9在aclrtSynchronizeStream外层包装器发出，没有证据覆盖rtMemcpyAsync内部同步；不为只获得外层事件追加采集或接入生产订阅器。事实见s3-copy-mspti-boundary/verification.json和installed_callback_boundary.txt。后续Level2诊断已找到runtime自身同步记录，见下项，不再叠加ACL包装。
- **验证范围**：当前源码已在Docker构建，并用s3-runtime-wait-capture完成真实采集/提取，s4-runtime-wait-expansion完成两组给定耗时下的双配置目标展开。早先整包络绑定结果仅保留历史意义；新来源六请求主回归s4-runtime-wait-main完整61/83阶段，134/156个复制等待绑定、10/12次上传字节及等待→复制→kernel路径通过；模型输入、KV和发布指标无错误，全部物理节点/边无时间逆序。当前开发主来源据此改为s3-runtime-wait-capture；独立原生结构资格没有由开发通过自动提升。
- **当前时钟冲突的因果核对**：s3-runtime-wait-capture/clock_conflict_paths.json覆盖140项冲突、63次等待，仅允许设备顺序、完成汇合、原生event/notify/task依赖，再接native_host_wait；全部设备结束均到达正确等待结束，无需CPU顺序或native_flow绕行。冲突分布为普通同步87项、带超时同步49项、复制内部同步4项，最大283846 ns。现有图不是因漏掉这些等待路径而冲突；时间与成本资格仍缺，不能拟合偏移或删边。
- **资产与规模**：新原生字段和clock_start_info是当前合同；旧采集缺字段不可回填假观测或走兼容解析。上述来源时钟整改时累计42次管线加10次诊断；后续两次独立验证使当前累计为44次管线加10次诊断，见独立原生对照。SDK头文件pedantic警告不等于自有代码错误；原生探针开销仍非成本模型。
- **无归属同步与释放尾部**：四个未知设备同步不在serving模板或跨模板接口。sync_serving_scope_verification.json进一步检查全部已归属CPU span和完整源图的后继闭包：同步开始晚于本rank最后已归属CPU结束1.782150–6.213690 ms，无可达的已归属服务边界。这排除当前记录图内的回流影响，不证明观测完整或调用者身份，也不构成通用忽略规则。新来源每rank最后两个设备节点是allocator.free的Cast/Concat，Published本就不代表释放设备工作完成。s4-runtime-wait-main的10/12个释放结果均匹配完成请求；有后续分配的8/10个节点全部保留至下一分配的路径，无须补release→Published同步。来源证据与目标验证见unowned_sync_closure.json、publication_completion_scopes.json和release_dependency_verification.json；未知同步缺口保留。
- **模板规模的实际口径**：template_scale_inventory.json记录当前来源122个顶层serving实例（双rank合计：Schedule 32、Prepare 30、Prefill 10、Decode 20、Publish 30）、10160个内部区域实例；templates.json为100255523字节。BlockExtractor::extract/extract_regions输出实例及源边界引用，没有独立的模板族定义；6种serving binding也不是6个已资格化模板族。重复模型层区域仍逐调用记录，不能把区域数、绑定数或运行成功当作模板归并完成。下一步须区分采集来源实例与实际复用定义，并按参数、拓扑、端口和资源关系证明归并；不得仅按名称/节点数去重。 消费路径仍依赖source_batch联合选择及source_boundary/call端点关联，归并不能直接删来源实例。公开报告已改为observed_step_phase_rank_groups与observations；s4-template-report正式运行确认18分组/122观测、24/28阶段，汇总和指标不变。职责拆分合同见采集设计，尚未实现共享模板族。 后续已统一已有/新增局部依赖类型并删除无消费者的内部source_edge，s4-local-dependencies完整双配置报告不变。dependency_reuse_inventory.json确认整段有序边数组共享仅节省0.626%记录，未为此增加存储机制；归并重点仍为重复模型区域。
- **重复MLP的复用证据**：计算、通信及CPU角色序列的现有资产核对见下节；尚未实现完整模板族归并。
- **整体未完成项**：来源设备时钟/等待、独立资源与结构验证、模板族和轻量建库、最终固定模型×配置集×workload集入口、节点成本与S6闭环均保留。PredictionWorkflow仍消费显式base/targets及给定节点耗时；S3–S6不能收口。

关闭本周期需要：核对统一时间域对全部同步路径的影响，完成同步复制前缀处理及当前合同的正式运行。期间优先整改上述问题，不新增长度特例或成本拟合。

### 2026-10-11：共享结构消费后的周期审查

本次在节点、依赖、参数、资源和连接合同五批实质代码修改后，沿公开scripts/markov.sh、Python prediction/workflow、Project解析、PredictionWorkflow装配、生产提取/绑定、模板替换、控制器和执行计费贯穿检查，并核对CMake与当前设计。静态检查用于职责和所有权判断，运行正确性以本页所列正式资产为准，不宣称已覆盖所有输入。共享定义由TemplateSource集合拥有，移动进入TemplateBank后保持不可变，连接合同与来源表按原索引交接；目标连接独立拥有可修改成员，不引用迭代临时值。没有新生命周期管理器或框架分支。

- **已整改的重复状态**：TemplateInsertedNode原先同时保存供体定义/槽位和一份复制的TemplateNodeObservation。插入和重定位只更新begin/end，供体在TemplateBank中保持不变；现改为通过供体定义与原槽位读取唯一观测，目标描述符覆盖仍由插入实例拥有。不会用接收方槽位解释供体来源，也不增加同步或新生命周期管理类。
- **分层与执行合同**：框架选择仍集中在工作流/生产端；控制器、执行和性能目录未新增框架名称分支。LightweightController只在约定端口推进请求；ExecutionGraph只给Service节点加显式耗时，缺耗时返回缺口，Wait/Annotation仅贡献依赖。NodeDurations仍是带来源与假设的给定耗时，不是成本模型。
- **采集与交付合同**：Python仅因缺整个base manifest启动有界初次采集，已有结构缺口不自动补采。Project解析与恢复仍绑定base/targets和单workload，最终集合合同未迁移；原生评价与预测资产隔离规则保留。CMake使用C++23，未新增test工程或旧实现兼容层。
- **继续保留的高优先级缺口**：模型区域的节点、内部依赖、模型参数规则、局部资源及跨定义单侧连接合同已进入共享消费；其余参数合同、完整接口与跨目标资源职责仍缺；有限替换接口不等于共享区域接口；来源时钟、独立目标结构资格、轻量建库和最终集合输入仍缺证据。先完成S3/S4，不能借当前运行complete推进到S5成本验收。

### MLP重复区域复用核对

证据目录：data/redesign/s3-runtime-wait-capture。覆盖30次模型调用的1920个MLP区域；以下均为当前来源证据，不授予独立目标或成本资格。

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

当前正式消费：SGLang生产端选择feed_forward区域，通用TemplateGraph共享节点、内部依赖、模型/通信参数规则、资源槽和接口。1920个MLP区域使用13份局部图，保留注释及未归一化连接，不等于诊断中的三组执行连通投影；全来源21566处模型参数绑定共用44条规则，3900次通信观测共用4条计数/字节规则。来源span、kernel描述、CPU观测时间、调用及实际资源独立保存，插入按供体定义和固定槽读取来源，目标描述符独立持有。稳定合同见[采集设计](../redesign/04_collection_templates.md#31-从源实例到可展开模板)。

接口已统一：TemplateInterface同时供serving内切口和跨定义连接使用，共用远端表；共享定义保存端点/资源别名、相对rank及远端与局部端点重合关系，调用绑定实际编号。TemplateGraph的依赖与连接读取均消费该表，远端编号不再重复存于逐连接观测。两侧切口只解析一次原边槽，父层服务extent及跨rank通知仍保留，不新增屏障或服务成本。旧TemplateDependencyPort及单侧连接读取接口已删除，无兼容实现。

通信规则：CollectiveParameterRule把行数/宽度计数与CommunicationWorkLayout组成类型化合同，scope及原生任务仍归来源观测。共享图按包络节点槽交接，联合替换与诊断均读取该规则。保留绝对分片编号的中间表示曾得到5条规则和17份局部图；已有取证说明是4组大消息结构各按rank分裂。HcclMeshLayout已保证数据任务目的rank为本rank或对端，原分片表达式等价于任务destination_rank；参数化后得到当前4条规则和13份图，未增算法分支或放宽有效域。证据mlp_collective_rule_sharing.json及两次正式验证；中间表示不再保留于产品代码。

归属与资格：物理任务汇合点须全部贡献者属于该区域且没有外部输入；端点共享不转移外部工作的所有权。局部资源域/别名及rank关系相同，不证明跨配置CPU职责、缓存状态或成本相同。模型计算规则按类型化字段比较，来源注释不参与等价判断；其余注意力/KV、批次和传输参数合同尚未统一共享。

当前验证：全新build/markov-shared-collective-rules目录Docker Release及默认构建均无警告，后续目的rank参数化重建通过。s4-relative-collective-rules双配置完成24/28阶段；对照s4-shared-collective-rules，全部物理图（515368/615680节点、1195718/1436276边）、阶段诊断、指标、缺口及原有来源字段一致，仅产物路径与共享统计scope变化，见对应verification.json。这是固定资产行为保持，源采集文件尚未轻量化，独立目标与成本资格不因此成立。

剩余项：补齐其余参数合同，并在固定规则下独立验证目标资源/端口角色绑定；编译资产交付见下节。当前来源端点职责不能自动推广到其他资产；CPU角色序列一致不证明真实CPU工作等价。诊断中的三组连接、一套计算描述和两种CPU标签都不是完整模板族数；45种源顺序形式、4组中间投影和15种带调用编号CPU标签仅作为历史取证，不新增变体或采集规则。

### 编译资产与原始图依赖审查

已关闭此前每次预测读取675095200字节生产输入并重新绑定的问题（历史取证compiled_template_boundary_audit.json）。ExtractionWorkflow现在写出compiled_templates.cbor；PredictionWorkflow直接加载统一资产，Python仅挂载manifest和编译文件。生产参数来自采集记录，模型/配置/部署匹配留在预测入口。缺引用或引用文件不存在均返回compiled_templates_missing，不触发补采或原始图回退。

TemplateAssetCodec显式保存合同字段，将共享规则/结构、kernel/传输和通信任务的借用引用转换为所属表索引。加载先恢复来源拥有者，再恢复局部引用；定义移动不改变内部容器所拥有对象的地址。通信任务表按定义重建，不能借用解码临时对象。私有参数布局仅向这一编解码类开放访问，不增加可修改getter或版本/指纹机制。大拓扑使用位置记录、顶层定义使用命名字段；参数和结构合同变化须同步修改唯一读写实现，并明确重提取资产。

正式隔离证据为data/redesign/s4-compiled-template-asset/verification.json及isolation.json：容器只读根文件系统、无网络，未挂载原始图、实例、source_facts、环境或vendor trace；进程启动时检查前三种原始文件不可见。24/28阶段、515368/615680节点和1195718/1436276边与s4-independent-template-production完整一致，诊断、指标、缺口、来源及共享统计均保持。1920个MLP区域仍用13份局部图，模型/通信规则仍为44/4条；不是通过新分支获得运行成功。

编译文件142074701字节；本次生产绑定及编码41.298秒，加载11.335秒。重提取进程总墙钟159.894秒（包含上述编译），由Docker exec事件起止证明，见extraction_process.json。数据为本机单次实际开销，期间有编译任务，不作为基准性能结论；原始取证文件继续保留，不能把输入文件体积下降称为总存储或峰值内存下降。给定耗时与加载墙钟分别报告，目标指标不含宿主处理时间。

本次同时沿脚本→Python编排/恢复→Project→提取/编译→加载→控制与执行核完成接入审查：归一化/提取仍属于CPU后处理，已完成任务的resume复用原结果，旧资产由用户显式extract迁移；仅缺整个base_manifest可启动初次采集。发现并修正公开extract相对路径与Project绝对路径不一致的问题，生产端按Project同一方式统一保存绝对规范路径；本次正式验收使用绝对路径。控制器/执行/性能层未增加框架分支，Service独占耗时计费，Wait/Annotation仅贡献约束。完整字段读写属于资产维护职责，不新增调度或变换规则。Docker新Release及默认构建通过，无警告；Python语法和脚本检查通过，未新增test工程。

交付边界见[采集设计](../redesign/04_collection_templates.md#编译资产与预测加载)。本项通过不授予独立目标结构、CPU/来源时钟或成本资格；其余参数合同、最终配置×workload集合入口与S5/S6仍未完成。

### 共享区域与替换接口的边界

当前消费链为 TemplateBank::instantiate_source → plan_materializations → 各有限替换 → TemplateTopology → TemplateAssembly/ExecutionGraph。TemplateDefinition::semantic_region通过TemplateGraph读取重绑定的节点和依赖，再按原索引完成内外分区；模型/通信参数规则与含远端别名的接口已纳入共享定义，其余参数合同和独立目标角色资格仍待完成。plan_region_replacement 调用 region_ports，后者只接受线程/设备流和已观测的部分等待接口，拒绝一般跨 rank 通知、完成导出及未暴露的父层服务端点。这是现有替换规则的支持边界，不是已确认的执行丢边缺陷，也不能以放宽拒绝条件解决共享定义。

MLP现有来源包含4608条跨rank通知和33024条到所属serving的完成导出；父层服务extent还必须保留。collective_operation_ports及collective_replacement已有独立通知槽和联合rank提交机制。当前共享定义通过局部端点槽与调用绑定交接，后续目标角色匹配仍须保留层位置和外部消费者，沿用原有关系类型；不将其强行转换为每资源单入口/出口，不增加统一结束屏障，不另写通信控制器。共享结构复用与跨结构分支替换分别处理，只有后者需要对应的有限替换资格。

新增接口取证复用当前原始图，按与生产共享相同的资源内节点排序与端点别名清点完整切口。接口槽加入远端端点别名、远端/局部资源别名和同rank关系后仍为13种，每种局部结构仅对应一种接口；未比较模型参数规则，也未据此删除注释或合并CPU成本。通知计数为4608条入加4608条出，即4608条跨rank依赖的两侧引用，不能误计为9216条执行依赖；完成导出33024条，父层extent入/出各1920条。该证据确定共享接口可采用槽与调用绑定，不需因层位置复制定义；正式接口读写已接入，目标角色匹配仍须独立验证。

证据：src/markov/src/templates/templates.cpp中的semantic_region、region_ports、plan_region_replacement、collective_operation_ports、instantiate_source；collective_replacement.cpp中的联合端点重绑定；来源数量见本页MLP表。节点、内部依赖、模型计算参数规则、资源及接口已进入正式消费，完整模板族与独立目标资格仍待完成。

### 集合入口迁移：已完成的边界解耦

以下三个整改已接入正式路径，均未引入兼容接口或第二执行器：

- SimulationRuntime只借用当前workload的请求、控制器、模板和耗时，接收资源限制及可选图路径；Project和base/target命名留在工作流。双配置完整报告/图保持，证据s4-exploration-runtime-boundary/verification.json。
- SGLangSamplingRecipes只接收配置、prompt来源及显式需求或独立通信资产。CPU/device必须给出非空测量域，删除默认workload组合网格。证据s4-explicit-sampling-boundary/verification.json。
- predict从编译资产读取来源配置，逐候选核对控制路径及有限变换；模型、manifest身份、部署和耗时来源仍检查。不支持的比较base不阻断合法候选，合法完整图保持，证据s4-source-owned-configuration/verification.json。plan明确不检查资产适用性，SGLang配置解析/序列化共用值对象方法。

公开合同已切换为configurations×request_workloads和assets.templates。ProjectReader、Planner、Python快照/挂载/恢复及C++组合报告同步消费；无旧项目包装循环或兼容字段。资产当前按配置/部署及已有请求池适用域选择单来源，模板语义覆盖联合选择仍待实现。代表性建库、轻量模板族与S3–S6仍未完成。

### 独立原生对照：事件依赖保留，CPU执行缓存资格仍缺

以下是 `data/redesign/s4-current-contract-holdout/verification.json` 的历史核对：97/47/37输入、3/4/2输出、0/0/400微秒到达的三请求序列，两配置使用当时API、复制和时钟记录合同。输入后续参与了规则修正，现属于开发证据。下列数量依次为当时base配置/输入预算配置；不代表当前独立验收。

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

跨采集不能直接比较物理stream编号。当前事件流职责由实际kernel与Notify Wait工作确定，每rank各一条计算流和通信流，两侧角色均唯一；在此资源对应下事件端点形成双射，记录/等待边全部一致。直接比较进程局部编号得到的差异保留为诊断，未据此改资源规则；该结论不推广到其他流布局。

worker内部Runtime记录并非逐项相同。差异仅涉及二进制加载/函数查找和零时长同步结束点：原生差异记录不是flow端点或已记录API的父区间；同步结束点均位于已经匹配的等待内。目标多出的Binary记录均为annotation，不承担已检查的提交/等待/事件端点。预算配置的两次FloorDivides还存在相同数量Binary查找在kernel提交前后的位置变化，完整Runtime序列不能宣称等价。进一步检查全部Binary记录：原生1348/1732条均无已记录flow/API父端点职责，目标1342/1718条均为无已检查因果端点的annotation。显式排除这些记录及已分类的额外零时长结束点后，其余Runtime进入/返回和派发边界的必要依赖路径全部通过。该结果不要求按Binary注释增补等待/模板，也不授予其CPU开销、缓存状态或额外依赖等价资格。

其余同步复制已清点为具体缺口：当前来源134次中仅10次输入上传已绑定；两份留出观测56/82次中仅4/6次已绑定，其余52/76次分为Prefill准备40/60次、前向元数据8/12次、过滤4/4次。方向/地址/驱动区间记录均已存在，缺的是逐复制语义与参数关系，不以再次采集代替绑定。前向元数据没有命名binding，但保留实际scope_call及serving归属；不能将binding为空误读为scope为空。原生候选及绑定约束见[同步复制说明](../redesign/sglang_template_relations.md#输入解析与cpu上传的显式区域)，逐项事实为`source/base/budget_synchronous_copy_inventory.json`。 来源134次复制的唯一worker/队列/aten::copy_关联已核对，全部同形同类型张量的计算字节等于驱动字节；证据`source_copy_provenance`及`source_copy_tensor_provenance`。观测分组不等于逐项身份资格，也不授予相同人数下所有字节固定的通用规则。

后续来源整改`s3-synchronous-copy-work`已接入全部同步driver工作的描述：134项与原生驱动区间/方向/字节完全一致，新增124项；原有42项保留，现共166项。kernel、等待、通信、操作、资源、边界、span及边逐字段不变，Docker构建与公开extract完成，编译资产142076054字节。来源绑定证据为该目录verification.json及extraction_report.json。后续生产端已沿原生队列/张量链加入120项请求数组和4项过滤索引参数；重建资产中166项transfer均有显式layout（transfer_parameter_bindings.json）。公开predict使用新资产完整展开24/28阶段，72/102项复制在逐serving调用的次数、方向、字节上与已有原生观测一致；与原预测比较，除transfer描述外目标图全部字段相同。证据为parameter_expansion_verification.json及base/budget_parameter_copy_comparison.json。此次是参数整改后的开发验证，上表仍对应原固定资产，不能替代全部关系固定后的最终独立验收。

**主线程直接复制的服务边界**：另有112次`AscendCL@aclrtMemcpy`不属于上述rtMemcpyAsync/driver工作描述，其中72次位于aten::copy_、40次位于aten::_local_scalar_dense；Prepare/Prefill/Publish/Decode分别22/20/50/20次。来源全部完整落在单一CPU服务中，没有已记录的非thread/interval因果端点或HostWait覆盖。新资产的两目标图保留50/58次直接复制annotation，逐serving请求签名和region binding的计数与原生一致；每项均有唯一同work/rank的来源服务实例包围，模拟时间包含关系与入口/返回可达路径均通过。证据：旧对照目录的source_direct_copy_inventory、source_direct_copy_coverage，以及s3-synchronous-copy-work的base/budget_direct_copy_comparison和direct_copy_service_coverage。因而这些已核对调用不要求为结构展开新增复制节点或追加采集。该结论只覆盖记录到的范围与依赖，未证明方向、字节、未记录的内部同步或真实服务耗时；若S5独立选择复制性能单元，须先取得其工作量和测量边界，不能与外围CPU服务重复计费。

**主线程CANN差异与入队顺序**：两配置原生/目标记录为17168/17132、20755/20452，按serving请求签名和名称分组有149/231组计数不同，主要为workspace、tiling、注册、资源查询和零时长同步结束标记。差异组原生2794/4977条均非已记录flow端点或native_api父区间；全部在显式wrapper范围内，内部无外层ATen、wrapper、CPU flow、已记录同步或driver复制切点。目标对应2758/4674条均为annotation，无独立服务或非thread/interval因果端点，且全部被唯一同work/rank的CPU服务或等待实例包围，包含关系及两侧可达路径通过。仅以ATen为包围范围时仍有2340/4160条无法归入，因此该早期诊断保留，不将非ATen算子内部工作误报为遗漏。来源41938条主线程CANN注释全部落入已提取CPU服务/等待覆盖，41358条位于单一服务，580条覆盖含等待；这不是CPU成本证明。

入队按已核对的每serving worker序列配对：21294/25282次Dequeue各有唯一原生主线程Enqueue和唯一目标native_flow前驱；每rank主线程上相邻的原生入队起点组，在目标图中的21292/25280条必要顺序路径全部存在，覆盖跨serving调用，无身份歧义。证据在s3-synchronous-copy-work的base/budget_control_api_comparison、native_control_difference_cuts、control_difference_service_coverage和control_enqueue_order；source_control_api_coverage位于原对照目录。当前不因上述注释差异补模板或重采；未证明缓存/分配开销相等、未记录依赖不存在、入队返回及其与同步的完整相对次序，不能把起点顺序核对当作完整控制端点验收。

**主线程联合端点顺序**：进一步用唯一async queue关系识别目标Enqueue的进入/返回，加入已按API/语义scope配对的106/140次主线程显式同步进入/返回，再加入48/56个Selected、Prepared、Submitted、Published端口。Selected定位子Prepare开始（空返回则定位Schedule返回），Prepared包含父Schedule后缀，Submitted/Published定位对应原生scope返回；DeviceDone不混入CPU时间排序。每rank控制线程上相邻严格有序原生端点组的42846/50898条目标路径全部通过，无身份歧义，包含跨serving边界。证据s3-synchronous-copy-work/base/budget_control_joint_endpoint_order.json。此项补齐当前两组数据的必要联合先后关系；相同原生时间不强造次序，不证明不存在额外约束、未记录行为或真实CPU成本。

**仍未授予的资格**：通信直接stream后继、所属device_done导出和跨流事件信号已核对；worker派发名称/顺序、片段归属和服务/等待区间覆盖已匹配，必要提交/返回顺序投影也通过，但完整运行时序列、逐区域身份及上述范围外的设备/CPU依赖仍未完整对应；当前数据的主线程联合端点次序已补齐；完整支持域的独立验收、未记录/非flow依赖和来源设备时间资格仍须分别处理。已核对的直接复制服务边界不重复列为缺失节点；CPU成本与跨时钟时长不能由上述结构通过推导。源/目标区间覆盖也不是纯CPU服务耗时或成本模型。后续复用现有事实检查这些关系，不按名称、请求长度或供体标签补模板。

旧 `s4-independent-joint-native` 已是开发证据：第58层缺独立EVENT_WAIT但依赖路径保留，历史通信payload多重集匹配，输入上传工作与内部等待后来已通过当前记录合同接通。旧API wall_time与profiler直接比较的判断已撤回；原始时钟可恢复，但缺driver copy/地址记录，不能补空字段迁移。workspace/tiling差异被包围于单一CPU覆盖候选且无额外切口，只能排除按注释数补模板的理由，不能证明executor缓存行为或成本相等。历史细节、取证文件及范围见[执行记录](2026-10-08_redesign_execution_log.md)，不重复列为当前缺陷。

### 页表物化节点存在条件：开发对照已修正，独立结构验收仍待完成

原问题是 ModelKernelLayout 只重绑定 shape，未将 PageTableView 的存在条件用于实际拓扑。历史原生对照虽 complete/74 阶段，30 个 Decode rank 调用仍各多一个 AsStrided，见 s4-native-recomparison/page_table_presence_audit.json。

当前 SGLangTemplateBindings 绑定完整 floor_divide 变体；BatchOperationVariant 按请求数、最大序列长度和页大小选择连续、单列跨行或多列跨步布局，TemplateBank 复用 plan_operation_replacement 处理完整 CPU/worker/设备子图。切口仍要求单一流入口/出口、设备完成、可映射 worker、无跨切口服务及无 serving 端口；非最终完成消费者不能被静默并入最终完成。

复跑原开发输入后，准入 2/1 分别 complete/48、74 阶段。准入 1 的 36 个模型 rank 调用 kernel 签名多重集均与历史原生一致，总数均为 28674；两配置的节点/边时序、请求进度、KV 及页表完整设备成员检查无差异。准入 2 包含 12 个连续、12 个 Slice 调用；准入 1 为 36 个连续调用。48 次页表操作实际复用另一模型阶段的已绑定变体，说明共享路径进入了正式图。证据 s4-page-table-rewrite/{kernel_comparison,physical_verification,verification}.json。

该历史输入已用于修正，属于开发证据。之后固定模板、规则和耗时运行未参与修正的新跨页序列（125 输入/6 输出、47 输入/3 输出，到达 0/350 微秒）：准入 2/1 分别 complete/28、37 阶段，最大序列从 125 推进到 130，跨越 128 页边界；实际覆盖连续、Slice、AsStrided 三布局。两配置全部模型 rank 调用均具有完整页表操作成员，参数、KV、发布与物理时序检查通过；证据 s4-page-boundary-sequence/{verification,page_layout_timeline}.json。此次未改规则、未补采，属于固定资产的独立输入合同/时序证据，尚无该输入的原生目标对照。签名多重集比较也不证明全部 CPU 或依赖与原生相同。旧来源等待及时钟缺口保持，不以正确的 kernel 数关闭它们。

### 1. 来源关联、完成语义与时钟资格（高优先级）

**无硬件等待成员时的事件依赖遗漏已修复**：原 read_event_signals 跳过空 hardware_intervals，导致唯一 Record 代次到后续通信任务在全图中不可达。现按 CANN StreamWaitEvent 的成功语义，在关联唯一、同线程且无重叠的消费流上选择各已资格化任务链的后续入口；复合任务保留所有原生依赖根。并发、歧义及提交/设备顺序不一致仍报缺口，不新增硬件任务或服务耗时。

已有事实重新 extract 后，源图所有非边字段完全相同，仅新增一条 native_event_signal：Record interval 580474 完成 → 消费任务 580475 入口；通过原生内部依赖覆盖该次通信全部 8 个物理子任务。图仍无环，原有提取/归一化缺口逐项不变。证据 s3-event-wait-projection/{source_verification,consumer_frontier_verification}.json。

Docker 构建、格式检查及新派生资产的公开 predict 通过：准入 2/1 完整 61/83 阶段；实际物理图分别消费新增依赖 3/6 次，全部节点/边时序、模型参数、KV、发布与页表成员检查通过，见 verification.json。此次修复适用于已资格化的源提交链；这组输入是开发验证，不证明任意并发或其他通信变换切口。历史硬件任务关联仍未知，156 条时钟冲突也未删除。本次无新采集。

位置：production/source_extractor.cpp 的 host wait/CPU 分割及 manifest 状态，collection/normalization.py 的原生 API 关联。readiness/dependency_coverage 仍固定赋值，templates.gaps 为空不证明整体资格。

- **内部时钟**：156 条冲突均在 serving 范围，154 条在 profiler 时间域仍为正差，最大约 294 微秒。原始 trace/msprof 已具有相同矛盾；连接、stream、线程及同步顺序已核对。138 条存在唯一 HostArgs launch 子调用；其他为事件等待、Cat/Memcpy 及 3 个无该子调用的 launch。不能拟合偏移或删边消除问题。新增完整源图回溯确认 156 个设备结束都能经设备依赖及对应 API 的 native_host_wait 到达 62 次等待结束；没有依靠 CPU 顺序或 native_flow 绕行，排除这些项漏掉等待路径的解释，不证明时钟或完整提交前沿。证据 s3-host-wait-audit/synchronization_paths.json 与 s4-independent-sequences/host_wait_clock_audit.json。 原始 STARS 计数器复算已覆盖 rank 0 的早、晚两个代表任务：采用记录频率和整数锚点的高精度转换与导出四个边界均相差小于 1 ns，厂商浮点 offset 路径只增加 50 ns，不能解释晚项 293620 ns 的同步逆序。证据 s3-host-wait-audit/raw_counter_conversion.json；该结论限定于代表项，下一步查校准或观测语义，不拟合频率。另复用三次 base 采集、两个 rank 的启动锚点（声明设备 4/5），跨运行计数器/主机速率约 49999702–49999711 Hz，相对记录的 49999001 Hz 产生约 14.03–14.20 ppm 换算差异；见 cross_run_clock_anchors.json。这是独立于 workload 时延的校准一致性线索，不是单次运行内频率证明。缺稳定设备 UUID 和同次运行末端成对计数器锚点，不能将该速率应用为修正；设备不同的旧资产已排除。 当前只读驱动诊断已确认设备 4/5 的 SYS_COUNT 持续变化；计入调用窗口的条件化速率范围仍排除历史 49999001 Hz，见 driver_clock_probe_verification.json。接口未保证采样新鲜度，当前测量也不证明历史频率，保持未资格化；累计观测现为 37 次管线加 6 次独立诊断。
- **尾部同步**：两条未归属设备同步晚于最后 serving 观测，无模板引用或外部后继，已证明不影响当前固定资产的保留结构。源码支持 profiler 停止会同步的解释，但历史未记录 torch_npu git 提交；不建立通用忽略规则。证据 s3-host-wait-audit/device_sync_scope.json。
- **复制归属**：124 条类型冲突和 10 条无硬件匹配已恢复 CPU 归属；完整同线程序列每 rank 83 次，已有 32 条硬件锚点一致。正式重提取后图不变，24 个模板仅增加原生成员。真实框架诊断复现普通 Host 内存返回前一计算任务身份；pinned 复制的 PCIE_DMA_SQE 分类已修正。证据 s3-memcpy-cpu-association/{verification,postprocess_verification}.json 及 s3-host-wait-audit/framework_copy。
- **复制完成与成本**：134 条调用都完整对应一个 CPU 服务片段，且共享返回边界，工作没有丢失。官方接口对非锁页/锁页内存分别规定返回时已完成/仅已提交；历史缺少逐次源内存属性，当前 pin 策略不能替代历史事实。现有片段没有显式 host-wait 分割，不证明内部没有等待，也不授予纯 CPU 成本资格。证据 s3-memcpy-cpu-association/{cpu_coverage,completion_contract}.json。

补充取证：未配对 HostToDevice flow 的起点是 rank 1/thread 1102 的 StreamWaitEvent（CPU interval 657852）；同线程原生/profiler 均 3900 次调用，已有 3899 条 CPU 关联逐序一致，唯一缺失对应 native index 20757。两项缺口指向同一等待调用。归一化现复用按 API 类型分组的 CPU 序列关联，恢复其 source_interval、owner_call、scope_call；正式重提取后源图不变，一个模板只增加该调用成员。仍缺设备完成端点，原缺口未关闭；验证见 s3-stream-wait-cpu-association/verification.json。未归属 CPU 分类另见 unowned_inventory.json，不能仅按事件名称排除。 事件生命周期已定位唯一 Record 代次，但历史缺少创建类型和等待时的事件状态。首次 default 事件诊断返回 207000 后停止；按已查框架 ACL_EVENT_SYNC 模式另做一次 45 秒上限、固定复制量的诊断，用时 1.794320040 秒并成功退出：第一次等待前 recorded_status=0（NOT_READY），返回任务身份由生产流改为消费流；同步后 recorded_status=1（COMPLETE），再次等待返回身份不变。此处枚举为 aclrtEventRecordedStatus，不能使用另一 aclrtEventStatus 的数值含义。结论仅是返回身份不保证每次成功等待都有新任务；未启用硬件 profiler，不能证明历史设备工作可省略。证据 unpaired_wait_lifecycle.json、runtime_event_sync_verification.json；累计独立诊断为 5 次，无重试或参数扫描。证据 s3-host-wait-audit/{unpaired_flow,stream_wait_sequence}.json。

版本来源已补记录合同：SGLang base/采样及 CPU、通信 worker 共用 RuntimeEnvironment，记录包版本和声明 CANN 安装的 runtime/HCCL 版本文件。实际两容器读取和五文件编译检查通过，见 s3-runtime-provenance/verification.json；没有运行模型或回填历史资产。后续检查原始文件纠正了版本缺失判断：当前固定源资产的两个 profiler_info 均记录 CANN 9.0.0、torch_npu 2.10.0；环境 JSON 没有该字段不等于整个资产没有记录。起止锚点及路径见 s3-host-wait-audit/historical_clock_metadata.json，未拟合时间修正或回填源资产，仍不替代已加载库/完成语义证据。

关闭条件：取得必要的完成/等待证据，核对提交前沿及时钟转换，按保留结构区分真正依赖缺口、范围外观测和 S5 成本事项；继续保留未证实的设备关联缺口，不把 API 名、无新任务 ID 或已有 CPU 片段当作完成证明。完成资格后再替换固定 manifest 状态，不提前标 ready。

### 2. Decode 发布成员的 CPU 目标工作交接已修正

位置：TemplateInsertedNode、TemplateBank::plan_publication_members、TemplateInstance 与 ExecutionGraph::Node。替换节点记录接收槽位，保留节点绑定原位置对应的目标槽位；两者引用实例内唯一 PublicationWork 的成员描述，来源 span 不再承担目标成员身份。共享输出和逐成员服务分别呈现，给定耗时路径保持。

关闭证据：s4-publication-cpu-work/verification.json。两配置完整 45/50 阶段，108 个成员 CPU 服务逐一核对目标请求、生成进度和输出分支，其中 16 个 donor/target 槽位不同；删除新增描述字段后两张物理图与修改前完全相同，metrics 及阶段报告相同。此项关闭已发现的 Decode 成员重绑定缺口，不声明所有 CPU 阶段已有完整成本特征；其余 CPU 工作边界及成本资格在 S5 前继续核对。

### 3. 独立结构验收与支持域仍不完整

位置：TemplateBank 候选选择、running update/transition 规划和生产参数域。

同源四步骤与全部 rank 仍联合选择。已验证成员分支重绑定和固定人数部分过滤，未授予任意人数变化、周期输出、可选载荷或过滤序列重定位。冷Prefill长度候选已支持固定观测人数、每成员冷单页；两请求127/65开发输入完整8/12阶段，其他人数和实现分支仍须独立资格。Idle 新撤销只删除已资格化 CPU 区域，目标必须无 running update，外部端口保持。

新增证据：s4-independent-arrivals 的两组未参与实现的序列在两配置、两种给定耗时下均完整终止；目标请求合同与物理时序核对通过，completion-mix 调度顺序随耗时改变。不过所有模型计算批次均为单请求，没有授予多请求合批或原生 kernel 结构资格。

s4-independent-merge 首次检查在 100 ns 过滤后立即完成时失败，200 ns 通过。实际资源切口表明，来源已有设备过滤和完成释放两种分支，组合受新增资源限制。当前共同区域替换已引入所需接口，复用同一资产使该输入完整终止，详见 s4-resource-introduction；输入因用于修正已转为开发证据，不能继续计作独立通过。共享输出人数校验未删除，周期输出及其他未资格化组合仍拒绝。

新增独立证据 s4-independent-resource-sequence：两种耗时、两种准入均完整终止（28/33 阶段），实际包含双请求 Decode 和部分完成过滤，目标参数与图时序自洽。均未触发新增资源接口，不据此关闭该分支独立资格。

关闭条件：补齐新增资源接口的独立验证，并用独立原生目标核对关键结构；范围外拒绝沿用已有证据。不能仅以开发输入 complete、正确拒绝或更多模板数量宣称泛化完成。

### 4. 装配和等待仍有跨来源限制

位置：TemplateAssembly::connect、SimulationRuntime::accept、ExecutionGraph::append/schedule。

connect 更新资源前沿后再追加和调度，失败终止候选，尚无提交后重选或回滚。CPU 连接含来源时间投影。撤销保留端点与纯依赖，不能将未匹配等待迁移到 CPU 入口或保留零耗时旧任务。插入端点在实例内重定位，来源数据保持只读。

新增资源的局部 lane/边界、入口/出口及资源前沿已落实。切口外已有同资源工作、或同一实例多个独立切口重复引入同一资源时拒绝；没有已知前沿的新资源从初始状态进入。按调用 span 的实际顺序连接前后工作和后续等待，不按 WorkId 数值或 donor 时间排序。100 ns 物理图中 Schedule work 21 的两个 rank 各有前序 work 20 接入，并连接 work 22/23/24；14 个实际 Index kernel 均为 2→1、保留 [0]。

本轮发现的后续逐节点连边膨胀已整改：按资源和调用，依据既有依赖只连接最早的不可比较入口，与出口共用可达前沿裁剪。开发输入删除 32904 条物理边（1243114→1210210），无新增边；逐条核对被删边在新图中仍有路径，全部节点、边界完成时间、端口和指标保持。准入 1 的整图保持。证据 s4-resource-entry-frontier/verification.json。此项关闭已发现的后续连边膨胀，不宣称所有前序/等待依赖已最小化。

剩余关闭条件：补齐真实资源/等待接口语义核对，再进行新输入及原生结构验收。保持时间和可达性证明本次裁剪等价，不证明原装配语义已经完整正确；跨来源组合和无回滚限制继续保留。

### 5. 已发现的来源提取与采样配方耦合已关闭

来源提取的直接耦合已关闭：SourceExtractor 和共同 BlockExtractor 消费 SourceBindings，不再包含 SGLang 类；SGLangBlockExtractor 端口解释移入 production/sglang，ExtractionWorkflow 负责显式选择绑定。复用原事实完整重提取后 source_graph/templates 逐字节一致，除输入引用路径外的 manifest 字段全部一致，readiness 和原缺口保持。证据 s3-source-bindings/verification.json；不是只移动目录或仅编译通过。

采样与通信配方已移入 production/sglang/SGLangSamplingRecipes；Planner 只分派框架、封装公共权限及输出。历史职责迁移证据见s3-sampling-bindings/verification.json。默认组合采样网格已删除，当前仅保留显式CPU/device需求与独立通信资产配方，验证见上文集合入口迁移；不继续沿用历史7项默认条件。只准备输入，未执行采集或成本计算。

此项关闭审查中定位的两处职责耦合；显式框架选择仍归工作流。现有采样设施不因此获得 S5 节点成本资格，也不声明 KT 或其他框架的模板接入已验收。新框架仍须按同一生产合同交付并验证实际产物。

### 6. 最终输入与模板规模尚未收口

公开集合输入及组合条件执行已接入，模板族归并、容量/内存临界与采集开销分项仍未完成。旧单workload初次采集回路已删除：代表性建库规划未实现时，predict明确报告规划缺口，不按目标request补采；profile保留为显式诊断。恢复仍先核对真实进程再改状态。

关闭条件：完成集合上的代表性建库、多来源语义覆盖与依赖重算；按实际等价关系归并定义并报告实例、定义和开销，不能靠选择少数通过配置缩减原目标。

### 7. 成本模型与正式性能闭环尚未开始验收

执行核仅给 Service 加显式耗时，等待/annotation 只提供依赖，缺值拒绝。Published 推进 token 和资源；Idle 只使用 Selected，无请求 token 推进。现有条件化时间不是准确性能预测。

关闭条件：S3/S4 逐项验收后，按目标节点/工作描述接入独立成本模型及集合探索，预测与原生评价隔离。不能用拟合吸收结构、时钟或插桩开销缺口。

## 2026-10-12 周期审查：恢复边界与集合迁移

本轮沿公开脚本、ProjectReader/Planner、Python预测与采集恢复、提取/编译资产、TemplateBank消费、控制器、执行图和耗时提供者审查职责与状态交接。检查的是当前主线链路，不声称静态检查证明所有原生行为或任意配置正确。

| 范围 | 当前结论与后续 |
| --- | --- |
| 输入及结果 | 已消除全局framework/environment、单requests和base/targets；新项目与快照按配置、workload独立保存。集合入口已接入，代表性采集与受影响组合重算仍待实现。 |
| 模板生产与消费 | ExtractionWorkflow选择生产绑定并发布编译资产；预测只消费明确编译资产。来源适用性已与比较候选分开；多来源组合、模板族及轻量采集开销仍未验收。 |
| 控制与耗时 | 控制器维护请求/KV，端口完成驱动反馈；ExecutionGraph只给Service使用NodeDurations，等待与注解依赖连边。未引入真实成本拟合，现有完整图回归不能替代S5。 |
| 预测恢复（已修复） | prediction.py原resume只检查原生采集是否存活，宿主中断又将predicting覆盖为interrupted，可能修改正在使用的输入或另开轮次。现在检查实际预测容器，活跃时不修改资产/预算；外层异常保留持久处理阶段，让后续resume接收同轮结果。 |
| 文档 | 合并三个边界整改的重复叙述，标明默认采样网格已删除，统一下一步顺序；开发说明和闭环合同补充宿主中断与容器存活的区别。 |

恢复证据：s6-prediction-host-recovery/verification.json。真实公开predict启动后暂停其CPU容器并中断宿主，活跃resume的预算修改被拒绝且输入/预算/轮次不变；恢复容器完成后，同轮结果被接收，重复resume不新增轮次也不改写原结果。两个候选complete、仅一轮、零采集attempt。容器暂停仅用于验证，已恢复并正常退出；没有新建test工程或修改C++行为。此证据不覆盖尚未实现的集合级恢复与失败隔离，不勾选S6整体验收。

## 集合合同切换验证

解析、prepare快照、隔离挂载、组合报告及resume已使用新合同。公开s6-exploration-set-contract/run记录包含4配置×2workload：4个complete、2个控制路径unsupported、2个部署来源needs_data。原workload的指标、图摘要和执行摘要与迁移前一致；新workload只整体平移到达1 ms，所有请求与端口绝对完成时间随之平移，相对指标保持，验证组合状态隔离，不冒充新结构分支泛化。

同一次运行验证宿主中断、活跃预算修订拒绝、完成后接收同轮结果与重复resume不新增轮次。另一任务验证允许采集但缺来源时返回代表性规划未实现，显式导入模板列表后缺口转为node_durations_missing；两个任务均零采集attempt。诊断profile的SGLang/KT禁采、显式sample、选定validation输入、旧合同拒绝与双框架快照均通过。证据目录中的verification、recovery_live、entry_verification、asset_resume_verification及plan文件。全新Docker Release构建通过，纯导入运行不挂载未被节点成本消费的旧CPU测量文件。

这些结果交付集合入口的当前条件执行能力。自动代表性建库、按语义覆盖联合选择多来源、仅重算受影响组合、完整失败矩阵及S3–S6终止验收仍待完成；旧单workload初次采集回路已完整删除，不作为兼容后端保留。

## 代表性配置分组与请求池来源筛选

SGLangCollectionPlanner位于生产端，由plan/predict共用。当前配置关系内按部署分组，每组选择最大请求池，平局按声明顺序；控制路径、框架或设备数不符明确排除。分组不读取目标workload，也不发起采集，报告requires_mechanism_coverage。8配置验证形成3组、排除3项；小容量先声明仍选择后续大容量，增加workload不增加组，plan不创建输出目录。证据s3-representative-configurations/verification.json及对应输入/计划。

资产选择曾仅按配置关系和耗时是否存在挑选来源，忽略模板已有请求池适用域。现由TemplateBank.qualify统一检查来源身份缺口、准入策略和RequestPoolCapacityDomain，选择与实例化共用；来源静态不合格时先于耗时优先级排除，不复制规则或新增变换。真实已有来源无给定耗时时，容量内返回node_durations_missing，容量增加先返回request_pool_capacity_increase_not_supported；公开预测零采集，证据capacity_verification.json。该项不等于完整多来源语义覆盖选择；代表短请求、既有事实复用及有界建库仍待实现。

## 结构来源与耗时输入失败隔离

PredictionWorkflow原先在同一资产加载异常路径中处理NodeDurations，耗时单位/来源等输入错误会丢弃已加载结构，随后误报compatible_template_missing，误导结构建库需求。现保留合法TemplateBank，单独记录duration_error；相关组合返回failed/node_durations，其他来源仍参与既有选择，容量不支持仍先拒绝。没有将错误耗时当作缺失值或零值，也不新增成本模型。

Docker Release构建及公开predict通过预期失败路径验证：允许base采集时，1份真实结构保留loaded和122实例，2组合准确报告错误单位、另2组合报告容量不支持，无compatible_template_missing、无代表性采集缺口标签、无采集attempt。证据data/redesign/s3-cost-input-isolation/verification.json。此验证针对解析失败隔离，不代替完整成本或多来源联合消费验收。

## 绕过批人数资格的诊断规划

instantiate_step要求来源与目标请求数相同；但SimulationRuntime诊断曾resize(1)并直接调用公开plan_materializations和parameterize_insertions，跳过完整批次、请求数与状态资格。这使logits/Decode物化的跨人数改写只在局部预览中存在，文档却含有“已接入”的当前态描述。删除single_request_topology输出、专属logits donor查找/替换和只供该路径的CPU view移植分支；删除正式规划内不可达的跨人数物化分支。底层规划和插入参数方法改为TemplateBank私有，正式实例选择成为唯一入口。源logits识别、RoPE单操作切口诊断和实际批次/区域替换继续有消费者。

Docker Release构建通过，固定真实资产和给定耗时的两组合公开回归均complete；全部非provenance结果字段与先前集合合同结果逐项相同，包括指标、结构摘要及执行摘要，来源manifest和耗时描述一致。未比较完整序列化图，诊断输出删除以消费者扫描和编译检查确认。证据data/redesign/s4-qualified-planner-boundary/verification.json，零采集attempt。批人数的后续整体变换边界已集中到模板关系文档，不能通过恢复局部预览代替实现。

## 2026-10-12：集合迁移后的周期审查

本次覆盖最近五批实质修改，从ProjectReader/Planner、模板生产和加载、控制/执行、成本边界到Python持久化与结果发布进行静态审查，并用真实公开入口核对本次发现。不是完整结构或性能验收。

| 职责链 | 当前发现与处理 |
| --- | --- |
| 输入与快照 | contracts/project.cpp拒绝旧字段、重复配置/workload/manifest；Planner交接集合快照，Python按组合输入挂载。resume资产修订仍先持久化再由C++完整解析，非法修订可使该轮失败；尚未实现修订前完整校验，不能宣称输入修订原子性已验收 |
| 生产与消费 | SGLangTemplateBindings.produce资格化部署、原生策略与worker controls；TemplateBank消费编译资产。本次审查发现templates.hpp包含production/source_extractor.hpp以获取观测类型；后续已将共同观测值移入contracts/source_observations，NativeRegionBinding.read保留在production，关闭该头文件依赖。原生元数据的完整类型化仍是独立事项 |
| 控制与执行 | LightweightController持有请求/KV及发布状态；SimulationRuntime维护时钟、同刻事件和物理完成。批次按所有阶段/rank资格化后装配，底层规划已私有；跨人数、内存回退及其他明确拒绝域未新增支持 |
| 来源与成本 | 来源静态资格和耗时优先级分开；非法NodeDurations不再丢弃结构。仍为单来源选择，不能用静态available或有耗时证明全部语义覆盖 |
| 缺口发布 | 确认并修复prediction.py::_publish将全局采集原因附到所有needs_data组合的问题；现仅标记compatible_template_missing的组合 |
| 恢复与文档 | 活跃容器阻止修订、已完成轮次复用保持既有行为。本次未重复恢复实验。修订后仍全量重算，计划误勾项改回未完成；删除自动初次采集仍有效的失效描述，压缩计划重复历史 |

本次缺口验证见data/redesign/s6-combination-gap-isolation/verification.json：真实编译来源、两个配置、同一workload，在允许base采集时一个组合仅node_durations_missing，另一个compatible_template_missing附加代表规划原因；与C++原始结果逐项核对，前者完全不变，零attempt。Python语法及diff检查通过；C++未改，不重复构建。计划当前状态段以最新编译资产和集合输入为准，详细历史转为已有审查/执行记录引用，阶段门槛保留。

## 共同观测合同与生产解析分离

SourceRequest、SourceTensorLayout和SourceNativeRegion迁入contracts/source_observations，共同值的读写在合同实现中；依赖NativeRegionBinding的原生模块/方法校验与字段映射改为production/block_binding.cpp中的NativeRegionBinding.read。SourceExtractor和SGLang模板绑定复用该方法，核心值不声明生产接口。templates.hpp显式引用观测合同和source_graph，四个布局生产实现也直接引用所需数据；删除原定义与解析入口，不保留转发。公共合同/图/模板/控制/执行/性能目录不再直接包含production头，也无SourceExtractor、SourceBindings或NativeRegionBinding使用；该扫描只证明这项依赖边界，不代替完整语义验收。

Docker Release构建通过。复用原始事实在独立s3-observation-contract目录公开extract：source_graph.json与templates.json逐字节相同；CBOR只规范化两条明确的来源路径后整体字节一致，未改资产格式，readiness仍needs_data。公开predict的4组合来源报告除路径外与原报告相同，组合结果不变、零attempt；本次没有运行给定耗时循环。证据production_verification.json及consumer_verification.json。原资产保留，不新增原生采集或拟合。

## 2026-10-12：长度候选域替换后的周期审查

本窗口包括共同观测合同、长度缺口分类、跨调用发布成员、共享序列容量及Decode容量候选五批代码修改。沿ProjectReader、来源生产/codec、资产选择、Controller/Runtime、模板实例化、NodeDurations和Python发布/恢复复核；核心目录对production的依赖扫描无命中，不能据此宣称所有语义已解耦。

- 容量只有TemplateSource一份，原生模型、请求表、RoPE与逻辑KV解释留在生产端；codec显式保存，整组实例化先检查容量再选来源。supports_configuration_change目前只允许请求池人数与prefill预算变化，max_total_tokens不能变化，因此当前来源容量不会被这个配置变换绕过。
- Decode候选不再取观测最大长度；保持人数、单query和所有设备参数绑定门槛，随后仍走相同的页表/KV/发布/端口及跨rank资格。没有新增逐长度映射或第二套控制器。固定来源的7批开发案例及两配置回归证明此次修改的具体路径；跨下一页和完整范围的独立原生对照尚未完成。
- 原native_environment只单独记录torch_npu及CANN runtime/HCCL，没有ATB修订。此次ATB源码与二进制审阅来自当前安装容器，不能倒写成原采集时已记录的事实。后续记录合同已由RuntimeEnvironment补齐ATB_HOME_PATH对应安装的version.info路径与原文；SGLang/KT两个容器实际读取核对通过，见s3-atb-provenance/verification.json。只关闭未来记录缺口，历史身份及worker实际加载库仍未被证明；未重新采集或回填历史。
- 编译资产仍约142 MB，重编译阶段约59秒，源图/模板提取另有开销；新域没有增加模板，但轻量建库、模板族归并与多来源语义覆盖仍未完成。单来源按配置选择、资产修订全量重算和修订前完整校验问题沿用上节记录，不重复标记关闭。

当前开发证据为s4-decode-capacity-domain的verification.json与regression_verification.json。使用已有采集和明确给定耗时；图核对了每rank/每Decode的64个attention节点、目标长度、最终KV释放及依赖时间，回归组合字段在资产路径规范化后保持。不是成本精度、全部支持域或完整恢复矩阵验收。更新计划当前资产及长度合同，删除仍以131为当前上限的失效表述；无新增原生采集或test工程。

## 后续顺序

集合输入已切换，接着核对后续页边界并收口批人数/模板族与机制覆盖，再完成代表性建库、多来源语义选择和有界开销，使用固定规则及未参与修改的输入做S3/S4整体验收。局部结构证据继续复用，未资格化接口与来源时钟分别按结构/成本门槛处理；满足S3/S4后进入节点成本模型。不得把条件展开或个别区域复用当作整个支持域完成。S3–S6未完成，不生成最终implementation_log总结。


## 2026-10-12：周期输出至有序循环的周期审查

五个实质批次（周期输出、ATB来源记录、来源切口/诊断开关、独立诊断产物、有序CPU循环）后，核对公开predict/resume → PredictionWorkflow → SimulationRuntime → TemplateBank → TemplateTopology/ExecutionGraph及生产绑定、来源记录和当前文档。以下是本窗口结论，不重置此前未关闭项。

- 周期算术由publication_parameters维护，工作进度仍来自唯一控制状态；原生首次输出事实与推导的后续增量关系区分清楚。131批开发证据不外推全容量或成本精度。
- region_sequence.cpp只消费通用区域/端口/资源和值引用，不读取框架名或原生回调。借用的定义归TemplateBank持有；新边界按计划再按实例重定位，成员槽位与原生request身份分开。CPU桥接保持服务成本，端点/外部保留工作通过既有region_ports检查；计划由apply_operations统一原子提交。已核对正式两配置的8个循环改写，范围见模板关系。
- 方法能够组织给定成员序列，但公开人数门槛和固定请求数输出资格未变。成员数量变化、Prefill设备循环及模型/调度关联没有验收；不能把本轮CPU序列拼接标为跨人数完成。局部donor候选仍先经过原区域接口资格，再进行整体CPU循环资格，未开绕过路径。
- 报告局部缓存不进入执行状态，关闭diagnostics直接跳过明细；独立明细和图以引用交给Python编排，重复kernel/communication数组及无用开关已删除。实际明细/物理图等价、status/resume复用有证据；明细内部来源重复和结构资产轻量化仍未关闭。ATB记录只证明当前安装声明，不补历史或冒充worker加载身份。
- 可扩展性债务：execution_graph.cpp的schedule每次重新遍历累计nodes/edges并重置全部completion_ns；当前有限输入正确性回归不能证明长workload的处理开销可接受。此处是明确的累计重算，不是此次报告缓存已经解决的问题；后续需按增量依赖和唯一时钟收敛，禁止另建第二套执行器。

本次未发现需要放宽支持门槛才能运行的情况。核心目录依赖搜索无production/框架名称新增，正式CMake包含新序列实现；没有test工程、兼容分支、硬编码拟合或新增采集。引用、直接include、逻辑分段及diff检查通过；未声称静态检查覆盖所有历史未决项。下一步仍是完整批人数关系、代表性覆盖与轻量建库，S5后置。


## 2026-10-12：logits、成员循环与初始化交接的周期审查

本窗口五批代码为logits分支/CPU donor、多资源Publish循环、成员donor资格、last_location插桩、Prepare共同序列。核读ProjectReader与Python预测/恢复、PredictionWorkflow资产加载/选择、生产绑定和codec、TemplateBank选择/参数化/原子计划、Controller进度、TemplateAssembly等待连接、ExecutionGraph及NodeDurations；本节不替代已有未关闭项。

- 框架只在生产端解释last_location、冷prefix和请求身份。共同序列接收显式成员索引，保留真实父迭代服务和单独的线程/流/设备完成前沿。插入槽位同时服务批参数和KV成员参数；执行图只在publication_work存在时附加发布描述，不向Prepare借用发布状态。源码依赖扫描未发现controller/execution/performance新增production引用或框架名称，正式CMake已包含region_sequence。
- 生命周期仍由TemplateBank持有定义；计划内新端点在apply_operations中重新定位，完整计划先暂存后提交；实际图失败仍终止组合，没有未经回滚的重选。collect_stream_waits从保留和插入的HostWait建立等待，避免继续引用被删除的源等待。初始化资格曾错误复用排除HostWait的is_cpu_region，已由真实节点证据纠正为资源端口资格；不能由单CPU端口推断无等待。
- 正式instantiate_step先限制整批人数，因此初始化增减、logits跨人数CPU donor及跨人数发布成员路径尚未运行验收。模板metadata/codec成功不等于变换成功。后续须联合准备操作、模型连续性分支、调度/共享工作资格，不能删掉人数检查再以逐个失败补丁代替完整规则。
- 本窗口确认多来源编号与资源对应存在缺口；来源身份的后续整改及仍未关闭的资源问题统一见下节，不以历史实现描述当前状态。
- 输入集合、单一控制进度和给定耗时合同未变。Python的资产修订仍全量重算且未完成完整发布事务；执行图仍每批重算累计图，明细仍重复来源；这些既有S6/规模债务未关闭。本次局部静态审查不证明全恢复矩阵、全支持域或性能精度。

压缩模板关系中初始化插桩与历史失败的重复叙述，当前合同保留机制、证据路径和未验收范围，历史过程留在执行日志。没有新增采集、test工程、兼容分支或拟合；人数资格保持，S3/S4整体及S5/S6仍未完成。完整主资产回归结果由本轮执行记录补充，不能仅以构建成功作为行为证据。

## 2026-10-12：目标工作量、重编译与来源身份的周期审查

本窗口五批为目标发布成员、目标传输规模、显式模板重编译、节点来源、边界来源。复核公开入口、ExtractionWorkflow、生产绑定/codec、PredictionWorkflow、TemplateBank/Assembly、SimulationRuntime/ExecutionGraph、NodeDurations和Python预测恢复。当前证据为 `data/redesign/s4-source-boundary-identity/verification.json`；下列结论不代替完整S3–S6验收。

- **工作量与结构资格分离**：PublicationParameterLayout按目标请求形成工作描述，TransferParameterLayout的请求元素/输入token关系按目标求值；固定分支和释放成员仍有独立资格。整批人数门槛尚在，不能把局部参数关系完成标为跨人数公开验证。共享CPU服务没有按成员数私自缩放成本。
- **生产与消费职责**：compile-templates复用已保存事实、图和模板，只重做生产绑定与编译资产；extract仍完整提取。编译前撤销旧资产引用，文件写入后rename发布，失败不将旧文件宣称为新结果。没有自动格式探测、缓存或兼容路径；这只保证单次文件发布，不证明并发写入、整个资产修订事务或掉电恢复。
- **来源及生命周期**：TemplateBank加载后给不可变定义绑定manifest；Assembly端点与CPU顺序投影按来源限定，ExecutionGraph节点/原始边界按来源共享。插入节点与边界保留donor来源，但目标端点不共享。string_view和路径指针借用bank内定义，PredictionWorkflow的unique_ptr所有权覆盖组合运行；未引入全局注册或第二份控制状态。成本只查节点实际来源，缺口带manifest，不跨来源按同号span取值。
- **当时的高优先级缺口**：跨采集资源角色、来源候选与成本集合尚未接通。后续已在下面“资源、等待与成员序列”窗口实现并验证；原生批次与rank的主体来源资格继续保留。
- **既有债务保留**：Python `_revise_assets`仍先写项目再写状态，修订后全量重算；ExecutionGraph::schedule仍重算累计图。代表性自动建库、模板族与存储规模、完整人数变换、固定规则独立验收尚未完成，S5不提前启动。核心controller/execution/performance未发现新增框架判断；此静态结论不证明跨框架接入已通过。

Docker构建通过，正式predict的两配置均complete。逐项确认新增来源字段后，完整物理图、端口摘要、指标与缺口和改动前一致，所有边时间有序；该证据仅覆盖单来源回归，不证明跨来源组合或跨人数变换。此次无新增原生采集，累计55次；无test工程、.clang-format或third_party修改。来源组合稳定合同集中在采集设计，历史审查的过时实现说明改为引用本节。

## 2026-10-12：资源、等待与成员序列的周期审查

本窗口五批为资源角色、按来源耗时集合、多来源选择、同批等待和CPU separator子图。审阅公开predict/compile-templates、生产绑定与codec、TemplateBank/Assembly、成员变换、ExecutionGraph/NodeDurations及Python恢复路径；运行证据见执行记录和 `s4-cpu-separator`，本节不授予完整S3–S6资格。

- **资源与来源已贯通。** `production/sglang/template_production.cpp`的SGLangResourceBindings依据入口、队列、通信提交和事件关系绑定角色；核心仍保留原生身份用于追溯。`TemplateBank::bind_sources`要求共同原生条件，整批主体按完整阶段/rank组合选择，局部替换另查资源角色。PredictionWorkflow持有全部bank及共享只读耗时集合，donor指针和来源视图覆盖组合生命周期。不同配置的组合串行重新绑定候选，每组合重新创建controller/runtime，没有复制KV状态或新增框架分支。
- **等待只有一个跨调用装配路径。** `templates.cpp`的collect_stream_waits及Assembly同时处理原生关系两端；源invocation顺序只资格化来源，目标按实际动作顺序查询此前工作。保留同调用内部的等待和区域完成端口，当前调用/未来调用不进入前沿。旧added_resource_ports专用补边已删除。source_invocation_order编译字段不兼容旧批次序号，旧资产需正式重编译；未引入格式探测或版本分支。资格仍限已支持non-overlap路径。
- **序列公共表达整改。** `region_sequence.cpp::cpu_sequence_separator`将单CPU服务假设替换为因果路径上的完整父级CPU子图。边界先由路径确定，服务、注解及连接点一起归入切口，沿现有端口和原子拓扑mutation装配；不靠源时间、片段数或框架名分类。新增检查分别对应真实的资源边界、父级所有权及切口合法性；没有独立规划后端或采集触发。当前正式发布消费者可回归；三服务准入separator的实际消费仍须随下一项接入，不能以静态算法或来源清点宣称已验证。
- **下一项必须共同收口。** `template_bindings.cpp::bind_admission_work`仍只给定人数的CPU准入资格，未将budget区域按接纳事实绑定成目标成员；共享队列整理、尾部统计须保持一次。`instantiate_step`的人数门槛还保护后续按来源长度索引目标请求的代码，`RequestLengthDomain::accepts`也明确固定人数。移除门槛前须一起改为目标成员合同，并检查Prepare初始化、Cat操作数、模型视图/物化及Publish成员；不能只改一处比较后逐例补洞。
- **成本与闭环没有提前冒充完成。** NodeDurations按manifest/span查询明确给定值，等待不计服务，source发布前完成文件校验；无拟合或未知填零。Python PredictionWorkflow._continue对缺资产仍明确报告代表采集规划未实现，不存在遇到结构Rule缺口就循环补采的路径。S5继续后置。
- **既有工程债务保留。** `_revise_assets`仍先写project再写run状态，且资产修订全量重算；ExecutionGraph::schedule仍遍历累计图。模板族归并、存储/后处理开销、有界代表建库、固定规则独立验收和完整恢复矩阵均未关闭。它们进入对应阶段，不通过增加内部版本、缓存平台或额外状态账本绕过。

本次顺手合并日志中已被实现结论替代的等待定位与separator定位叙述，保留证据索引及适用域。未改.clang-format、third_party或主分支，未新增仓库test工程；无新增原生采集，累计55次。

本窗口最终公开回归见 `s4-cpu-separator/verification.json`：多来源two-step/staged和原序列两配置的完整物理图、指标及缺口均与s4-invocation-waits一致。此项覆盖当前正式发布序列路径，不替代准入多服务separator的目标执行验证或跨人数独立验收。

2026-10-12后续整改：此前“联合处理人数入口、长度域与来源槽位读取”的要求已落实到正式选择器，冷Prefill 2→1已通过公开整批展开及成员/共享工作核对，证据见s4-cardinality-qualification/verification.json。原有三处来源槽位访问均只在同人数比较时执行；跨人数须有来源参数关系及语义成员切口，再逐操作/阶段/rank资格化。此项关闭入口与索引耦合问题，不关闭人数增长、反向物化、代表建库和独立结构验收等未决项；本段是整改跟踪，不代替下一次整体审查。

### 2026-10-12 后续整改：输入列表合同与任务身份

BatchParameterLayout 现分别表达行数和输入个数，SGLang绑定同型列表后由公共实例化重建shape/dtype/format，操作分支支持域仍独立；s4-input-arity五组合的完整图与执行明细保持。未授予增长范围或成本资格。

本轮公开并行执行实际暴露 PredictionWorkflow 的全局容器身份缺陷：不同输出目录中的single/regression均生成run-20261012-104008-553756，容器名只取目录basename及round，导致regression启动冲突；status也会看到single的容器。通过docker inspect确认Cmd后没有误恢复。PredictionWorkflow.predict与CollectionWorkflow.profile的任务名现使用时间加UUID，既有任务恢复不重新生成身份，不新增状态副本或兼容分支。新regression公开执行已核对UUID、容器Cmd绑定及exit0；采集入口身份生成只做语法/静态检查，本轮未采集。旧失败记录保留，不对该冲突任务执行resume。此为发现后的局部整改，非新一轮整体审查；S6其他恢复矩阵仍未完成。

### 2026-10-12 第五批整体审查：人数域与完整消费链

本轮从PredictionWorkflow的输入/来源资格，贯穿SGLang绑定、TemplateBank整批选择/成员重写、SimulationRuntime与ExecutionGraph，再回到Python集合发布/恢复及CMake进行检查；不是只看本轮Cat常量。以下结论区分静态边界和公开运行证据。

- **结构规则与参数分开**：BatchInputArity只重建同型输入列表，producer的BatchOperationVariant决定单DMA或单ConcatD。32来自公开ACLNN分组机制，并由当前安装环境32/33输入的一/二设备任务边界核对；常量只在production/sglang。核心controller/execution/performance未新增算子名或框架分支。来源CPU服务仍按原生操作切口处理，输入列表增大不证明CPU耗时等价，S5须消费目标工作量。
- **完整资格未绕过**：PredictionWorkflow在加入donors前调用TemplateBank.qualify，已验证admission_policy才进入bind_sources，不存在这条正式路径对失败expected的未检查解引用。多来源仍要求共同原生配置/部署，整批所有阶段/rank同一主体；跨人数仍要求length_domain、准入/初始化/发布成员合同。region_sequence重复成员时重绑request_slot，保留完整separator和资源端口，不重写源定义。
- **该审查时的人数增长限制（后续整改见下节）**：当时两资产公开request_pool_domain.maximum均为2。池容量增加由RequestPoolCapacityDomain拒绝，不能用Cat[2,32]绕过；代表性配置应先按探索集合选择最大池。Decode的FinishedSequenceTokens传输还要求来源request_count和finished_count，不能当任意成员释放已支持。单请求主体新增准入成员/反向设备物化以及完整资源页关系继续保留资格边界。
- **执行与成本职责**：controller中的Progress持有请求槽和KV页，runtime消费实际完成事件推进唯一时钟。ExecutionGraph仅对Service取按来源限定的给定耗时，等待/注解只贡献依赖，缺耗时返回缺口；此轮算子诊断耗时未导入。schedule仍累计重建约束，模板/诊断体积也未达到轻量验收，继续列为既有债务。
- **入口和恢复**：新的独立任务用时间加UUID避免跨输出目录同名容器，恢复沿用任务目录。_continue仍明确返回representative_collection_planning_not_implemented，不因结构缺口重采。_revise_assets先写project再写run的非事务窗口、全组合重算和完整失败恢复矩阵仍未收口；未因新任务名宣布S6完成。
- **工程与文档**：根CMake维持C++23/关闭扩展，无新增test target或库；新参数在唯一codec读写，不保留旧资产兼容。此次观察脚本与核对脚本只在/tmp，原始观测和引用源码位于data证据目录，不修改third_party或.clang-format。已替换模板合同中“Cat只支持来源人数”的失效描述，来源域、规则域和整批支持域分别交代；当前状态以执行计划为准，里程碑证据留执行日志。

下一步先联合收口增长方向的成员/共享资源合同与代表性最大池建库方案，再固定资产做独立结构验收；不能把局部节点范围扩大算成S3/S4完成。模板归并与开销、代表性自动建库、独立结构、S5和S6仍为未完成项。本轮图保持验证结果记录在执行日志及s4-concat-domain/verification.json，不以静态审查代替运行或独立精度。

### 2026-10-12 第五批整体审查：代表配方与完整 Decode 增长

本窗口包括释放传输成员解耦、非空释放组选择、目标释放列表、代表配方报告及分组执行；最后一批同时修正完整展开暴露的全保留更新人数耦合。审阅公开CLI/Planner、Python采集/恢复、SGLang生产绑定、TemplateBank选择与区域替换、控制/执行与按来源耗时路径；运行证据为s3-representative-execution/verification.json，不代替独立原生验收。

- **入口与生命周期**：plan.cpp的prepare_representative只接受实际代表配置；Python workflow.profile复用现有权限、UUID、预算和恢复。capture.py在Engine初始化后解析实际页大小与容量，每组批量提交并等待真实返回。稳定请求身份在worker启动前安装；分组返回只记录batch_response，不授予逐请求完成或成本资格。普通workload和validation采集仍使用原请求到达方式，predict._continue仍不自动采集。
- **确认缺陷已修正**：templates.cpp的matches_running_filter此前对全保留分支也隐含要求相同人数。sglang_inline.py的更新区域在prepare_for_decode前结束；原生filter_batch全保留直接返回。现只对两侧全保留且完整区域为同线程纯CPU的情况允许人数变化，保持目标单token/内存足够与非回退资格，部分过滤规则不变。Docker构建及三/四请求公开全图检查通过；未增加分人数定义、观测或成本倍率。
- **共同合同保持**：prediction_workflow.cpp先qualify再加入donors；bind_sources仍要求共同原生配置、模型、部署和容量。每目标批次的四阶段/两rank同一anchor，局部donor经端口/依赖资格替换。释放区域决定成员域，KvParameterLayout决定目标列表；无框架名称或NPU常量进入controller/execution/performance。控制器仍持有请求/槽位/KV状态，执行核消费节点完成推进模拟；NodeDurations按真实节点来源查值，缺值不填零。本轮全边时序、attention/slot、共享输出及释放核对支持该场景，不证明任意配置。
- **覆盖风险保持开放**：客户端上一组返回不代表原生running batch清空。本次最后一组实际出现旧已完成成员与新组的merge，再过滤；不能把提交顺序当成批次结构保证，也不能凭偶然出现merge授予完整合批覆盖。候选配方的完成条件仍需事实与变换资格共同判断；容量1、周期输出、准入停止、内存压力及域外后端仍未收口。
- **规模与后续优先级**：本次7批次/58实例，编译资产约69.6 MB，全部尝试目录约955.4 MB；预算内不等于轻量验收通过。模板归并和后处理/原生初始化开销拆分仍是S3工作。S5真实模型后置；S6资产修订先写project再写run、全组合重算和恢复矩阵仍待完成。下一步优先形成已有资产的机制覆盖判定，不能按新workload循环补采。

同步精简实施计划的历史人数/释放进展为当前证据，运行说明统一显式配方入口，删除文末重复说明；稳定机制留模板合同。未修改.clang-format、third_party或引入仓库test。累计原生59次，审查计数重置0；S3–S6未完成，implementation_log保持不写。


### 2026-10-12 第五批整体审查：通知排序与设备同步归属

本窗口涵盖来源机制报告、跨人数部分过滤、同刻请求提交/CPU复制参数、通知代次排序及设备同步归属。沿公开入口、Python事实生产/恢复、SGLang模板生产、核心资格/执行和成本输入复核；运行证据分别见s3-notify-dependency-order的重提取/预测对照及s4-device-sync-association/verification.json。本节记录当前结论，逐次核对数字只引用执行日志和资产。

- **已关闭的职责缺陷**：source_extractor.cpp::order_notifications使用已确认设备链与FFTS可达关系，删除task/subtask排序假设；没有在controller/execution/performance新增框架判断。normalization.py::_associate_cpu_api_calls补齐设备同步的CPU归属，复用完整同线程序列资格；不造硬件提交、不将时钟接近当身份。重归一化仅两条SDK记录各新增source_interval/owner/scope，calls、intervals、flows及其余SDK字段保持。validation仍为partial且没有导入模板库。
- **P1，仍阻断结构验收**：SourceExtractor::read_host_waits只组装stream/driver-copy等待。首次四请求Decode的PagedAttention设备级同步现在能归属，但尚无全设备前沿/等待切口；缓冲分配分支的触发机制也未证明。下一步先用现有记录区分同步范围、提交前沿和分配状态，再决定通用合同如何承载；不能按首次Decode或具体人数补特例。TemplateBank::qualify检查来源控制/容量/适用域，manifest中的完整观测缺口仍单独报告，不能把条件执行complete解释为全CPU依赖合格。
- **核心状态与成本边界保持**：ExecutionGraph::schedule拒绝未解析连接和缺耗时，注释/等待不直接计服务，只有Service节点读按来源绑定的NodeDurations。它仍重排累计图；NodeDurations仍是显式给定值，不是S5真实模型。此次全图保持验证支持既有开发场景，不证明设备级同步缺口无影响。
- **入口与恢复债务保持**：prediction.py::_continue仍只模拟并报告代表建库未实现；_revise_assets先写project再写run，且后续重算全部组合。代表机制覆盖、轻量资产和S6受影响组合/恢复矩阵均未完成。生产端拒绝非base角色，Python资产修订也检查角色；此次验证资料没有旁路进入预测。
- **规模及文档**：设备同步关联仅扩充现有API名称关系，未增加wrapper、配置项、兼容层、拟合常数或test工程。清理本报告将历史留出输入仍称为独立证据的失效表述。template_production.cpp的静态limits仍描述旧人数范围，后续应与现有资格报告统一；本次不据此扩展支持域。旧资料中的“下一步”按该节历史时点理解，当前唯一实施状态以执行计划为准。

本轮无新增原生采集，累计61次；第五批审查完成后计数重置0。静态审查与当前真实输入核对均不能代替固定规则独立结构验收，S3–S6保持未完成。
