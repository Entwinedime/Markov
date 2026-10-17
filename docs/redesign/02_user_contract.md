# 用户输入与探索合同

正式入口为固定模型 × configurations × request_workloads，不指定 base。本文定义输入与结果合同，阶段完成情况只见[实施计划](../redesign-workflow/2026-10-08_redesign_execution_plan.md)。SGLang + Qwen3-32B 为预测主线，KT 保留显式采集，不能因配置可解析而称为可预测。

首个性能闭环采用明确近似：保留原生子图、有限变换和轻量控制动态展开，排除 host 控制、准备、提交与结果处理的 CPU 服务耗时；计算、通信、搬运和设备命令继续计时。CPU 控制逻辑、请求/KV 状态及必要提交、等待、发布依赖保留。同步 host 复制属于传输，不能随 CPU 服务一起排除。详细范围见[性能与执行合同](06_performance_execution.md)。CPU 服务收费、拟合和申采链已删除；实际部署、无 given 预测和独立评价的状态统一见[实施计划](../redesign-workflow/2026-10-08_redesign_execution_plan.md)，不能由候选可用推定精度通过。

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
| assets.performance_manifests | 显式独立测量资产列表，默认空；由独立拟合进程生成候选后供全部组合共享，不将样本直接交给执行核 |
| assets.cost_models | 默认空；显式拟合成本候选文件列表，声明顺序用于重叠工作选择；当前输出保留条件性质与测量资格 |
| output.directory / diagnostics | 必填输出目录；完整图诊断默认false |

路径相对声明它的文件解析。模型路径可为部署容器路径；CLI项目路径使用仓库路径。prepare-prediction生成全部配置和workload的独立文件，持久任务按数组位置命名快照，不把用户id拼入文件路径。

## 仿真起点：统一预热后开始

项目定义固定预热流程，结构采集与独立目标验证共用；用户无需选择 base 或提供预热请求。模型加载、运行时初始化和预热不计入预测时延，但仍计入采集预算。目标请求的 arrival_offset 从预热结束后的业务窗口解释。

当前 SGLang 主线采用单请求、输入 token `[1]`、生成两个 token、ignore_eos=true、temperature=0，覆盖一次 Prefill 和一次 Decode。配方不读取目标 workload，不以达到最大 workspace 容量为目标，也不保证所有后端分支已预热。适配其他框架/模型时须资格化其具体执行，不默认继承此配方。

这段描述是当前实现，不能作为任意目标特征已进入稳态的证明。声明域内编译覆盖、相容的采样/验证起点及必要 CPU 范围由[当前专项](../redesign-workflow/2026-10-17_steady_state_template_cpu_plan.md)规划，尚未实施；具体合同随 P0/P1 落地替换，不把方案写成已支持输入或命令。

当前关闭 radix cache 的主线在预热请求完成后，经原生 idle 握手确认批次清理并释放请求/KV，再开始业务窗口；客户端收到响应不能替代该确认。适配层须核对清理接口在当前部署保留已热的设备分配缓存，不能因接口叫 flush 就推定其效果。目标不继承预热内容，资源编号无需恢复成进程启动时的编号。不同配置预热后的容量可以不同，目标执行中仍须模拟分配、扩容及同步。

采集记录实际预热输入、sampling 参数和窗口前资源快照。C++ 生产端检查记录并保存 `initialization_gap`，预测起点须具备相容的初始化规则。旧资产不补造预热证据，也不能无条件将来源容量复制给目标。请求池缩容已有有限初值迁移规则；其他配置变化仍须满足生产端的实际适用域。

整批及局部子图复用另按共同来源条件、工作绑定与接口资格判断，始终消费目标的容量状态，不继承供体的初始容量。容量推进和 allocator 分支替换见[workspace 合同](sglang_template_relations.md#workspace-容量状态与设备级同步)，具体结构验收范围只在实施计划维护。

## 组合执行与结果

combinations 按配置声明顺序 × workload 声明顺序输出，每项包含 configuration_id、request_workload_id、status、scope、metrics、gaps、assumptions、provenance 和 collection_usage_ref。预算用量在根 collection_usage 共享，不按组合重复计费。预测没有隐含 base、base_id/target_id 或默认 speedup；同一 workload 和 scope 内的指标才可比较，当前不生成综合排名。独立原生对照另存结果，不把来源配置当作比较基准。

上述是完整持久报告合同。预测任务CLI只展示状态、范围、缺口及整体指标摘要，并保留任务目录与报告引用；逐请求指标、性能需求和来源明细从该报告读取，见[集合入口](development.md#2-集合入口)。摘要不改变任务状态、成本来源或恢复语义。

候选状态为complete、needs_data、unsupported、failed。complete仅表示当前scope下条件执行完成，不表示结构泛化或性能精度通过。任务finished可以包含缺口/失败组合；总体status反映这些未完成项。一个组合的执行失败不阻断其他组合。 结构采集缺口仅附加到实际缺少兼容模板的组合；其他组合仅缺耗时或其他数据时保留其自身原因。每个组合重新创建控制器、KV、事件与资源状态，共享资产只读。

模板按自身的来源配置、模型和部署判断适用性，不以某个候选充当来源。当前先检查配置、部署、来源身份缺口、准入策略及请求池容量适用域，再按声明顺序选取兼容的首选来源，优先具备显式给定耗时的来源；同等条件选择第一个。其他来源须与首选来源的模型、原生配置、部署、页大小、序列容量及准入策略一致，才能进入共同候选。每个目标批次按来源顺序选择覆盖全部阶段/rank的原生批次，再按有限规则做局部donor替换；跨来源要求资源角色资格成立。该路径已有局部donor和整批切换的公开运行证据，不授予任意来源或任意配置变换资格。来源已加载不等于目标所需模板齐全，动态展开仍报告实际缺口。assets报告每份来源的加载结果；assets[].source集中保存来源观测和参数绑定，组合provenance.source记录首选来源；structure.graph.sources列出实际图使用的来源，工作明细中的source_manifest限定source_call/source_batch。给定耗时文件与假设由assets[].node_durations提供，不再在组合provenance中重复单一来源耗时描述。source.static_qualification只说明来源身份、准入策略和请求池的静态资格，available不代表全部语义覆盖或成本可用。 显式耗时文件解析或来源/单位校验失败时，结构来源仍为loaded，assets[].node_durations及相关组合单独报告failed/node_durations；不把成本输入错误解释为compatible_template_missing，不据此规划结构采集。

## 采集边界与恢复

常规predict只需探索集、可选显式资产和采集条件。代表配置及代表request由有界建库规划选出，不直接枚举目标workload重采；预算到限不等于覆盖完成。plan/predict在representative_collection报告候选组与排除原因，requires_mechanism_coverage不代表每组都必须采集。predict先消费已有资产，缺兼容来源且允许采集时才在累计base预算内执行每组一次的固定短请求配方；禁采返回base_collection_disabled。生成库导入后重新预测，失败不自动重试；资产加载失败先修复，规则及成本缺口不触发结构采集。恢复合同见[集合流程](07_integration_workflow.md)，当前完成程度只在实施计划维护。

profile、sample、observe-target 是显式诊断采集命令，使用 --configuration/--workload 定位其输入；不代表用户必须为普通 predict 指定 base。observe-target 默认 --purpose ports，观察原生提交/发布返回端口；--purpose structure 保留完整结构观察。两者都只用于独立评价，不作为预测模板或成本；目的随任务保存，resume不修改它。具体边界见[性能评价](06_performance_execution.md#6-输出与误差)。sample 支持 transfer、device、communication，默认 transfer；transfer/device 必须提供非空 --measurement-requests，不自动展开长度/批大小网格。当前 transfer 采样要求成套重编译的原生观察库和 torch_npu 队列头文件，部署限制见[开发说明](development.md#队列-abi-与镜像)。

status/resume保留任务预算及来源；活跃预测或成本拟合不接受资产/预算修改，返回requested_changes_applied=false，`performance_live`报告实际拟合容器状态。宿主中断后接回原处理进程及其发布结果，完成任务的重复resume不新开轮次。

`explore` 复用同一项目合同，在全部预测完成后固定资产，再用独立预算取得 validation ports 并汇总；不增加用户指定 base 的要求。预测仍缺数据时不进入验证，允许按原合同修订资产或预算。进入验证后不改预测资产，恢复只处理未完成验证与汇总；改变预测输入须新建探索。实现命令见[开发说明](development.md#全流程示例与独立对照)，完整验收程度见主计划。

`resume --run DIR --template-assets FILE`使用JSON数组替换完整模板输入列表，字段同assets.templates，路径相对FILE解析；可同时更新各来源的node_durations。`--performance-manifest FILE`可重复，替换独立测量输入；`--cost-model FILE`可重复，替换完整拟合候选列表。预算仍使用--base-budget-sec/--performance-budget-sec追加总额。显式资产修订后当前从初始状态重算全部组合；自动建库按代表配置组重算；显式资产修订完整重算，不恢复模拟器内部 checkpoint。

## 显式经验成本输入

`assets.performance_manifests` 进入独立、有锁的拟合进程，处理收据由 `performance_imports` 引用；生成候选追加在显式 `assets.cost_models` 之后，供各组合共享。预测只读取候选值，不取得原始样本或重新拟合。原测量缺口、假设与资格不因导入而提升；恢复复用原处理结果与累计预算。

首个闭环的成本输入只覆盖[性能合同](06_performance_execution.md)规定的设备计算、物理通信、传输及设备命令。模型先匹配模型、框架、部署与适配端声明的执行路径，再按目标实际工作及特征查询；声明顺序用于重叠候选选择。来源不匹配和数据不足分别报告，未知不外推、不填零，也不触发结构采集。

shape 相同不代表工作相同。kernel 查询还保留实现、dtype、layout 及必要的 `operand_work`，attention 使用实际 query/prefix 与 cache 工作；显式张量遍历投影不能自行扩大。搬运区分异步设备服务与同步 host 复制，字节量、方向及计时边界必须一致；同一物理工作只计一次，等待不另叠加观测包络。特征及生产/测量/查询合同集中在[成本定义](06_performance_execution.md#经验候选的生产)，本页不重复列出候选字段。

`sample --kind cpu` 及 host CPU 候选加载、拟合、评价入口已删除，不提供兼容模式。同步 host I/O 使用 `sample --kind transfer`，manifest 仍以 `measurement_kind=native_serving_timing` 交接完整 driver 叶子；该载体不再表示 CPU 服务测量。历史 CPU 资产保留原用途，不回填或改写为新范围证据。

图摘要的 `cost_usage` 按成本来源统计范围内实际计费节点，`cost_usage_by_work` 再按工作类型分组；数量不是时延占比或模型资格。目标图中的纯 host CPU 控制节点为不计费注解，必要边界与依赖仍保留；同步 driver 复制保留传输服务身份。旧报告中的 `cpu_cost_units` 和 CPU 计费仍按历史范围解释。

每组合的 `performance_requirements` 汇总实际查询及覆盖；共享的 `plan.catalog` 与 `plan.target_bounds` 描述来源对象和目标条件。`candidate_available` 仅表示可查询，`needs_data` 表示范围内数据不足，`contract_incomplete` 表示相应工作合同不足。排除的 CPU 服务不再申采，也不因没有 CPU 成本阻止闭环；结构或传输缺口仍须保留。

`performance_plan` 引用共享计划，`performance_sampling` 保存工作域结果。计划先复用数据，再合并范围内的必要采样；一次执行可服务多个对象。`ready` 只表示可执行配方，不保证实际覆盖或精度。同域已有观测或配方已尝试却无新证据时停止，保留原原因；完成成员与请求特征的关联不能简化为完成数量。具体交接见[集合流程](07_integration_workflow.md)。

## 显式给定耗时

每份模板资产可指定 `node_durations`，包含 `base_manifest`、`unit="ns"`、非空 `assumptions` 与 `values`。每项为来源内 `source_span` 和非负整数 `duration_ns`；来源必须匹配，不能跨资产复用编号。

给定耗时用于结构验证或明确的条件分析，不是拟合数据或真实预测精度。首个性能闭环要求范围内的计费工作脱离 given；缺模型时报告缺口，不回退到源观测时间差或父级包络。CPU 服务的有意省略由范围声明表达，不能要求用户逐节点填零；其控制与依赖仍保留。当前执行不再查询纯 host CPU 节点的给定耗时；历史条件运行保留原口径，不能改标签当作新范围通过。

## Workload 与结果条件

[workload 示例](../../configs/redesign/workload.json)包含三个开放到达请求。requests 非空；每项包含唯一非空 id、arrival_offset_us、output_tokens，以及 input_tokens / text 二选一。

- arrival_offset_us 为非负整数微秒，归一为 ns；无需按时间排序，同刻按文件声明顺序。当前SGLang原生观察将同刻的预分词请求合为一条批量消息，避免前端逐请求提交把逻辑同刻拆开；不同到达组不等待前组完成。采集记录的batch_response只表示整组返回，逐请求完成仍以Publish事实为准。
- input_tokens 为非空、非负整数 token 数组；真实消费前还须按模型词表验证。text 为非空字符串，采集/准备时用声明 tokenizer 转换一次并保存实际 token；当前预测 plan 对 text 返回 unsupported / text_tokenization_not_implemented；原生采集准备可进行分词。
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
| SGLang max_running_requests 缩小 | 请求准入、组批及请求索引表容量变化 | 请求池及派生视图 layout、实际 batch 工作量、对应节点成本 | 有限缩容规则与主线结构验收已有；适用域及证据见实施计划，成本资格独立判断 |
| SGLang max_prefill_tokens | 接纳当前请求后达到预算即停止本轮接纳 | 准入工作量、批次与相应节点成本 | 已有同库变换及独立结构对照；不等于任意配置或成本已支持 |
| SGLang 其他参数 | 分块、缓存、重叠或实现规则可能变化 | 相应新子图、变换、资源与成本 | 当前范围外；给出具体差异 |
| KTransformers max_batch_size 4→2 | mini batch/chunk 规则变化 | 专家负载、graph padding、CPU/NPU/传输依赖 | 当前范围外，原生路线和取证任务已明确 |
| TP、线程、后端或权重变化 | 逐项解释 | 布局/通信/资源/独立测量，不按比例除时延 | 本轮首个变化不包含 |


## 指标、范围与验收门槛

首个闭环的 modeled 时间轴从已有 token 的请求逻辑到达开始，到最后一次 Published 完成；保留排队、开放到达及全部必要控制/依赖，由设备计算、通信、搬运和设备命令的耗时推进。CPU 调度、准备、launch 和结果处理保留逻辑次序，但不追加 CPU 服务时间；等待仍受真实前驱约束，同步 host 复制仍计传输成本。HTTP、分词/反分词、初始化/预热和 idle polling 不在计时范围。它是理想化 CPU 的 serving 预测，不是完整原生或 HTTP 时延。

| 指标 | 定义 |
| --- | --- |
| window_time_ns | 最后一个请求 published − 最早的逻辑 arrival，含排队及开放到达间隔 |
| request_latency_ns | 本请求 last published − arrival |
| ttft_ns | 本请求 first published − arrival |
| tpot_ns | (last published − first published)/(output_tokens−1)；N=1 时 null |
| output_tokens_per_sec | 总 output_tokens / (window_time_ns/1e9) |

独立评价先对齐请求、起止/发布端口和可关联时钟，报告窗口与请求指标的预测值、原生观测值及偏差；观测为零或缺失时不计算比例。理想化 CPU 的预测可直接与对应原生端口间隔作诊断比较，偏差包含有意省略的 CPU 服务、范围外工作及观测影响；无需先逐段还原或扣除它们，也不能任意减残差制造同范围真值。端口或原点无法对齐时，不报告相应数值。给定耗时和未资格化候选的比较保留条件性质，不能称为已通过预测精度。完整原生/HTTP 指标另列范围。最终阈值在独立验收前固定，当前不沿用旧 3%/5% 门槛，不按成绩调整；细节见[验收合同](08_acceptance_delivery.md)。


### 目标执行图诊断文件

`output.diagnostics=true` 时，`structure.graph.artifact` 指向本候选实际装配的图文件。组合编号按配置声明顺序 × workload 声明顺序产生，不是实现版本，路径不直接使用用户提供的 ID。诊断与临时工作目录分开，已发布结果所引用的图不能随轮次清理而消失；具体位置见[运行说明](development.md#2-集合入口)，目录改造的验证范围见主计划。

文件包含 boundaries、nodes、edges、ports 四个数组，各数组下标为当前候选局部身份。boundary 保留 source_boundary、batch、release_ns 与 completion_ns；尚未完成的时间为 null。node 保留 work_id、rank、source_span、角色、begin/end 边界及变换后的 kernel 参数。edge 使用实际 source/target 目标边界，relation 为源关系名称，null 表示控制器依赖；不能将 null 当作缺边。port 给出实际工作、rank、端口名与边界。源引用按各节点/边界的 source_asset 索引图的 sources 表，再以来源内编号追溯；provenance.source 只表示首选来源，不能代替实际多来源引用。

该文件直接来自执行核，不重建一张预期图。节点 cost 仍由结果中的显式耗时来源或后续合格模型解释，边界时间差不能直接当成测量成本。诊断关闭时不生成图文件；摘要与结果状态保留。失败或缺数据的候选若已进入执行核，可保留已装配的部分图，其完整性以候选状态和未解析连接为准。
