# 2026-10-16：过滤子图复用后的周期代码审查

> 历史审查/观测记录：结论限定于文中日期与当时实现。2026-10-16 清空数据后，旧 `data/redesign` 地址不再可复核；本文的“当前”“待完成”不作为现在的任务或授权状态。实时状态见[主计划](../2026-10-08_redesign_execution_plan.md)。

依据：[工程约束](../../redesign/engineering_constraints.md)。本次是此前周期审查后第五个实质代码批次，覆盖CPU范围迁移、轻量端口观察、原生deferred reset、共用预热收尾及过滤子图角色复用。当前交付状态仍只在[实施计划](../2026-10-08_redesign_execution_plan.md)维护。

**结论：过滤片段复用通过真实资产的公开实例化，保留原切口保护；删除两套同义计数和一个无独立职责的转发接口。未发现需另建控制器、图改写框架或采集状态机的确认缺陷。** 成本缺口、测量资格与预测精度不由本次代码审查证明。

## 审查范围与边界

人工沿公开入口核对输入、Python采集/插桩、框架与runtime适配、模板生产、有限变换、控制器、执行核、成本及任务恢复的主要读写链。重点检查目录和类职责、依赖方向、状态所有权、逻辑分组、无消费者功能及特例增长。不是逐行证明所有原生签名或重验全部历史规则。

静态检查覆盖139份Markov C++文件、49份Python文件及正式CMake源清单；C++格式、构建及依赖检查在modeling Docker内完成。原生观察与环境适配沿现有部署证据检查职责，没有启动新的模型或采集。

| 职责链 | 审阅结果 |
| --- | --- |
| CLI、workflow与适配器 | CLI绑定具体实现，workflow装配公共参数；CMake保持workflow → adapters → core，核心直接及传递头文件依赖未进入具体框架或workflow。 |
| 请求、KV、workspace与时钟 | controller持有请求/槽位/KV；WorkspaceState持有容量；SimulationRuntime拥有时钟和完成反馈。BatchInput是快照，整批装配成功后才提交workspace变化。 |
| 采集与恢复 | 原生attempt、后处理、预测/拟合容器各有实际所有者；框架wrapper读取原生状态，不接管请求/KV生命周期。公共插桩不导入具体框架，环境解释留在runtime。 |
| 模板与有限变换 | 新索引只引用已有区域；插入节点保存目标角色，最终参数化再复制布局求值，来源库不变。使用既有区域替换、资源端口和等待重绑定，没有按人数或样本新增分支。 |
| 成本与报告 | 需求分析和执行共用CostQuery/NodeCosts；设备工作收费，CPU控制保留结构身份，在执行投影时排除服务费用。同步host复制仍计入传输。 |

## 发现、整改与关闭证据

### 过滤片段的调用角色此前不能重绑定

[模板生产绑定](../../../src/markov/src/adapters/frameworks/sglang/template_bindings.cpp)只临时保存filter角色，核心不能从已有running-update partial片段生成pending-prefill partial调用。现在把已资格化的角色映射到已有区域索引，严格写入[编译资产](../../../src/markov/src/templates/template_asset.cpp)；旧资产从同一来源事实重编，不增加兼容解析或复制子图。

[共同模板层](../../../src/markov/src/templates/template_bank.cpp)保留transition外围，在最终参数化阶段同时重绑定[Index布局](../../../src/markov/src/templates/batch_parameters.cpp)和索引H2D字节。审阅中保留了两项原限制：内层替换失败仍可尝试合法whole-transition donor；running-update外壳仍只能含原先允许的CPU/等待工作。成功的内层与整段替换互斥。

公开实例化中，两rank各为57−4+242=295节点，只替换内层filter；每rank七个Index变为3→2、索引[1,2]，一条H2D为16B。外层transition和running-update各保留一次，来源仍为原始2→1。新增端口与等待绑定到上一批真实Publish，图无环、未解析连接为空。原有切口已经通过，无需扩大祖先搜索或放松资源资格。

### 性能需求重复维护同义计数

CPU聚合收费删除后，[CostRequirements](../../../src/markov/src/performance/cost_requirements.cpp)中的units/services两对计数始终同增，且没有消费者区分。删除units字段及维护逻辑，只保留actual_services、measurement_required_services，不保留旧别名。对象种类继续由objects/coverage表达。

### NodeCosts保留无职责的转发层

[NodeCosts](../../../src/markov/src/performance/node_costs.cpp)的model_cost只将expected转为optional，唯一调用者estimate随后再判断。删除声明和实现，estimate直接调用query_cost；候选顺序、显式given回退及缺口语义保持。同步修正publication区域仍沿用CPU收费口径的注释，保留成员语义和完成依赖。

## 公开验证与限制

- 新目录Docker C++23构建及同事实模板重编通过；68个正式C++源全部进入CMake，139份C++格式及核心传递依赖检查通过；49份Python AST/Ruff/格式通过。
- 无given预测已越过旧transition停点，完成第二批结构装配，随后明确返回五个成本合同缺口和一个D2D成本缺口。没有新采集、拟合、未知填零或结果补偿。
- 另用原有given的全部值及假设不变、只关联重编来源，完成三请求的三批条件结构执行。这不是完整性能预测或独立精度验证。
- 两配置固定序列保持全部逐请求指标、图规模、成本来源和窗口一致。编译资产身份变化没有改变这些已验收行为。

证据在`data/redesign/s4-filter-role-reuse`的scope、operations、static-verification、verification及regression-verification。事前声明native/raw均0、derived768MiB、Docker流程处理600秒；结算见accounting。没有新test/fixture、`.clang-format`修改或历史采集改写。

累计DAG每次追加仍遍历历史图，是已知增长风险；尚无本轮瓶颈证据，不因此预建增量执行框架。观测开销、部署及成本域资格继续按原计划处理，不把代码审查升级为新的逐事件门槛。本次周期审查收口，实质代码批次计数归零。

## S3–S6交接前复查

本次因进入下一阶段前的工程约束执行，不按采集次数触发。审阅覆盖活动C++核心、适配、生产、模板、workflow/CLI与构建，以及Python编排、插桩、产物、框架/runtime适配和原生观察安装链；适配/模板/生产专项实际覆盖79份实现及头文件。人工沿控制动作、参数化、原子装配、成本查询、完成反馈和持久任务追踪消费者，静态范围为139份Markov C++与49份Python。最终命令和核对在`data/redesign/s6-stage-handoff-review`，阶段状态只在实施计划维护。

### 发现及最小整改

| 发现 | 影响与处理 |
| --- | --- |
| 同义成本计数 | ExecutionGraph同时维护、输出完全相同的cost_usage及cost_covered_service_nodes，无独立程序消费者。删除后一字段、累加器及合同引用，只保留cost_usage和按工作分组，不留兼容别名。 |
| 插入同步复制缺少共同参数约束 | parameterize_insertions此前只让DMA在缺transfer_layout时停止，CpuControl上的已知同步复制可能沿用donor字节。统一通过已有缺口拒绝缺布局的已知传输；固定字节仍须生产端显式声明已有布局，不增加传输特例。未证明旧固定资产已触发错误。 |
| Prefill配方缺少确定的预算前提 | 普通采样仅逐成员校验。按原生“接纳当前成员后再停”的规则，前缀未取整prompt已耗尽预算时才拒绝下一成员，保留末成员越预算；带连续槽位的Prefill复用同一前提，不再重复总量拒绝。Decode filler保留独立cohort限制，不猜页大小、不复制控制器，其余配方仍须实际覆盖。 |
| CPU范围迁移遗留 | 删除SourceCall中只被读取、转存的observed_thread_cpu_ns及解析/JSON输出，原始事实不改；修正提取器仍要求独立CPU测量、同步复制被称为CPU服务和parent bridge必定收费的文字。保留必要排序时钟、结构注解和同步传输链。 |
| runtime识别具体框架 | Ascend产物层此前按KT名称添加无消费者native_requests引用。删除该特判，KT框架端仍保留真实原始请求映射文件；不新增接口或搬走有消费者逻辑。 |

未发现新的请求/KV复制状态机、预算恢复或role隔离缺陷。filter性能观察读取真实前后成员，pool槽位在原生释放前记录；两者不依赖语义AST。采样配方留在适配端，核心通过公共工作合同处理过滤和池访问；CPU服务在执行投影排除，实际传输仍收费，未知成本在推进前停止。

### 保留的债务与证据边界

Ascend normalizer把RAW导出偏移直接用于MONOTONIC观察，是尚未显式桥接的跨时钟假设。已追到torch_npu本地源码确认两个时钟不同；现有实际4个API、2个同步复制均在对应父区间内，另一次原点位于3770ns的RAW括号内，没有据此证明当前样本错位。typed成本键、同钟设备/driver时长和任务身份关联不因该加偏移失效；潜在影响是跨域位置及包含判断，不能用task ID证明全部时间关系。ports路径全程使用同一perf_counter，不经过该偏移。证据见clock-review-verification，不宣称跨部署已资格化，也不扩成逐CPU计时任务。

累计图每批全量拓扑重建、扫描历史work的规模债务继续保留；当前目标尚无因此超预算的证据，后续只有真实规模需求时才改增量算法。代码审查不能替代独立误差评价，原生窗口大偏差也不能用新增CPU补价或目标残差掩盖。

### 交接关闭证据

全部修改后Docker C++23构建及139份C++格式通过，68个实现全部注册、81份核心文件的传递依赖未进入adapters/workflow；49份Python AST/Ruff/格式及修改文件复查通过。最初依赖扫描把include/markov下的目录深度识别错，产生的假阳性及修正均在证据中保留，没有据此修改产品依赖。

六个正式prepare-sampling输入核对带/不带连续槽位的预算前缀耗尽拒绝、末成员越界和单成员越界保留，全部符合预期；这些是准备语义，不替代真实分页覆盖。最终两个公开predict覆盖固定序列两配置及pool3，三组合均完整、零采集/缺口，170/170/226成本对象可用；指标、执行摘要和结构保持，只有明确删除的同义计数及诊断文件引用不同。新共同guard没有要求额外模板或费用。

证据为s6-stage-handoff-review的最终构建、sampling-boundary-verification、final-regression-verification和静态检查。此次整体交接复查收口，周期归零；部署/观测/跨时钟及最终精度限制继续独立保留。
