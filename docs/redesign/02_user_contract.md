# 用户输入与探索合同

正式入口为固定模型 × configurations × request_workloads，不指定base。当前已切换解析、规范化快照、组合执行、挂载、报告和恢复；代表性机制覆盖及采集编排、模板族收口、节点成本模型和最终集合验收仍未完成。SGLang + Qwen3-32B为预测主线，KT保留显式采集，不能因配置可解析而称为可预测。

## 工作负载、配置与适配器

工作负载描述要执行什么：模型和 request 序列。正式输入把它拆为任务内固定的 `model` 与多个 `request_workloads`。框架、并行策略、后端、设备及软件环境属于 `configurations`，描述如何执行；框架可以是探索变量，但不因此成为 request workload 的组成部分。

adapter 是我们理解原生实现的代码，不是用户负载。实际支持单位是“框架及原生版本 × 模型实现 × 后端/并行策略 × 运行环境”的组合。配置可解析、镜像可安装、能够采集、能够模板化、能够预测分别需要依据；任何一项通过都不自动授予下一项资格。当前组合见[框架与环境适配](framework_paths.md#适配组合与源码归属)。本次只明确现有输入语义，不添加另一套 workload 字段、版本路由或用户必填适配器选项。

## 项目输入

示例见[sglang/project.json](../../configs/redesign/sglang/project.json)，命令见[开发说明](development.md)。外部边界拒绝未知字段、错误类型、空集合、重复身份和无效预算。不保留旧framework/environment/workload/base/targets项目字段或兼容解析；历史运行记录只作证据，修改业务输入建立新任务。

| 字段 | 合同 |
| --- | --- |
| model.path | 固定模型路径，全任务共享；改变模型建立新任务 |
| model.tokenizer_path | 默认model.path；token输入不要求加载tokenizer |
| configurations | 非空数组，每项id/framework/environment/config；id唯一，不继承其他配置 |
| configurations[].framework | sglang或ktransformers，属于该配置 |
| configurations[].environment | runtime默认docker；service与框架对应；devices非空、不重复的非负设备编号。每个配置明确部署，不能从别的候选推断 |
| configurations[].config | 完整配置文件路径；参数子集见下文 |
| request_workloads | 非空数组，每项唯一id与请求文件path；请求身份仅在其workload内唯一 |
| simulation | controller默认lightweight，scope默认modeled；其他当前返回unsupported |
| initial_state.cache | 默认cold；warm当前unsupported |
| collection.base / performance | 默认禁止；allow_run及累计max_wall_time_sec。启用时预算须为正，恢复不重置。base在此表示结构资产采集角色，不是用户指定比较基准 |
| assets.templates | 默认空数组，每项manifest及可选node_durations，来源manifest不重复。缺耗时不填零 |
| assets.performance_manifests | 显式独立性能资产列表，默认空；当前节点cost model尚未消费它们 |
| output.directory / diagnostics | 必填输出目录；完整图诊断默认false |

路径相对声明它的文件解析。模型路径可为部署容器路径；CLI项目路径使用仓库路径。prepare-prediction生成全部配置和workload的独立文件，持久任务按数组位置命名快照，不把用户id拼入文件路径。

## 仿真起点：统一预热后开始

项目定义固定预热流程，结构采集与独立目标验证共用；用户无需选择 base 或提供预热请求。模型加载、运行时初始化和预热不计入预测时延，但仍计入采集预算。目标请求的 arrival_offset 从预热结束后的业务窗口解释。

当前 SGLang 主线采用单请求、输入 token `[1]`、生成两个 token、ignore_eos=true、temperature=0，覆盖一次 Prefill 和一次 Decode。配方不读取目标 workload，不以达到最大 workspace 容量为目标，也不保证所有后端分支已预热。适配其他框架/模型时须资格化其具体执行，不默认继承此配方。

当前关闭 radix cache 的主线通过原生请求完成路径释放预热 KV，目标不继承预热内容；保留模型、workspace 等运行时资源，不调用会清空设备分配缓存的通用 flush。资源编号无需恢复成进程启动时的编号。不同配置预热后的容量可以不同，目标执行中仍须模拟分配、扩容及同步。

采集记录实际预热输入、sampling 参数和窗口前资源快照。旧资产不补造预热证据，也不能无条件将来源容量复制给目标。固定配方已接入共同 SGLang 采集/验证入口；C++ 生产端检查实际预热记录并保存 initialization_gap，预测在仿真起点选择时拒绝缺失或不匹配的记录；后续整批及局部子图复用另按共同来源条件、工作绑定与接口资格判断，始终消费目标的容量状态，不继承供体的初始容量；新配方已通过当前 pool-four 开发配置的原生运行和资产交接，证据见 data/redesign/s4-fixed-warmup/native_workspace_verification.json；同配置容量推进和保留区域的allocator分支替换已接入，四请求开发序列已在给定耗时下完成扩容及后续复用；完整替换支持域及跨配置初值适用性仍未完成，改变请求池容量当前返回 workspace_initial_capacity_migration_not_qualified。

## 组合执行与结果

combinations按配置声明顺序 × workload声明顺序输出，每项包含configuration_id、request_workload_id、status、scope、metrics、gaps、assumptions、provenance和collection_usage_ref。预算用量在根collection_usage共享，不按组合重复计费。没有隐含base、base_id/target_id、comparison或speedup；同一workload和scope内的指标才可比较，当前不生成综合排名。

候选状态为complete、needs_data、unsupported、failed。complete仅表示当前scope下条件执行完成，不表示结构泛化或性能精度通过。任务finished可以包含缺口/失败组合；总体status反映这些未完成项。一个组合的执行失败不阻断其他组合。 结构采集缺口仅附加到实际缺少兼容模板的组合；其他组合仅缺耗时或其他数据时保留其自身原因。每个组合重新创建控制器、KV、事件与资源状态，共享资产只读。

模板按自身的来源配置、模型和部署判断适用性，不以某个候选充当来源。当前先检查配置、部署、来源身份缺口、准入策略及请求池容量适用域，再按声明顺序选取兼容的首选来源，优先具备显式给定耗时的来源；同等条件选择第一个。其他来源须与首选来源的模型、原生配置、部署、页大小、序列容量及准入策略一致，才能进入共同候选。每个目标批次按来源顺序选择覆盖全部阶段/rank的原生批次，再按有限规则做局部donor替换；跨来源要求资源角色资格成立。该路径已有局部donor和整批切换的公开运行证据，不授予任意来源或任意配置变换资格。来源已加载不等于目标所需模板齐全，动态展开仍报告实际缺口。assets报告每份来源的加载结果；assets[].source集中保存来源观测和参数绑定，组合provenance.source记录首选来源；structure.graph.sources列出实际图使用的来源，工作明细中的source_manifest限定source_call/source_batch。给定耗时文件与假设由assets[].node_durations提供，不再在组合provenance中重复单一来源耗时描述。source.static_qualification只说明来源身份、准入策略和请求池的静态资格，available不代表全部语义覆盖或成本可用。 显式耗时文件解析或来源/单位校验失败时，结构来源仍为loaded，assets[].node_durations及相关组合单独报告failed/node_durations；不把成本输入错误解释为compatible_template_missing，不据此规划结构采集。

## 采集边界与恢复

常规predict只需探索集、显式资产和采集条件。代表配置及代表request必须由有界建库规划选出，不能直接枚举目标workload重采；预算到限不等于覆盖完成。当前只完成同机制/部署配置分组和最大请求池代表选择，plan/predict在representative_collection报告候选组与排除原因，状态requires_mechanism_coverage。短请求和机制覆盖规划尚未实现：缺可用来源时，禁采返回base_collection_disabled；允许采集仍返回representative_collection_planning_not_implemented，不启动目标request的冒充建库循环。这是待补齐能力，不是最终产品完成状态。

profile、sample、observe-target是显式诊断采集命令，使用--configuration/--workload定位其输入；不代表用户必须为普通predict指定base。observe-target产物只用于独立评价，不作为预测模板或成本。CPU/device sample必须提供非空--measurement-requests，不自动展开长度/批大小网格。

status/resume保留任务预算及来源；活跃预测不接受资产/预算修改，返回requested_changes_applied=false。宿主中断后保留处理阶段，依据真实容器状态恢复同轮结果，完成任务的重复resume不新开轮次。

`resume --run DIR --template-assets FILE`使用JSON数组替换完整模板输入列表，字段同assets.templates，路径相对FILE解析；可同时更新各来源的node_durations。`--performance-manifest FILE`可重复，替换独立性能输入。预算仍使用--base-budget-sec/--performance-budget-sec追加总额。显式资产修订后当前从初始状态重算全部组合；按依赖只重算受影响组合尚待实现，不恢复模拟器内部checkpoint。

## 显式给定耗时

每份模板资产可指定node_durations路径，文件包含base_manifest来源引用、unit="ns"、非空assumptions数组及values数组。每项为source_span和非负整数duration_ns。base_manifest是文件内的资产身份，必须匹配该条模板来源；不能跨来源复用span编号。当前predict需要所用服务节点的值；缺失返回needs_data，不回退到观测时间差或原联合模块成本。

等待与注解只贡献依赖，服务节点才计费；不能以父模块包络代替内部节点耗时。给定假设仅支持结构分析，真实预测的节点成本由后续S5提供。

## Workload 与结果条件

[workload 示例](../../configs/redesign/workload.json)包含三个开放到达请求。requests 非空；每项包含唯一非空 id、arrival_offset_us、output_tokens，以及 input_tokens / text 二选一。

- arrival_offset_us 为非负整数微秒，归一为 ns；无需按时间排序，同刻按文件声明顺序。当前SGLang原生观察将同刻的预分词请求合为一条批量消息，避免前端逐请求提交把逻辑同刻拆开；不同到达组不等待前组完成。采集记录的batch_response只表示整组返回，逐请求完成仍以Publish事实为准。
- input_tokens 为非空、非负整数 token 数组；真实消费前还须按模型词表验证。text 为非空字符串，采集/准备时用声明 tokenizer 转换一次并保存实际 token；当前 plan 标明 requires_tokenization。
- output_tokens 是正整数，表示条件化生成长度，不预测内容或 EOS。prefill 已生成第一个 token，因此输出 N 个 token 是一次 prefill 加 N−1 次 decode；N=1 不调用 decode。
- 请求到达与完成共同决定并发；不再加入含义冲突的客户端闭环并发参数。
- 首轮 SGLang 关闭 prefix cache，冷 KV 且不做内存压力重撤。需证明容量能满足声明请求预算；不足时报告规则缺口，不能修改请求或忽略等待。
- 专家选择、输出内容和缓存命中等内容相关行为不能由长度虚构。KTransformers 路由尚缺目标适用规则，当前明确 unsupported。


## 框架配置及参数影响

配置文件仅包含 parameters 和可选 model_assets（默认空）。当前接受的参数子集全部显式填写，不采用隐藏的原生自动默认值；空 parameters 会报告缺必填字段。后续采集仍须记录原生解析后的最终生效值，若发生调整则报告差异。

| 框架 | 当前 parameters 子集 | model_assets |
| --- | --- | --- |
| SGLang | device、tp_size、attention_backend、max_running_requests、max_total_tokens、chunked_prefill_size、max_prefill_tokens、schedule_policy、disable_radix_cache、disable_overlap_schedule、disable_cuda_graph、enable_mixed_chunk、enable_hierarchical_cache | 当前为空；主权重由 model.path 指定 |
| KTransformers | backend_type、tp、max_batch_size、chunk_size、cpu_infer、cache_lens、sched_strategy、use_cuda_graph | 必填 gguf_path、optimize_config_path，保留原生含义 |

字符串、布尔与整数保持原生类型；SGLang chunked_prefill_size 为 -1 或正整数，其他计数必须为正。parameters 中不重复主模型路径。实际进程启动使用参数数组或原生对象，不拼 shell 命令。

| 参数差异 | 控制需求 | 模板/成本/资源需求 | 首轮判断 |
| --- | --- | --- | --- |
| SGLang max_running_requests 2→1 | 请求准入、组批及请求索引表容量变化 | 请求池及派生视图 layout、实际 batch 工作量、对应节点成本 | 已接入有限请求池缩容规则；结构资格和节点成本待验证，条件执行不等于可预测 |
| SGLang 其他参数 | 分块、缓存、重叠或实现规则可能变化 | 相应新子图、变换、资源与成本 | 当前范围外；给出具体差异 |
| KTransformers max_batch_size 4→2 | mini batch/chunk 规则变化 | 专家负载、graph padding、CPU/NPU/传输依赖 | 当前范围外，原生路线和取证任务已明确 |
| TP、线程、后端或权重变化 | 逐项解释 | 布局/通信/资源/独立测量，不按比例除时延 | 本轮首个变化不包含 |


## 指标、范围与验收门槛

modeled serving 从已有 token 的请求逻辑到达开始，到最后一次 publish 完成；包含 schedule、prepare、CPU launch、NPU/CPU 专家、必要 DMA/通信、采样及完成处理。HTTP 接收/发送、tokenization/detokenization、初始化/预热和 idle polling 时间排除，必要依赖与数据仍保留。不能从 target HTTP 总时延任意减残差得到该 scope。

| 指标 | 定义 |
| --- | --- |
| window_time_ns | 最后一个请求 published − 最早的逻辑 arrival，含排队及开放到达间隔 |
| request_latency_ns | 本请求 last published − arrival |
| ttft_ns | 本请求 first published − arrival |
| tpot_ns | (last published − first published)/(output_tokens−1)；N=1 时 null |
| output_tokens_per_sec | 总 output_tokens / (window_time_ns/1e9) |

独立评价报告窗口/请求/TTFT/TPOT 的绝对误差、相对误差、总体 WAPE 与相对误差 p90，以及覆盖、失败和分框架结果。WAPE=Σ|预测−观测|/Σ观测；观测为零或缺失时不计算无意义比例。当前最终精度阈值**待业务确认，S7 评价前固定**，旧 3%/5% 门槛不自动沿用，也不根据成绩调整。本阶段完成指标定义不代表精度通过。


### 目标执行图诊断文件

output.diagnostics=true 时，structure.graph.artifact 指向本候选实际装配的图文件。公开 predict 将它写在 prediction/round-N 内：combination_N_graph.json按配置声明顺序×workload声明顺序编号。编号是组合位置，不是实现版本；文件路径不使用用户提供的候选 ID。每轮有独立目录，resume 复用已完成轮次结果，不覆盖其他轮次。

文件包含 boundaries、nodes、edges、ports 四个数组，各数组下标为当前候选局部身份。boundary 保留 source_boundary、batch、release_ns 与 completion_ns；尚未完成的时间为 null。node 保留 work_id、rank、source_span、角色、begin/end 边界及变换后的 kernel 参数。edge 使用实际 source/target 目标边界，relation 为源关系名称，null 表示控制器依赖；不能将 null 当作缺边。port 给出实际工作、rank、端口名与边界。源引用通过结果 provenance.source.manifest 追溯。

该文件直接来自执行核，不重建一张预期图。节点 cost 仍由结果中的显式耗时来源或后续合格模型解释，边界时间差不能直接当成测量成本。诊断关闭时不生成图文件；摘要与结果状态保留。失败或缺数据的候选若已进入执行核，可保留已装配的部分图，其完整性以候选状态和未解析连接为准。
