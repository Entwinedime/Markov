# 粗粒度图工作流状态

更新日期：2026-11-08。S1待交付，性能层不在本窗口范围。第68次固定TP2×PP3完成新上下文合同的采集、生产、旁路预测及对照：936动作和12次输入读取全部对应，无findings，原七项PP身份缺口在本例闭合。来源11图的已测关系均与各自完整校准区间相容；PP未测等待边界、EP2×PD重复轮询及独立泛化资格仍须分别收口。

## 阶段与基线

| 阶段 | 状态 | 剩余交付 |
| --- | --- | --- |
| S0 清理与环境核对 | 完成 | 已推送基线 0d5e208de61aba5bf34ad90b26bbd73994871e87 |
| S1 粗图工作流构建 | 待交付 | 代码/文档/数据检查已完成，核对暂存范围并分层提交推送；重复身份限制按原范围交接 |
| S2 初步验证与收口 | 待开始 | 代表机制、来源与评价可信性、冻结条件 |
| S3 全量独立验证 | 待开始 | 冻结矩阵与归因报告 |
| S4 多机探索 | 待开始 | 有界最小证据及扩展结论 |
| S5 最终交付 | 待开始 | 审查、仓库整理及交付 |

S1正在阶段边界交付，不做局部提交。用户参考仓库、忽略配置和论文资料改动不纳入窗口提交。旧逐操作资源日志、IndexTrace/index_work 和旧 PP/PD 控制链已删除，无兼容入口；根目录正式assets仍为空，work中的库是开发候选，不作为S2已冻结资产。

## 实现与资格边界

| 环节 | 当前实现 | 尚未证明的范围 |
| --- | --- | --- |
| 来源解释 | 调用、张量、KV、通信、队列、同步、查询进入粗图；设备计时校准；裁剪无作用标记 | 跨时钟精确对应及未解析提交位置不能猜测；映射一致不证明原生插桩无遗漏 |
| 模板与展开 | 共享定义、子调用组合、调用绑定分离；局部选择、重绑定、资源分段和输出交接 | 同一调用位置子调用数变化、任意角色组合、已有存储跨片段变更 |
| 控制与资源 | 原生维护请求/组批/分块/输出；核心维护槽位、字段行池、页、缓存和就绪；区分 CPU 返回、设备完成、通知和读取 | 重复逐次对应、长在途、失败/撤回与卸载恢复 |
| 代表复用 | 采集前按模块、布局和通信职责选代表；目标独立绑定 | 非对称布局及联合拓扑须有各自证据 |
| 正式任务 | capture 额度/锁/恢复，produce 解释/编译/发布，predict 按域装配；固定镜像和精简输出 | Docker 故障、完整中断及并发恢复未全覆盖 |
| 独立评价 | 内容/内部依赖、跨动作顺序、数据/物理/资源/查询/协议/读取分别检查；来源完整分区和语义归属检查 | 未配对重复、通信与读取仍显式缺证据；符号表达式变换不由来源映射检查证明 |

给定每工作 1ns 仅用于执行结构开发，不是性能数据或精度结论。集合示例的默认域与 chunked/mixed 域已有旧合同运行，不自动外推当前完整资格。

## 当前开发闭环

路径均相对 data/20261105T115043.090552Z_coarse-graph；不得扫描历史目录补齐当前来源。

| 案例 | 已取得的证据 | 保留限制与入口 |
| --- | --- | --- |
| 第68次 Qwen3-32B TP2×PP3 | 两请求/5输出、1536页归还；936动作内容/依赖/返回、31835跨动作边界、376数据输入、200信号、2960通信边、30物理绑定及12次输入读取全部对应；findings为空，旁路覆盖complete | 同配置、同workload限定闭环，不是留出泛化或性能证明；来源时钟仍有限制。work/pipeline-state-pp-{source,production}；results/pipeline-state-pp-{timing,serving,comparison,source-evidence} |
| 第65次 Qwen3-30B-A3B EP2×PD | 三请求/11输出、1024页归还；1040唯一动作、91908边界、120数据输入、34绑定、8654可比通信边一致；全部8752目标通信边自洽。读取→metadata gate→MIN→准入已关联核对 | 148重复身份和4读取共152项 evidence_gap；7764来源/444目标调用及1754来源通信边未逐次配对。work/expert-wrapper-{source,production}；results/poll-causality-{timing,serving,comparison} |
| 第63次 Qwen3-8B TP1 小池缓存 | 四请求/三组；330动作、26155边界、152数据输入、24绑定和24读取一致，无 findings；两次淘汰；7空闲页、144缓存 token、零保护 | 旧镜像限定同来源结果，不证明当前多 worker 或长在途。work/resource-segments-{source,production}；results/resource-segments-{timing,serving,comparison} |

第68次使用原固定TP2×PP3输入，物理卡2–7、六worker全观察。一次采集139.82秒（初始化29.98、预热60.72、有效采集/排空15.68秒），生产124.30、旁路提取1.43、预测32.85、对照7.59秒。936动作/1136端口及12次输入读取均有旁路约束，70,048事件完成；来源六图映射一致，时间检查仍返回1。logs/pipeline-state-pp-summary.json汇总命令、实际退出码与限制。

PP 修复依据：原生 launch 在自身设备入口之后 wait_stream，再把 proxy 交给模型。外层输入只转交句柄，现声明为 metadata；C++ adapter 接受显式 metadata 输入绑定，实际读取由子调用 data 端口和原生等待维护。输出仍要求 data；输出准备的等待在调用前，保留实际读取。第66次含8条错误提前读取边的来源/库留作反例，不改写成修正后数据。

身份修正：scheduler_work原只看当前队列，漏掉PP其他槽位。选批、输入读取与通信/信号现共用PipelineABI.control_context，保留当前批次、槽位进度和running_batch的固定槽位绑定，并区分None和空成员；不读取轮询次数或补写旧记录。第68次每个worker的五次空选择均有五种真实上下文，目标完整对应。隔离检查及镜像核对见logs/pipeline-context-{review,image-review}.json，原生快照统计见logs/pipeline-state-pp-contexts.json；这不保证任意循环都有唯一身份。

EP2×PD 的重复时间限制：来源一次出现的子动作，若处于重复父路径中，不保证目标也只执行一次。给定1ns下，一个 Decode rank 可能更早读到 Success；MIN 正确保留 pending，下一轮才执行条件子调用。首次错误赋时导致的运行失败保留于 results/expert-wrapper-serving；当前排除重复祖先下的绝对时间，保留1030个合格端口，不注入原生查询结果、不改时间或强配次数。全部468来源/54目标操作数和各6次准入已核对；四项读取缺口均来自重复身份。进一步沿既有观察记录独立重放，篡改操作数、移除参与者依赖、归约未完成及重复准入四项内存反例均被现有评价拒绝，未新增协议实现。logs/polling-evaluator-review.json 保存证据；这不授予逐次时间或轮询轮次资格。

## 来源、资源与动态组合的剩余边界

- 来源投影独立检查已覆盖完整工作/顺序分区、恰好所需的边界、共享事件语义归属、调用层级/上下文及通信声明。当前 PP 六图、EP2×PD 四图和 TP1 一图共11图映射一致。真实图内存副本中的漏/重复工作、无依据边，以及错误环境/角色/父层级/参数/通信参与者/事件归属均被拒绝；没有改写业务图。证据 logs/source-{partition,semantic}-review.json。表达式归一化及条件/shape 变换不属于该检查。
- 来源检查现使用同次采集的校准区间，按每个rank全部已测关系求共同可行性，不逐边放宽时钟、不输出拟合时间。PP六图106766项、EP2×PD四图430750项和TP1一图48303项已测约束均相容；原191/1153/0条名义倒置仍保留（最大4.669/3.960/0微秒）。EP2×PD和TP1本次来源检查返回0，PP因212项未知时间关系返回1：涉及88个未测事件，均属于等待区间，没有工作区间使用这些未测端点。此结果证明现有仿射校准假设下每张图的已测约束可同时满足，不证明精确时钟、跨rank时间、SDK关联或观察完整。证据results/clock-constraints-{pipeline-state-pp,expert-wrapper,resource-segments}及logs/clock-constraint-{review,checker-check}.json；原时间/依赖未修改，旧报告不改写。
- 同图状态初始化已修正：先收齐本次访问，允许真实依赖先行的本地写入与有效历史内容共同覆盖读取。四项临时内存断言覆盖合法先写后读、反向依赖、部分 KV 字节及多个片段完整覆盖；一次复用第67次来源的 PP 预测/对照与既有 result.json、actions.json 完全一致，保留七项缺口。证据 logs/local-state-{initialization-check,pp-summary,pp-prediction,pp-comparison}.json；results/local-state-pp-{serving,comparison}。未新增原生来源，断言不等于真实在途证据。
- 核心逻辑归还使旧内容失效，在途访问保留到设备完成；跨池写入使用目的资源所有者。独立 KvLifetimes 检查物理页/层/token/字节交叠及跨 rank 因果。PP 的1920次、EP2×PD 的4512次物理访问均无在途归还或待完成冲突，不能据此声称触发了长在途复用。原始反例只检查评价器，不是真实在途场景。失败、撤回、卸载不借此取得资格。
- 现有 EP2×PD rank3 库已通过公开 instantiate 入口组合48层：24层专家接收量为0、24层为4；48个 combine 的端口形状及边界依赖/工作放置，与同位置同参数的192次真实来源局部调用一致。这是有限局部组合的核心机制证据，不是完整 serving、跨 rank 路由守恒或原生留出目标验证。没有新增来源/规则，调用树仍391个 scope。输入 inputs/mixed-layer-composition.json，证据 logs/{template-composition-inventory,mixed-layer-composition,mixed-layer-composition-check}.json；生成图核对后删除，仅保留可重现输入与摘要。
- 同一调用位置子调用数变化尚缺可达真实分支。不同层固有3/4/5子调用或空/非空专家接收不代替此项；当前 Qwen3/Ascend、DP/CP=1 非空批次不能仅改长度触发空 attention，不为此盲目补采。已有存储跨片段变更仍须在实际写入处声明端口。

阶段静态联查已覆盖以下职责链；它不替代行为证明或S1交付：

| 职责链 | 审查结论与边界 |
| --- | --- |
| 原生观察→解释→切分→模板→实例化 | 原生关联/队列次序与实测时间分离；外部依赖由端口或串行资源承接，定义共享保留别名、版本与写入者，等待不重复计费。未发现新增确认缺陷；子调用数变化仍缺真实分支证据。 |
| 控制→资源→执行→完成反馈 | 输入/停止、PD通知/元数据和资源复用已联查，同图初始化缺陷已修复。重复逐次时间与长在途覆盖仍未闭合。 |
| 配置→代表来源→框架语义/布局→worker绑定 | 采集与预测共用配置及批次语义；探针保留前缀、页/chunk余数与路由转折；代表比较模块、布局、组职责和PD分片位置，目标重绑模块、KV层及通信参与者。未发现新增确认缺陷；不证明所有原生分支均已覆盖，Ascend MHA五维布局之外仍明确不支持。 |
| 任务入口→容器/进程→发布/恢复→构建安装 | 所有权及生命周期已联查；阶段引用检查见logs/stage-entry-review.json。本次source/control镜像内56/36个自有Python文件与源码一致，证据logs/pipeline-context-image-review.json。完整中断/并发恢复未全覆盖。 |

收尾归属已澄清：重复轮询与读取的时间身份不足属于旁路/跨运行评价的事实缺口，现有partial保留，不要求靠新增身份或强配次数归零。此次沿停等、实际消息/完成唤醒、读取、metadata gate、组内MIN及准入复核，未发现新的确认功能缺陷；原生234次、目标27次元数据交换各自计算正确，原生468/目标54项操作数及各6次准入均有既有独立证据。它们不证明逐次时间对应。动态子调用数、长在途及新配置属于S2/S3实际覆盖责任；若发现能力缺失仍须修正实现。整体交付审查及仓库检查已完成，尚待文档提交与远端推送；不能据此宣称EP×PD全流程独立通过。

交付前补充审查已核对CMake/安装入口、CLI装配、不可变库与模板索引生命周期、仅叶实现的局部替换、serving返回/尾部工作及进程发布边界；未发现新增确认缺陷。最终modeling增量构建通过，二进制仍为196667f9e6ee，与固定PP闭环相同，不重复运行案例。无活动消费者的build/native-observation及build/service-queues已删除（54文件、405081字节），source镜像内观察库及build/coarse-graph保留。证据logs/s1-delivery-{code-review,build}.json、s1-retired-build-cleanup.json；数据保留范围另已核对，代码实现提交为e3efa62；文档提交和远端交付仍待完成。

## 历史证据保留用途

本轮已撤下七组被替代的生产目录（batch-sync、state-lifecycle、reservation、query-boundary、pd-completion、marker-span-final、input-read），以及五次旧预测的完整/未完成执行明细（batch-sync、enclosing-timing、reservation、state-lifecycle、input-read）。历史日志及额度中的这些路径只记录当时结果，不能再作为可用资产或完整执行证据；来源采集、输入、预测摘要/错误日志保留，生产状态和步骤日志汇入 logs/s1-derived-data-cleanup.json。当前三案例及下表反例未撤下；仍被独立输入引用的 clock-binding 生产数据保留。

三次已结束的一键入口案例已原样归入work/entrypoint-cases/，各自保留原时间目录名：20261107T041600.395096Z是失败反例，050118.056820Z是四目标容量/请求组合，055954.052729Z是chunked/mixed两目标。448文件、502261945字节内容未改，data现在只有本窗口根；历史命令/生产记录的宿主绝对路径不改写，也不建立旧路径别名，不再按原路径恢复这些已结束运行。位置交接见logs/s1-entrypoint-data-consolidation.json。当前三案例和必要反例继续按明确路径使用，不扫描历史目录补齐资产；sources中的早期开发记录不授予当前合同资格。

| 入口 | 用途 |
| --- | --- |
| work/forwarded-pp-{source,production}；results/forwarded-pp-* | 第67次，七项PP身份缺口修复前的来源/对照；最新闭环使用第68次 |
| work/current-pp-{source,production}；results/current-pp-* | 第66次错误读取边及修复前预测，反例 |
| work/pp-event-owned-*；results/native-idle-pp-event-owned-* | 第59次 PP 旧合同机制证据 |
| work/protocol-poll-*；results/native-idle-protocol-poll-registered-* | 第56次 EP×PD 旧合同机制证据 |
| work/routed-cache-*；work/local-expert-* | 第60次 EP2缓存与第61次 EP1本地专家，不能外推新合同/新组合 |
| work/prebuilt-output-source；logs/prebuilt-output-capture.json | 第64次原生 dispatcher 包装器核对错误导致初始化失败 |
| logs/{native-release-path-review,inflight-release-inventory,kv-lifetime-evaluator-check,kv-lifetime-review-summary}.json | 在途静态路径、真实未触发范围及评价器反例 |

## 下一步与环境

1. 核对本阶段暂存范围，按实现/验证工具及文档分层提交推送；不包含用户参考仓库、论文、忽略配置或运行数据，不为已解释的重复轮询时间身份添加分支或重跑相同案例。
2. 保留固定PP完整闭环、EP×PD协议/结构证据及其partial、TP1缓存机制和必要反例，远端确认后记录下一阶段基线；不把阶段交付写成独立验证通过。
3. S1交付后启动S2：独立目标闭环、同位置子调用数变化、长在途可达条件及主要配置/调度交互分别取证。EP×PD没有逐次时间资格的范围继续明确报告，不能借阶段迁移变成通过；功能缺失回到完整职责链处理，未冻结前不开全量矩阵。

当前source：e392ee6301e29ebdf3a8eab2b22efe81b3c6f78ad366ad38d728d652c2cc4a53；control：59e0bbf7064d1b9c56661731cb0fead175385342f2000e336dc351ccf98f864c；modeling：e066a5196c3b34b2f46079227e4b5c2a1a811c9d06defca329175954101ca7ca；二进制：196667f9e6eed5a799b1428b6d4dd58e01353a00402b40759ff27bc785b13f0b。第68次使用当前组合；第67次及同图初始化回归使用旧source 1d2ad2481db9/control 46649554f4a6；第67次原闭环及逐层组合使用旧二进制d3588f405166；第65/66次使用旧source 173a9edef4d5/二进制3767ec20ddb0，各自记录不改写。

额度：原生累计上限68次，另有五次设备时钟诊断；后处理100260秒，保留34GiB。第68次一次原生尝试、无重试，新增来源/派生/结果326.30MB。随后派生数据清理释放6.570GB；三次历史一键入口案例归入后，本窗口根总保留约26.515GB。时钟检查增加300秒后处理额度，无新增原生尝试。当前来源、反例、输入、预算及恢复信息保留；阶段仓库检查完成，提交推送待完成。inputs/s1-evidence-budget.json保留原累计预算和运行历史，清理明细见logs/s1-derived-data-cleanup.json；未开全量矩阵。

自有Python/C++：src155文件/18398行，example15文件/4078行。PP共享适配器两文件净增11行；随后时钟评价在example净增88行，复用原来源检查入口，未增加核心分支、模板条件或永久测试。官方框架与.clang-format未改。
