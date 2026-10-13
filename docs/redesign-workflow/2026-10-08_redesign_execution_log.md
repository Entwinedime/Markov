# Markov redesign 执行记录

整理：2026-10-13。本文只保存关键决策、里程碑、证据用途及必要交接。当前任务与状态以[实施计划](2026-10-08_redesign_execution_plan.md)为准，规则以[工程约束](../redesign/engineering_constraints.md)为准。下列历史结果不自动授予当前实现同等支持范围。

原逐轮记录已合并，删除重复“上一轮属于有效进展”、工具会话号、构建流水和失效下一步；保留机制/决策、代表结果、证据路径及限制。不另存全文备份，原始采集及证据文件未修改。

## 1. 关键决策

| 决策 | 原因与后续边界 |
| --- | --- |
| 固定模型 + 配置集合 + request workload 集合 | 用户不指定 base；系统按声明范围选择代表配置与短请求，不拿完整目标序列反复补模板 |
| 统一流程 + 轻量控制器 | 原生执行提供模板及规则依据；目标路径由公共控制合同和模拟事件生成，原生控制器为后续选项 |
| 结构先行，成本后置 | 给定节点耗时先验证目标结构及反馈；原整批长度查表/联合模块成本不作为节点模型保留 |
| 统一预热后开始 | 预热不计入预测时延，但计入采集预算；KV cold 与 workspace 初始容量分开，不默认空缓存 |
| 持续采集授权 | 用户已允许自主安排后续采集，旧额度等待/blocked 记录不再是当前权限要求；目的、累计预算及停止条件仍须记录 |
| 官方依赖与适配分离 | third_party 恢复官方；框架/模型与 runtime 适配分层，环境补丁随适配器维护 |
| 范围与粒度纠偏 | 只处理统一流程子图及必要交互，模板外服务排除；不为原生后台事件完整性扩展主线 |
| 性能采样按四项约束选型 | 对象一致、简洁适配、跨框架共同途径与有界特征构造共同成立；合同集中在[独立性能采样](../redesign/04_collection_templates.md#独立性能采样合同)，不自行维护原生请求/KV 状态 |

## 2. 里程碑与证据用途

### 2026-10-13：保存适配与成本实现检查点

按用户要求，将此前未提交的适配解耦、Ascend CPU/设备观测、经验成本拟合与消费、任务恢复改动保存为实现提交 `d626a0b`，设计合同、压缩后的历史记录和本轮采样讨论保存为文档提交。沿用 `type(scope): summary` 格式，在 redesign 分支归档；该检查点不代表 S5/S6 完成。

本次 modeling Docker 构建及原生观察库构建通过，公开 `--help` 与示例 `plan` 可运行，67 个改动 C++ 文件的格式检查、Python Ruff、Shell 语法和差异检查通过。没有新增采集或性能精度评价；通信候选增量代码尚未完成训练、评价及预测消费的完整运行验证，提交说明和当前计划保留这一限制。goal 继续暂停。

### 2026-10-13：性能采样抽象与特征覆盖讨论收敛

根据用户提出的四项要求，统一更新采样合同、成本对应边界及 S5 近期交付；“独立性能采样”不再被解释为必须脱离 serving 逐算子执行。语义/性能插桩独立，共享工作描述与必要关联；性能样本须对应模板节点或明确局部子图。框架负责原生状态，公共采样接口不依赖 SGLang 专有 batch 类型。近期优先用现有原生请求入口验证少量参数化请求的实际特征覆盖，不以固定批次、逐 shape 命中或无限补采为目标。

源码核对依据：[NativeSampling](../../src/collection/markov_collection/adapters/frameworks/sglang/sampling.py)、[请求配方](../../src/markov/src/adapters/frameworks/sglang/sampling_recipes.cpp)、[实际批次记录](../../src/collection/markov_collection/artifacts/sampling_domain.py)、SGLang 的 [serving 主循环](../../third_party/sglang/python/sglang/srt/managers/scheduler.py)、[worker 调用](../../third_party/sglang/python/sglang/srt/managers/tp_worker.py)及[静态批次 benchmark](../../third_party/sglang/python/sglang/bench_one_batch.py)。已确认 CPU/device 采样使用常驻 Engine；benchmark 也管理状态，并与 serving 共用较低层执行方法，不能把它称作 Engine 对等的通用批次 API，也不能因“官方已有实现”推导其适用性。

本次只更新文档并做源码核对，没有修改采样/仿真代码、构建或新增原生采集。没有运行 benchmark 的 Ascend 性能对照；独立性能插桩、自动请求/特征域规划及完整覆盖评价仍是后续交付，随机森林等泛化模型未实现。goal 保持暂停，S5/S6 未完成，不写入 implementation_log 的阶段完成总结；当前任务只在实施计划维护。

### 2026-10-13：计算候选进入同一执行核

CPU与kernel候选通过公共`CostModel`消费目标`ServiceWork`，`NodeCosts`保留实际成本来源和显式给定值。kernel拟合、固定评价及预测共用特征合同；预测精确匹配张量描述，并有条件沿用唯一观测启动变体，带额外attention/KV等语义的工作不使用该简化模型。没有按来源span拟合、插值外推或逐名称拒绝表；候选仍为`not_established`，未用静态匹配证明目标启动与耗时迁移。

旧训练来源使用设备0/1且部署runtime未记录，不能直接作为当前4/5部署的可用候选。按预声明计划完成一次训练采样：固定部署、短请求，B2/B4各自预热后重复3次；原生193.545秒，40条预热与120条正式流程记录，预热域无缺失。28554条正式计算测量形成160组候选，没有读取留出数据拟合或重试。预留原始768 MiB、派生256 MiB，实际分别676982122/80902110字节，已纳入累计账本；累计第94次attempt，历史未知启动区间继续保留。

同一模板、六请求序列和三份候选完成两档Prefill预算预测：16384/256分别展开6/7批，使用5032/5070个kernel值、56472/67164个worker值及各4个准入值；156812/188236个服务仍为显式给定值。条件结束时间分别237513180/280652547 ns，不能当作真实性能精度结果。两次预测均无原生采集；仅CPU对照的完整指标、执行摘要及结构报告与改动前保持。此前固定worker/kernel评价也完全保持。

新候选与此前已查看的留出数据做开发比较：10992条匹配、54条未匹配，MAE0.992微秒、WAPE2.85%。这是匹配物理计算span的局部结果，不能称为新的独立最终验收，更不是整段请求误差；数据相关成本、profiler影响及其余服务仍有缺口。证据`data/redesign/s5-kernel-consumption/{capture-plan,declared-candidate,verification,evaluation-regression-verification,development-evaluation,accounting}.json`。Docker构建、Ruff、Shell语法及diff检查通过；第五批后的整体代码审查已完成，见[当日审查](2026-10-13_code_structure_review.md#cpu-与计算成本消费周期)，周期归零。S5/S6仍未完成。

### 2026-10-13：CPU 候选的配置关系回到适配端

删除 workflow 的完整配置 JSON 相等判断及只为该判断存在的 `configuration_parameters` 接口。模型来源配置在读入时由所属适配器转为公共配置类型，`supports_cost_configuration` 解释候选的实现路径关系；SGLang 复用现有控制路径与配置关系，只容许请求池容量和 Prefill 预算变化。它只放行目标工作查询，实际成员/提交成本键仍由 `CpuCostModel` 匹配；未知不填零，也不把结构关系当成耗时迁移证明。框架参数没有进入调度器或性能核心，模型、框架及部署匹配继续保留，worker 省略 shape 的假设仍公开。

公开双配置预测使用同一真实六请求 workload、同一模板和两份固定 CPU 候选。Prefill 预算 16384/256 分别展开 6/7 批，消费 56472/67164 个 worker 值及各 4 个准入值，配置成本缺口为空；其他 161844/193306 个服务继续使用显式给定值。无修订 `resume` 保持两组完整结果及一轮预测，新增原生 attempt 为零。该证据验证候选装配和目标查询，不证明完整成本覆盖或跨配置精度。

Docker C++ 构建、修改文件格式与 diff 检查通过，旧序列化接口无剩余引用；输入/性能/开发合同已同步。证据 `data/redesign/s5-cost-configuration/{processing-plan,verification}.json` 及公开任务，衍生资产在预声明 128 MiB 内并已结算。本批为最近周期审查后第四批代码修改；下一实质批次后进行整体代码审查，S5/S6 继续未完成。

### 2026-10-13：固定计算候选的独立评价

预声明一次 device 采样：SGLang/Qwen3 TP2、请求池 4、Prefill 预算 256、设备 4/5；请求长度 5/11，各输出 2 token，预热后重复 3 次。选择相同总输入量、不同成员长度，检验原固定 kernel 描述候选在新请求下的误差，不以评价残差拟合。一次成功，实际原生耗时 174.727 秒；16 条预热、48 条正式流程记录，无预热域缺失。生产端实际输出新 `kernel_work`，没有追加模板、规则或重试。输入准备曾有一次字段错误，发生在创建 attempt 前，不计为设备采集。

11046 条正式计算测量中，10968 条、90 组匹配，MAE 941.885 ns、带符号均值 -156.893 ns、WAPE 2.7067%；78 条、12 组未匹配，主要为请求池相关 Index/Slice/Cast 等工作，未据此补采。设备侧 Cumsum 的 12 条匹配观测 WAPE 52.18%、最大误差 144.054 微秒，保留原值。总体指标仅对应匹配计算 span，不是端到端或完整建模时间误差。新旧配置/部署不同且历史 runtime 缺记录，因此上下文不相等；attention 等数据相关工作仍需语义特征，评价不授予迁移资格或预测消费。

证据 `data/redesign/s5-kernel-heldout/{capture-plan,evaluation,verification,accounting}.json` 及公开采样任务。原始 264508050 字节、派生 29757574 字节，分别在预声明 256/128 MiB 内，累计第 93 次 attempt 已结算；候选保持不变、训练与评价原始文件分离。本轮仅采样、评价和文档，没有新增代码批次，最近周期计数仍为三；本次采样到此结束，S5/S6 继续未完成。

### 2026-10-13：物理计算候选与历史设备事实导入

`fit-costs --unit kernel` 与 `evaluate-costs` 增加物理计算基线。Ascend 适配器输出统一 `kernel_work`，C++ `KernelCostFeatures`/`KernelCostFitter` 按名称、操作、张量 shape/dtype/format、任务类型和观测启动块数分组，共用既有中位数与误差统计。设备侧 AI_CPU 的原始零启动块数保留，不用设备核心数替代、不加入任务名称特例。候选不含 source span/request 身份拟合，不自动成为目标节点成本。

旧独立 device 捕获缺少当前 native API 的 driver-copy 与语义 scope 字段，完整重新归一化失败后停止。明确导入任务复用原有样本、耗时和归属，只附加当前 producer 的工作描述及保留文件中的设备容量；未记录的 runtime 为显式未知，原始数据不变，也未加兼容解析。76776 条正式计算测量形成 218 组，逐组数量、中位数和范围与原测量一致；42296 条预热与 50112 条正式非计算记录排除。同源评价被拒绝，原 trace 路径参与来源追溯。该结果不证明独立 kernel 精度、数据相关语义或目标启动映射，尚未进入预测消费。

Docker C++ 构建、修改文件格式、Python 全包 Ruff、Shell 语法及 diff 检查通过；共享 workflow 的 worker 65250 段及准入 20 条留出评价与原结果完全一致。证据 `data/redesign/s5-compute-cost-planning/{import-verification,fit-verification,cpu-regression-verification,reuse-rejection}.json`。本轮新增 native attempt 为零，派生资产在预声明 256 MiB 内并已结算；独立 kernel 评价留待后续有界方案。此为最近周期审查后第三批实质代码修改，S5/S6 未完成，不写 goal 完成总结。

### 2026-10-13：经验 CPU 候选进入统一执行核

新增显式 `assets.cost_models` 与 `resume --cost-model`；`CpuCostModel` 的特征读入/查找由评价和预测共享，`NodeCosts` 先按目标工作查询候选，再查剩余显式给定值。没有按 source span 拟合、未知填零或重新训练。workflow 匹配声明的模型/框架/完整配置/部署，原测量缺口及 `not_established` 保留；完整配置相等只是初始候选条件，未完成跨配置成本迁移或正式资格。模型自含数值，Python 显式只读挂载，外部声明路径保持绝对路径；预测不读取来源样本。

公开预算场景使用 47664 个 worker 候选值，其他 137090 个服务仍使用显式给定值；模拟完成时刻由给定基线 54.584 毫秒变为 197.065 毫秒，这证明价格进入事件核，不是实际性能精度。资产修订后的第二轮额外消费 4 个准入成员，重复 resume 不增轮次/attempt、指标保持。为核对诊断索引只启用该开发任务的输出诊断，没有改变模型、配置集合或请求序列；47664 个 worker 诊断节点的 ns 与候选表及模型索引逐项一致，目标根均解析。移除给定值返回具体 schedule_prefix 成本缺口；完整配置不同的候选被单独报告，没有默认迁移或触发采集。

Docker C++ 构建通过；worker/准入留出评价及给定耗时基线保持。证据 `data/redesign/s5-cpu-cost-consumption/{verification,node-cost-verification}.json`、公开任务及模型只读挂载检查。没有新增 native attempt；保留衍生 266752741 字节，在声明的 2 GiB 内并已结算。此为周期审查后第二批代码修改，不新增 test 工程或 `.clang-format` 变更；S5/S6 仍须完成实际成本覆盖、适用性和真实闭环。

### 2026-10-13：提交点观察开销的测量边界

修正 `WorkerSampling` 的边界读取顺序：先取本线程 CPU 时钟，再读计数与墙钟；队列入口快照和提交点记录的已测读取/向量写入开销累加到共享 `CpuCounters.probe_cpu_ns`，归入下一段。workflow 对完整、无已观测等待的 worker 按段扣除成对探针计数，不按目标残差或固定系数校正，计数超过区间直接报输入问题，不截为零。原始时钟/探针事实保持，C ABI 布局不变，公共 core 不增加 SDK 分支。该修正不意味着所有时钟、拦截或采样扰动已消除。

原生观察库和 C++ 仿真在各自 Docker 内构建通过；旧零探针数据的固定候选评价完全保持，证据 `s5-worker-cost-estimation/raw-boundary-regression-evaluation.json`。已预声明训练/留出各一次、各 300 秒、各 3 次重复，新增原始 128 MiB、衍生 96 MiB，上限已追加累计记录；明细 `data/redesign/s5-worker-service-measurement/capture-plan.json`。准备时修正了输入字段和明确 prompt 来源，两次准备失败均发生在创建 native attempt 之前，未计为设备尝试；真实 token 前缀已作为完整采样请求显式提供，没有扩张采样器。训练已完成，173.953 秒；29272 个入队/worker 单元均归属，原始 CPU/等待/探针总量与分段总量守恒，探针无超过线程区间的记录。测量 worker 中 66119 段有正探针计数，总计 44.200 毫秒，占对应线程 CPU 599.951 毫秒的 7.37%；这不是 E2E 开销占比。按段扣除后形成 123 组、65250 个服务样本的固定候选。证据 `training-boundary-verification.json`；留出已完成，同样 29272 个单元无归属缺口，探针计数均未超过线程区间；候选固定后评价 65250 段全部匹配、上下文一致、原始文件分离，MAE 1.072 微秒、WAPE 12.80%，保留 `not_established`。最大误差 1.342 毫秒来自 AddRmsNorm 调用段，没有裁剪或按结果调参；它约占本次总绝对误差的 1.92%，不因此新增逐 API 探针或采样网格。完整证据 `evaluation.json`/`verification.json`。两次采样已结束，无自动重试；原始 56768433 字节、衍生 44156241 字节、原生 339.980 秒，均在声明预算内并已累计结算。此为周期审查后第一批代码修改，不增加模板/规则或 test 工程。

### 2026-10-13：worker 独立训练与留出采样

完成周期审查后，预声明两次各 300 秒、预热后各 3 次重复的 CPU 采样，总新增原始数据上限 128 MiB、衍生上限 96 MiB；记录在 `data/redesign/s5-worker-cost-estimation/capture-plan.json`，已追加累计预算，不自动重试或扩展网格。固定 SGLang/Qwen3 TP2 部署，训练请求长度 101/111，留出 107/101，均包含 Prefill 和 Decode。原观测开发数据未参与拟合。

训练采样完成，实际原生耗时 162.265 秒，29272 个队列配对均取得业务范围，其中预热 7318、测量 21954。固定候选形成 123 个操作/提交位置分组，65250 段观测；排除 120 个非完整单提交 worker、84 个操作名称未解析 worker，没有为它们补零或重采。候选已在启动留出采样前保存为 `declared-candidate.json`。留出已完成并对固定候选评价：65250 段全部匹配，MAE 1.065 微秒、带符号均值 -0.288 微秒、WAPE 12.30%；两份观测上下文一致、原始文件分离。最大单段绝对误差 175.15 微秒，主要贡献来自事件调用、HcclAllreduce/attention 准备及矩阵准备等，不能用总误差掩盖分组差异。没有按评价结果修改候选或追加网格。两个 attempt 共计原始 55145110 字节、衍生 43627150 字节、原生耗时 334.095 秒，均在声明额度内，累计资源账本已结算。证据 `evaluation.json`/`verification.json`。提交点记录自身开销未进入当时的 probe 计数，因此该结果仍是未校正观测的独立评价，不提升为纯 CPU 服务资格。本记录不证明正式成本消费或整个 S5 完成。

### 2026-10-13：worker 成本候选入口与公共统计收紧

新增 `fit-costs --unit worker`，以规范化的完整单次提交记录生成操作×before/call/after 中位数候选；固定候选评价复用同一读入合同，并检查原始记录来源不与训练重叠。等待/探针记录被明确排除，未知观测开销不扣造；保留原测量资格，不自动接入预测。原生事件名称解释留在 runtime 适配器。

准入与 worker 共用中位数及误差统计，公共 workflow 装配来源/部署上下文，两类样本由并列读取方法解释。Docker 构建通过；公共命令重跑已有准入训练和留出数据，候选与评价均与原结果完全一致，证据在 `data/redesign/s5-worker-cost-estimation/admission-regression-verification.json`。Python Ruff 与 diff 检查通过。没有新增设备采集，也没有将原 worker 观测开发数据改作训练或留出。该工作是成本候选入口交付，不证明 worker 精度或完整 S5 成本消费。本批为该周期第五批实质修改；整体职责、所有权、风格与文档审查已完成，见[周期审查](2026-10-13_code_structure_review.md#worker-测量到目标工作的交接周期)，计数归零。

### 2026-10-08 至 10-10：基线、公共合同与模板链

完成 S0–S2 历史基线，建立 C++23 正式入口及公共 serving/control 合同。之后按用户要求调整为 SGLang + Qwen3-32B 主线、成本后置，贯通 block → Python wrapper → 原生事实 → C++ extractor → 模板。原有联合模块成本及绕过正式资格的预览路径已删除，不维护并行后端。

逐步绑定模型矩阵/attention、页分配/释放、请求槽位、过滤/合并、发布与通信参数；保留来源、目标身份及端口。早期模板/目标示例属于开发过程，不证明任意目标配置可复用。早期资产路径见末尾索引；稳定机制按需查[模板关系](../redesign/sglang_template_relations.md)。

### 2026-10-11：联合 serving、控制状态及共享模板

联合观测补齐准入、transition/update、逐请求发布与批次共享输出，轻量控制器维护唯一请求/KV状态。修正了原完整输出预约等与实际主线不同的控制逻辑；原生绑定留在适配端，公共接口承担交接。

共享结构将重复 MLP 的节点/依赖、资源槽、远端别名和参数规则与来源观测分离。compiled_templates 可独立加载；预测不读取原始 DAG 或运行原生绑定，缺编译资产不回退。证据与边界见[架构审查](2026-10-10_architecture_review.md)。已有公开完整执行与固定输入回归属于局部开发证据，不能计作完整结构或成本验收。

### 2026-10-12：集合入口、多来源与代表请求

最终集合合同贯通 prepare/plan/predict、快照、挂载、报告、status/resume；4 配置 × 2 workload 验证组合隔离和 Docker 同轮恢复。证据：`data/redesign/s6-exploration-set-contract`。缺结构与缺耗时的组合分别报告：`s3-cost-input-isolation`、`s6-combination-gap-isolation`。

多来源资产按共同身份、资源角色和有限规则选择整批模板及局部供体，节点成本按实际来源查询；来源入口与供体职责分离。证据：`s4-multisource-donors/composition_verification.json`。共同来源选择不等于任意跨配置许可。

代表配置分组和三组短请求显式执行已接通，既有双请求来源在部分场景下展开三/四请求、部分退出和跨页。证据：`s3-representative-execution`、`s4-representative-partial-filter`、`s4-representative-page-crossing`。这不证明自动建库或全部模板族已经完成；配方在采集合同维护，不在日志重复。

### 2026-10-12：workspace 与统一预热

runtime 适配器采集 workspace 初值及分配/复用/扩容/释放；有限 ATB 需求规则编译入模板。WorkspaceState 独占目标容量，runtime 在整批图接纳后提交；allocator 复用共同替换器。目标扩容等待绑定实际提交前沿，不搬用供体时间。证据：`s4-workspace-growth`、`s4-fixed-warmup`。

统一预热主资产为第 69 次，几何/扩容供体为第 67 次，清理供体为第 70 次。两请求结束→Idle→四请求晚到→扩容→全部完成的开发输入通过，4 业务批次/17 动作。base 结束及组间交接以各 rank 原生清理确认，目标预测仍在最后 Publish 结束。证据：`s4-native-cleanup/{cleanup_observation_verification,dynamic_expansion_verification,full_graph_verification}.json`、`s4-group-cleanup/group_cleanup_verification.json`。

跨配置初始化未资格化；插入片段再次替换的接口限制没有被绕过。是否必须实现后者取决于承诺场景是否实际触发，不能因发现限制就新增通用图系统。第 71/72 次的 profiler 归属及时钟诊断保留原事实，未通过拟合偏移消除冲突；未归属事件数不作为完成门槛。

### 2026-10-12：耗时反馈与结构开发对照

固定资产 100ns 给定耗时完成 6 批/25 动作；1000ns 改变调度后曾触发合并限制，修正有限行数规则后完成 6 批/24 动作。2+2→4 合并、4→2 过滤及依赖链已核对。输入用于修正，归为开发证据：`s4-fixed-assets-acceptance/{graph_verification,feedback_verification,merge_structure_verification,merge_baseline_verification}.json`。这些数值为显式假设，不是成本拟合或精度结果。

第 73 次对照最初是独立验证，后用于事件归一化修正，**现在是开发证据**。validation 数据始终未导入模板或成本。具体对照见下一节，不能继续称为修改后实现的独立验收。

### 2026-10-12：后台事件调查的范围纠偏

此前沿未归属 event 销毁继续调查 watchdog/query/入口历史，曾提出核心后台工作展开。这超出了既有 modeled 范围，已撤回为必做任务。模板外服务排除，必要线程顺序通过已有投影保留；只有实际影响流程内端口或状态的新因果才需要处理。

实现中保留 event 销毁唯一范围关联、query 标量记录和入口 record/query/destroy 历史，用于来源取证。第 74/75 次 SDK 观察分别验证 query 返回值及入口集合；新建未 record 的 event 也可返回完成，不能据 query 补造 producer。证据：`s4-event-query/verification.json`、`s4-event-entry-history`。完整 serving 交接和跨入口并发未验证，但不会自动升级成主线新门槛。

### 2026-10-13：来源选择与机制报告

SGLangCollectionPlanner 共用预测所需的供体/初始化选择，workflow 不另维护一套来源筛选；成本可用性不改变结构资格。公开完整输入回归、缺成本、缺资产和池缩容路径见 `s3-source-eligibility/verification.json`。pool4→pool3 保留供体，但初始化仍报 `workspace_initial_capacity_migration_not_qualified`。

机制报告引用 12 类机制、106 个来源调用和双 rank 证据；移除 geometry 供体后，Prefill 直接释放与空释放组缺少证据。报告仍为未资格化，不自动补采、不等于目标变换通过。证据：`s3-mechanism-coverage/verification.json`。外围 workflow 仍直接使用 SGLang 类型，完整装配解耦未完成。

### 2026-10-13：推进审查与文档维护

用户指出粒度过细、模板外范围外扩、拒绝检查膨胀风险及外围装配耦合。审查确认：局部事件核对挤占能力交付，计划门槛混入未证明必要的扩展，机制报告增长快于闭环，周期审查偏依赖扫描，日志与状态重复。

本次重写当前计划、合并里程碑日志、归置配方及证据、更新范围和文档规则，区分后续代码任务与持续约定。未运行采集、构建或仿真，未修改产品代码、原始资产、third_party 或 .clang-format；原 goal 仍暂停。没有撤销必要的容量/端口/状态条件，也没有宣称 S3–S6 完成。已检查 redesign 两目录的本地 Markdown 文件链接及锚点、过期状态表述和 git diff 空白问题；相关检查通过。

### 2026-10-13：恢复 goal，完成 C++ 外围装配适配边界

前次文档整理属于有效进展，随后状态答复没有实现进展。用户现已恢复 goal。本批关闭 PredictionWorkflow 直接解释 SGLang 的耦合：入口显式注入 PredictionAdapter，现有 SGLangCollectionPlanner 实现来源配置解析、公共控制参数和模板选择接口；公共来源候选/选择类型替代框架专属类型。配置仍在资产加载错误边界内解析一次，bank与配置由workflow持有，只读视图在加载结束后建立；控制器保持公共构造及每组合独占状态。

预测入口不再依赖 Planner 或具体框架头文件，已有采集计划作为报告从入口传入，不参与模板变换。来源资格和缺口沿用实际规则，没有新增拒绝清单、控制器回调、全局注册器或兼容路径。随后 Planner 的控制路径判断和代表配置分组也改为消费入口注入的同一接口：按框架分派配置视图、汇总组及声明顺序的排除项，不复制来源选择或支持规则。采集准备和配置快照随后迁入 CollectionAdapter，SGLang/KT 各维护原生参数与插桩描述；公共 Planner 只组织请求、权限和路径，采样配方仍复用原实现。两类接口分开是因为 KT 已有真实采集而没有预测资格，不为它添加假预测实现。提取/编译与配置输入在本里程碑后续一并迁移，见下文。

Docker C++23 构建、修改文件格式及 git diff 检查通过。公开 predict 复用三来源完整输入、初始化缺口、缺资产和 KT 现有示例：完整输入及两条缺口的全部报告除宿主加载时间、此前新增的机制字段外与原证据相同；机制字段另与既有机制报告逐项一致；KT 两配置仍报告未实现。完整结构指标/端口摘要保持，但本批未导出全图，不宣称逐边或独立原生验收。证据：`data/redesign/s4-prediction-adapter/verification.json`。

规划交接另以修改前后的公开 plan 完整报告对比 SGLang、KT 及现有资产配置输入，全部一致；缺资产 predict 完整报告保持，prepare-representative 消费同一三组/六请求配方。Docker构建、格式及diff检查通过，证据 `data/redesign/s4-planning-adapter/verification.json`。本轮只验证规划/准备交接，不声称原生采集或目标结构扩大。

采集准备交接以两框架的 prepare/prepare-prediction、SGLang prepare-target/prepare-communication 修改前后完整JSON对比，结果一致；plan与代表配方也保持。通信例子未包含测量任务，不据此宣称采样执行通过。KT目标验证仍拒绝，错误文字改为不含框架名的能力说明。Docker构建、格式与diff检查通过，证据 `data/redesign/s4-collection-adapter/verification.json`；未运行原生worker，不扩大KT预测支持。

提取/编译装配随后复用同一模板接口：PredictionAdapter/SGLangCollectionPlanner 按实际职责更名为 TemplateAdapter/SGLangTemplateAdapter，删除旧文件和引用，不新增独立生产接口。SGLang实现组合既有框架绑定、Ascend提取器和模板生产；公共workflow维护编译产物发布与失败失效。未知框架在生产前报能力缺口，不增加后台模拟或新变换。

公开 extract 重用第70次base，实际执行提取和编译；source DAG、模板实例及compiled_templates分别与修改前逐字节一致，stderr空。证据 `data/redesign/s4-extraction-adapter/verification.json`。Docker构建、修改文件格式及diff检查通过。按五批约定完成职责/所有权/调用链审查，记录于代码质量审查；审查当时剩余配置输入解析耦合，随后按既有 CollectionAdapter 处理。

最后将 ProjectReader 移至 workflow；公共文件、整数与字段校验归入 contracts/json_input，SGLang/KT 参数解析归入各自适配器。入口拥有适配器并向读取器注入只读视图，框架配置值类型保留在合同中；没有新增框架实现、插件平台或支持判断。Docker 完整构建通过，两框架 plan/prepare/prepare-prediction 完整 JSON 保持，两个非法输入的错误保持；已有三来源条件预测完整报告除宿主模板加载耗时外一致。证据 `data/redesign/s4-configuration-adapter/verification.json`。这关闭现有 C++ 外围业务耦合，不扩大目标结构支持范围。

没有原生采集，尝试数仍75；配置交接回归派生384087字节已纳入账本，总派生5446906824字节。模板生产装配时完成五批周期审查，配置输入迁移为此后第1个实质代码批次。S3–S6继续进行，未写完成总结。

### 2026-10-13：跨配置预热入口的有界机制核对

现有固定配方来源均为 request pool=4，没有 pool 容量变化后的入口证据。原生 EagerRunner 会按容量建立静态缓冲，但 BaseRunner.warmup 对非 CUDA 直接返回；项目单 token、两输出的预热另外执行。因此不能由“同一配方”直接推导相同 workspace 初值，也无需逐配置重新建模板库。

本次只安排一次公开 observe-target：保持模型、设备、TP及其他参数，容量改为3，统一预热后执行一个单 token、两输出请求。用途是核对初始池、预热记录与实际 workspace 分配，属于开发证据，不导入模板库，不充当独立结构验收。原生额度220秒，无自动重试；预计原始产物不超过100 MiB、派生不超过160 MiB，使用现有剩余额度（原生约275秒、原始约437 MB、派生约190 MB），不提高累计上限。后处理一次；失败或不一致即停止该观测并分析原因，不继续枚举容量。证据目录 `data/redesign/s4-initialization-transfer`。

第76次完成但不合格：调用遗漏 MARKOV_IMAGE_PREFIX，实际使用默认旧镜像，workspace_probe=null。两rank快照容量与来源相同，但不能证明完整分配机制；保留记录，不修改其原始环境或填造安装来源。原生消耗163713579660 ns。正确镜像为 markov-workspace-20261012-sglang:ubuntu22.04，已在无设备启动的只读检查中确认包内安装来源。仅重做同一对照一次，新增原生总额度220秒（累计上限1960秒），存储沿用原上限；新尝试220秒内结束，不自动重试。这是启动参数纠正，不扩展采集配置集或模板库。

第77次在正确镜像下完成，原生耗时160270163980 ns。两rank统一预热入口与pool-four均为190840832字节，短请求的256条workspace记录全部reuse、都有owner及来源区间，结束容量不变，归一化没有workspace缺口。观测中其他范围外/未归属诊断不升级为本次门槛。结论仅为该容量变化的开发对照，未修改原始数据、未导入模板，也未据此放开任意容量；该观察用于后续生产端初值适用条件，核心只消费模板声明。证据 `data/redesign/s4-initialization-transfer/verification.json`；原生实现依据见同目录 mechanism_review.json。运行文档补充实际镜像前缀，并删除重复的旧任务顺序。

生产端随后发布独立 initialization_pool_domain：固定 eager dense 路径、统一预热及合格 workspace 记录下，沿用请求表行缩小域；核心查询模板声明，删除直接比较来源/目标容量的条件。编译资产同步保存该事实，三个既有来源已重新 compile-templates，无旧格式回退。Docker构建、格式及diff检查通过。原配置完整报告除宿主加载时间外不变；pool-three下三请求序列完成4批/16动作，图无环、连接无缺口。原四请求序列已执行3批/12动作，随后在已有运行请求时新准入一个请求的Prepare处返回batch_operation_variant_not_observed；单token短请求则超出当前长度域。两者如实保留，未因三请求成功缩小原任务，下一步优先解决前者的模板变换交接。证据 implementation_verification.json（同目录）。这关闭初值交接的代码缺口，尚非全部跨配置结构验收或性能预测。

继续读取四份现有编译资产，Prepare/Prefill 中需替换的角色确定为 LastLocationConcat：当前三来源只有2–32成员的ConcatD变体；历史 s4-release-list-domain base 有单成员Cat的INT64拷贝变体。历史来源与目标仅max_running_requests为2→3，其余配置、模型及设备相同，但来源筛选的整池容量资格和bind_sources的全配置相等均阻止接入。证据 prepare_variant_inventory.json（同目录）。下一步先按当前合同重编译该历史base，再只为生产端声明不依赖池容量的局部片段接入供体，不把整份较小池模板整体放行，也不复制验证记录充当base。此为已有资产/代码机制核对，没有新增采集、规则或测试程序。

历史base按当前compile-templates重编译时，旧图因缺少scope字段失败，入口已撤下其compiled_templates引用，未把旧文件继续宣称为可用。原始观测保留，改用一次公开extract重提取。旧图/模板约549/96 MiB，新产物将写入该base自己的目录；允许新增派生上限768 MiB，累计派生上限6442450944字节，原生时间与原始数据额度不变。取得当前合同产物即停止；失败不自动重试、不转为补采。记录历史目录原有文件大小，仅累计新增/增加的派生产物，不复制原始观测，也不填造scope字段。预算记录 historical-reextract-budget.json（初值迁移目录）。

一次extract及随后的编译成功，重建图无环；新增/增加的派生产物675049948字节，在768 MiB额度内。公开predict能加载重建资产，来源自身静态资格见historical-reextract-verification.json，缺少初始化及workspace窗口记录仍如实保留。加入完整历史来源后，原四请求目标仍返回同一Cat变体缺口，未修改筛选条件或把加载成功当作目标通过。该恢复步骤未修改C++/Python源码，原生尝试仍77。

随后接入统一局部操作来源：生产端 `request_pool_rebindable` 声明可按目标池消费的操作，适配器选择来源，核心沿现有操作替换及参数求值路径消费。Cat、页表除法与logits重排不依赖池总容量；relay/请求表写入必须为所有设备成员绑定容量、批次轴及目标槽位。曾用“容量无关”描述该资格，但写入操作实际依赖目标容量，因此收敛为同一个重新绑定合同，不另加写入特例入口。来源定义由工作流持有，局部来源不进入整批、KV区域或初始化供体集合；调度器无框架分支。四份既有资产重编译，未新增采集、模板定义或兼容解析。

公开输入确认：原四请求pool-three场景完成单成员Prefill，推进至4批/16动作、条件时间4461900 ns，图无环且无未解析连接，实际图包含历史base来源。历史base未进入整批或初始化供体；缺少其耗时时返回needs_data / given_node_duration_missing，并标明该来源及span 233030。提供显式100 ns服务耗时后，下一阻塞为Schedule的2+1合批转换 `batch_transition_variant_not_observed`，仍为unsupported，不宣称完整场景通过。原配置完整报告除宿主加载时间和新增来源说明外一致，完成4批/16动作。证据 `data/redesign/s4-initialization-transfer/fragment-verification.json` 及对应 fragment-{target,original,cost-missing}-result.json；均为开发回归，不是独立结构验收或真实成本预测。Docker构建、修改文件格式检查及diff检查通过。

继续核对合批机制后，确认现有BatchLayout已同时绑定MergeLeft/Right/Total，生产端也已资格化整个非空合批前缀；消费者却额外固定pending和右侧人数。`matches_transition`现允许两侧行数变化，仍要求pending过滤全保留、右侧等于pending人数、左侧等于入口running人数且两侧非空，不放宽部分过滤或其他分支。未增加模板、采集、产物字段或变换入口。原四请求pool-three输入完整完成5批/20动作，四个请求均结束；条件时间5458000 ns、图无环且无未解析连接。原配置完整报告仅宿主加载时间变化，Docker构建及格式检查通过。证据同目录 `merge-verification.json`、`merge-target-result.json`、`merge-original-result.json`；原生实现依据为ScheduleBatch.filter_batch的全保留提前返回与merge_batch的双操作数拼接。此为开发回归，未证明独立结构资格或性能精度；后续固定资产和规则进行独立对照，不沿失败输入继续累积“验收通过”。



### 2026-10-13：固定规则后的请求池缩小独立结构对照

固定当前四来源资产、片段重绑定和双侧合批规则后，选用未参与修改的四请求输入：长度125/123/117/120、输出5/4/2/3、同时到达，目标pool-three，其余主线配置不变。先展开预测，再运行一次observe-target，统一预热；核对流程成员、目标参数、必要依赖及KV/workspace状态，不以给定耗时评价性能误差，也不核对模板外全部事件。验证来源不得导入模板或成本；若用于修改，立即转为开发证据，不宣称独立通过。

本次原生额度220秒、原始产物200 MiB、派生768 MiB（包含目标图和必要观测处理），一次后处理，无自动重试。原生累计上限调整为2008286473050 ns，派生累计上限7084835496 bytes，原始上限2147483648 bytes不变；这是固定规则后的独立验收额度，非补齐模板或性能采样。预算及输入在 `data/redesign/s4-pool-transfer-acceptance`；失败或超额保留结果，不继续扩额追求通过。

第78次原生观测一次完成，耗时183157491420 ns，使用markov-workspace-20261012镜像，未修改规则或模板。新输入预测完成6批/24动作；两个rank的48个流程调用，其请求成员、input/context/generated进度与发布结束状态一致。按动作/rank比较10466个kernel的名称、输入输出shape/dtype/format，差异为零。原生槽位与目标槽位保持双rank一致的一一重命名，释放后复用无冲突；两rank workspace预热初值190840832、结束197132288字节一致。证据 `structure-comparison.json`、`prediction-result.json` 及原生validation manifest（同目录）。这些比较不证明全部依赖、传输或真实成本；下一步核对本次变换相关的传输与关键依赖，不重采、不以所有未归属后台事件为门槛。当前保留独立用途，未导入预测来源。 本次原始产物实际268178383字节，比200 MiB预留多58463183字节，累计原始上限超2041883字节；原生时间与派生在预留内。原始产物估计偏低，保留证据和原上限，不改变计量口径或自动扩额；停止新增采集，仅继续已有产物比较，开销超额作为S3未收口项。详见resource-verification.json。

继续只读核对现有产物：70次普通复制的方向/字节数、1560次逻辑通信的操作/类型/算法/元素数，以及15588个通信任务的操作/字节数/类型/两端rank/传输方式，按动作与rank比较均无差异。目标图中10510个Schedule/Prepare/模型计算与复制节点均可达对应Published；Publish自身有20个异步尾部，有后续模型消费者的16个均先于后续模型kernel。初始检查曾要求所有Publish内部工作先于Published，这比既有异步KV合同更强，已纠正分析条件，未修改代码或补同步；4个终局尾部在该输入中没有后续消费者。证据 `transfer-comparison.json`、`communication-comparison.json`、`publication-dependency-verification.json`。

本次独立证据支持新输入的统一流程、目标工作量及发布/后续使用的关键依赖；kernel/通信描述按动作分组比较，不声称逐CPU服务或所有通信内部边与原生逐项等价，也不证明真实性能精度。维持原生规则、成本及模板资产不变。下一步按S3/S4退出条件归并已有证据，核对耗时反馈覆盖与有界建库/模板族开销，而非继续扩展本次逐事件检查。


### 2026-10-13：阶段证据归并与模板库现状核对

当前程序重跑既有耗时反馈输入，仅节点给定耗时与输出位置不同，配置、请求和模板来源一致：100 ns完成6批/25动作，1000 ns完成6批/24动作，均无缺口。前者在20 ms后续到达前进入空闲，后者因实际完成事件推迟进入不同合批路径，不是固定图时长缩放。证据 `data/redesign/s4-stage-evidence/feedback-verification.json` 及两份公开入口结果；旧feedback_verification保留当时失败，当前状态以本记录为准。本输入继续属于开发回归；第78次独立观测的用途不变。

同目录 `template-library-inventory.json` 根据当前公开资产报告和编译文件核对：4来源、238份原生流程实例，7424个共享模型区域使用80个资产内结构定义，编译资产254540525字节。TemplateGraph实际通过共享结构解析节点、依赖、参数规则和接口，并保留实例的来源绑定；80是逐资产定义数之和，不是跨来源唯一族数，也不是238份流程模板已全部归并。未新建模板、规则或采集。

S3后续围绕有界建库交付：现有三组完成后再提交的短配方未覆盖排队后的单成员Prefill及非空running合批，不能据已有四来源预测成功宣称配方完整；需先整合既有机制覆盖和可复用来源，再明确必要采集需求及停止条件。来源实例、共享结构、有限变体与规则分别核算，不通过按请求长度新增定义补齐覆盖。原始开销超额仍保留，当前不新增原生采集。

### 2026-10-13：采集保留与第五批周期审查

原始产物超额主要包含vendor重复分析中间文件。核对安装的torch-npu `_profiling_parser.simplify_data`：清理发生在导出后，移除summary/timeline及分析输出，保留公开trace、kernel CSV、通信数据库、host/start_info和device信息。SGLang显式关闭该选项，项目AscendProfileWindow现仅在自身窗口中覆盖 `data_simplification=True`；保留Level2，没有自建删除器或改third_party。按第78次真实文件分析，36份中间文件共67473656字节可被该选项清理，12个已发布trace/元数据引用均不在清理范围。该数字是保留分析，不是新采集实测；历史文件未删、累计用量未扣减。证据 `data/redesign/s3-profile-retention/retention-analysis.json`。

本批达到五批周期，已完成[代码结构审查](2026-10-13_code_structure_review.md)：核对C++输入→适配→模板→控制/执行及Python采集/归一化职责、所有权、风格和重复逻辑。删除同一定义从整批与局部来源重复进入操作候选的问题；两个公开预测的完整结果除宿主加载时间外不变。容器构建、Python全包Ruff、语法和修改文件格式检查通过。未新增采集；产物简化尚待未来有界采集验证，不据静态核对宣称原生运行已验证。

### 2026-10-13：补齐代表配方中的排队触发逻辑

原三组配方依靠组间清理屏障，不能据此触发非空running合批。生产适配端增加第四组：池容量N时提交N+1个短请求，以输出进度差异释放一个位置，再准入排队者。配方共N+7请求、5N+12输出token；N≤32复用已有last-location Concat资格边界，不新增预测黑名单。输入按原生页大小解析，Prefill预算NP、token容量4NP是该配方的保守执行前提，不是目标成本或KV公式。机制报告同步增加单成员Prefill、running合批两项；仍不把事实出现当成目标变换资格。

容量3/4的公开prepare-representative分别生成10/11请求、27/32输出token，组索引及身份一致；原配置公开predict的全部组合结果与修改前一致。容器构建与修改文件格式检查通过。证据在`data/redesign/s3-queued-recipe/{pool-three,pool-four,prediction-result,verification}.json`。本批没有新增原生采集，第四组仍待真实执行证据；新增候选不触发自动补采，也不替代已有资产覆盖和模板族交付。本批为最近周期审查后的第1批代码修改。

### 2026-10-13：已有资产交接，排除重复采集需求

核对当前14项候选机制报告及固定资产的独立展开证据：13项有双rank来源调用；缺少同人数来源的单成员Prefill不等于缺少生成能力。独立pool-three场景的work 15在两rank均由四成员Prefill来源变换为单成员，原生call 1048对应同一请求；work 17的2+1合批也由既有模板/片段展开。已核对来源实例、目标调用及原生批次，并沿用既有完整流程/计算描述对照结果，未将验证数据导入模板。

交接结论：上述场景的单成员Prefill与合批无需新采集，第四组配方保留为从零建库候选，不能因来源清点缺项机械重跑。证据在`data/redesign/s3-asset-handoff/existing-asset-coverage.json`；这不证明所有目标域或新配方原生执行完成，自动复用决策与模板族交付仍待实现。本次只读已有资产并更新文档，没有代码修改、新采集或再次运行仿真，周期计数不增加。

### 2026-10-13：完整代表配方的一次建库验收预算

已有目标无需补采，但四组配方及产物简化没有完整原生执行证据，阻塞从零有界建库的交付。本次固定pool-four/TP2、11短请求32输出token，使用统一预热与四组清理屏障，只允许一次base尝试；不导入验证资产、不因缺口修改请求重试。原生额度300秒，原始保留预留768 MiB、派生预留2 GiB，按相对上一完整serving观测的批次数留出余量，实际开销仍需实测。到时、失败、机制缺失或存储超额均停止，不把本次成功等同于所有配置资格。

预算及原始累计基线保存在`data/redesign/s3-bounded-library/budget.json`，账本单独追加额度；此前原始超额2041883 bytes保持记录，不通过删除/重分类清除。此次为建库交付验收，不是填补已验收目标的单成员Prefill或合批模板。

实际结果：唯一一次原生尝试完成，耗时197521918630 ns；四组清理均获双rank确认，13个完整批次中第四组形成4成员Prefill、排队单成员Prefill及3+1合批。公开资产加载的14项机制均有双rank来源。产物简化保留12个必要引用，指定重复目录均已去除。取得112份实例、3328区域共享28定义，编译资产116878590 bytes。原始保留440562805 bytes、本目录派生1178754081 bytes；原生结束至后处理发布242744633380 ns（含编排，非独立CPU计时），编译自身56279324490 ns，均在本次预留内。

完整配方能取得机制事实，但还不能单独作为仿真启动来源：公开predict返回`region_exit_relation_not_supported`，供体可选而workspace绑定/初始化资格未通过；未提供耗时，不宣称性能结果。按一次尝试停止，不重采、不修改配方重试；后续已定位：flow 47202从缓存复用探针interval 102913（scope 0:121）指向HCCL envelope 171553；原生connection 76814的唯一CANN提交217083却属于scope 0:152。原始trace确实包含这条vendor派生连线，并非公共region切分凭空增加。证据`workspace-flow-investigation.json`保存在同目录；修复应在Ascend关联适配端复用connection/queue身份，不能放宽公共出口资格或仅删除出错边。尚未修改实现或重提取，原始数据保持。证据为`data/redesign/s3-bounded-library/{native-recipe-verification,library-verification,asset-result}.json`及其capture目录。累计第79次（管线61、独立18），本次无自有代码修改，周期计数保持1。


### 2026-10-13：以原生提交与队列身份修复派生flow

Ascend归一化现以目标connection ID定位唯一CANN提交，再由同线程Dequeue和correlation ID定位唯一Enqueue；原派生来源不能包含已关联的Enqueue时修正来源并保留profiler原引用。缺少唯一身份链时不猜测，不按workspace探针名字或样本编号放行。公共region出口及workspace顺序规则未改。

原始采集不变，重归一化实际只修正1条flow；通信scope从0:121恢复为原生提交的0:152，conflicting_flow_ownership消失。Docker公开归一化、extract和predict确认初始化available、双rank1664个workspace使用点绑定通过、2个需求定义复用，14项机制保留。全包Ruff、Python语法及diff检查通过。证据`data/redesign/s3-bounded-library/{flow-rebinding-verification,reextraction-result,rebound-asset-result}.json`。

单库随后使用显式1000 ns假设耗时运行既有延迟请求输入，在首批前返回workspace_use_order_not_qualified；见`expansion-{project,result}.json`。这不是成本结果，也不证明单库动态展开已经合格；下一步检查变换后的同池依赖，不新增采集。本批为周期审查后第2批代码修改。

### 2026-10-13：模板内部保留原生线程路径，单库完成动态展开

首批workspace顺序失败定位到同一提交线程的两个使用点；原生线程路径经过150个无归属CPU观测，其300个边界不属于模板。后续装配已有跨未建模事件的线程投影，但较早的状态资格看不到这条内部关系。公共BlockExtractor现仅沿真实ThreadOrder边跨过无归属点，到下一有归属点即停止；同block端点输出internal_thread_paths。TemplateDefinition将其纳入普通内部依赖，让状态资格、切分及执行共同消费，不导入被排除工作或耗时，不越过其他block。未编译模板需重新extract生成该字段；不增加兼容分支。临时定位代码已撤回。

重提取同次原始数据后，74条路径涉及12份实例，源码图仍为1160616个span。仅使用一个编译来源和显式1000 ns，既有延迟请求与pool-four→pool-three均完成6批/24动作，分别64538000/64502000 ns；图无环、连接全部解析。延迟输入的metrics与execution_summary和原多来源结果完全一致。这是开发结构回归，不是真实成本预测或完整域独立验收。

证据在`data/redesign/s3-workspace-order/{source-boundaries,projection-inventory,verification,expansion-result,pool-three-result}.json`。Docker构建、公开extract/predict、修改文件格式及diff检查通过；无新增采集。当前为周期审查后第3批代码修改。

单库后续证据归并：固定相同资产和请求，只把给定耗时改为100 ns，公开预测完成6批/25动作、23212500 ns；1000 ns为6批/24动作，100 ns结果的metrics及execution_summary也与原多来源结果一致。pool-three新图与此前已对应独立原生观测的目标图相比，24动作的请求/过滤/合批/准入/KV状态、10466个kernel、1560次逻辑通信、15588个物理通信任务和70次普通复制描述一致。新图10510个发布前计算/复制节点均可到达Published；26个Publish计算/复制节点中，20个有下一模型调用且均先于其全部kernel，末批6个无后续消费者。后者按结构选择全部Publish设备工作，不用假设耗时下是否晚于Published来筛选；Published仍不是清理屏障。

证据为同目录`feedback-and-reference-verification.json`、`single-library-{flow,device,communication}-comparison.json`及`single-library-publication-dependencies.json`。结论限于这一既有独立原生对照场景的设备描述与关键边界依赖，不宣称全内部边等价、完整支持域或真实性能通过。本次没有代码修改或新采集，周期计数保持3；本场景结构核对到此收口，诊断图与100 ns输入计入派生开销。


### 2026-10-13：S3/S4交付核对与计划收紧

按当前代码及单库证据重整实施计划：已完成的模板追溯、单库展开、耗时反馈和既有独立参考比较不再混入待修复项；模板族交付、开销口径、资产复用与自动建库交接继续保留。范围核对直接依据SGLangBindings的控制/配置关系、库中实际长度域和公开结果，不能把外层长度区间当作所有kernel/成员组合保证，也不降低原阶段条件。

确认两处交接问题：PredictionWorkflow仍以representative_collection_planning_not_implemented结束缺来源路径；代表选择在同请求池容量时保留首项，可能忽略同组更充足的Prefill预算。后续先解决已有候选的可执行选择与预算轴证据，不扩张最优base搜索，不重新检查已收口的单库对照。初始采集总开销已有记录，初始化/预热仍未各自计时。

同时修正生产端过期的“只支持采集批人数”说明，变换报告改用qualified_prefill_lengths及qualified_batch_membership；这是说明与现有资格逻辑对齐，未新增放行规则。已构建并检查格式；既有编译资产的历史说明不手工覆写，下次重新生产时刷新。该报告字段修改计作本周期第4批；本轮无采集或模拟重跑。

### 2026-10-13：代表选择与Prefill预算轴交接

同请求池容量下改为优先选更大Prefill预算，完全相同再保留声明顺序；最大池优先保持，未添加最优搜索或派生配置。公开plan在先声明预算256、后声明16384时选中后者，prepare-representative生成同一四组11请求配方。准备成功不代表原生执行通过。

固定单库、同一四请求输入和显式1000 ns节点耗时下，预算256/16384分别完成6批24动作与5批20动作。复查AdmissionWork与生产绑定确认：接纳后InputBudget停止已属于公共CPU参数域，配方缺少同名机制事实不是重新采集的理由。此次未改变准入、变换或耗时规则；没有运行原生采集，也未建立新的独立原生对照。证据为`data/redesign/s3-prefill-budget/{plan,prepared,result,verification}.json`。S3自动建库交接及S4独立验收仍按当前计划推进。

该修改为本周期第5批，已完成沿输入、生产、控制、执行及Python采集生命周期的结构审查，见[当日审查](2026-10-13_code_structure_review.md#第二周期代表配方到预算轴)。修正文档中过期CPU参数域，删除normalization方法间多余空行；未改变.clang-format、增加检查器或新增测试。Docker C++23构建、修改C++格式、宿主Python全包Ruff及diff检查通过；modeling容器未安装Ruff，未为此改动镜像。周期计数归零；原生次数及原始开销不变，本次派生产物计入累计账本。

### 2026-10-13：自动代表建库、资产复用与中断恢复

新增编排已接入已有采集生命周期，安排一次真实公开predict验证：同池预算256/16384配置组从无资产开始，只采代表16384，导入生成库后重新预测；缺节点成本即停，不补采结构或伪造耗时。同时以已有库检查允许采集时仍优先复用。一次原生尝试上限300秒，新增原始预留768 MiB、派生预留2 GiB；失败、预算或规则缺口即停，不自动重试。此采集验证持久化交接，不扩张机制或规则。额度及累计基线为`data/redesign/s6-representative-handoff/budget.json`，历史超额不抹除。实际公开入口验证完成：已有库且缺成本时0采集；声明未编译资产时返回template_asset_repair_required且0采集；无资产时自动选择预算16384代表并完成一次采集。主动终止宿主编排后原容器继续运行，live resume保持同一attempt；终止后resume完成后处理与manifest导入，再运行第二轮预测。两个配置均在node_durations_missing停止，未伪造耗时或补采。完成后再次resume保持1次采集、2轮预测及累计用量不变。

此次原生197117218910 ns，原始441934461 bytes，派生1184244746 bytes；原生结束至后处理发布约274秒，其中模板编译约89秒，编译资产116938656 bytes。开销包含该次生命周期验证，不把额外一次验证说成产品每次需要多次采集。初始化与预热尚无分段计时。证据为同目录`verification.json`、`result.json`、`live-resume.json`及`finished-resume.json`。当前验证只有一个配置组，不证明多个独立组的失败隔离、受影响组合重算或真实成本闭环；这些仍保留在S6。

实现仅在PredictionWorkflow接通已有CollectionWorkflow，未新增执行平台、兼容路径或采集重试器。Python全包Ruff、容器内语法检查、公开生命周期及diff检查通过，相关使用合同已同步。该批为新周期第1批；未改C++、.clang-format、third_party或原有采集数据。

### 2026-10-13：配置组失败原因隔离

代表记录改为配置组代表到失败原因的映射，成功/已启动保留空原因；不再同时维护尝试列表和第二份失败账本。配方不可执行时记录该组原因并继续查找其他组；已终止的原生失败同样归属该组，不自动再试。结果发布按组关联原因，不把首组失败写入其他配置的缺口。

公开predict使用三个部署组：池1得到single_request_recipe_not_defined，池33得到queued_recipe_exceeds_bound_concat_branch，已有库的池4只得到node_durations_missing。零原生采集、一轮预测，完成后resume保持结果和用量。证据为`data/redesign/s6-group-isolation/{result,resume,verification}.json`。这证明配方失败隔离及已有资产不受污染，不证明真实原生失败后继续下一组、多个成功组或后处理失败隔离。原生次数不变，派生产物已累计。

Python全包Ruff、容器内语法及diff检查通过；本批为当前周期第2批。未新增测试、分支阈值、兼容实现或采集；受影响组合重算仍未实现。

### 2026-10-13：模板族与参数关系交付清单

根据已有生产/消费代码及第80次单库，补齐采集合同中的语义职责、真实变体、目标参数与实现落点清单，覆盖Schedule、准入、Prepare/KV、局部操作、模型、共享区域、Publish及运行环境绑定。不新增模板族枚举、独立检查器或逐长度定义；删除该合同残留的自动建库未接通及过期实例数表述。

现有库112份流程实例来自13个模型批次×4步骤×2rank及8次idle Schedule；18个step/phase/rank桶不等于结构族。8914份内部区域中attention与feed_forward各1664次，共3328次区域引用28份共享局部结构。模型/attention/通信参数定义分别44/3/4，观测引用19148/3328/3380；规则定义数不是kernel种类数。未共享serving区域仍保留来源实例，不宣称完整整批去重或任意目标支持。

证据`data/redesign/s3-template-families/inventory.json`，规则正文见[采集合同](../redesign/04_collection_templates.md#模板族变体和参数关系的维护清单)。本轮仅解释现有资产并维护文档，无代码修改、模拟重跑或采集；代码周期仍为2。支持域与独立验证继续按S4完成条件收口，不能以清单交付替代结构验收。

### 2026-10-13：采集开销分段记录

SGLangCapture沿现有Engine、统一预热、profiler和请求执行边界记录单调时钟，成功时写入已有native_environment产物。七段覆盖初始化、请求准备、预热、预热后设置、profiler启动、观测/清理、停止/导出；其和仅覆盖Engine构造前至stop_profile返回，不冒充完整进程时间，不进入目标成本。外层attempt和后处理账本保持原职责，失败不伪造完整分段。

宿主Python全包Ruff、modeling容器语法及diff检查通过。没有启动原生采集或修改历史环境数据；新字段尚无实测运行证据，随后续必要采集验证，不为字段单独采集。本批为当前周期第3批；S3总体轻量结论仍以实际原生、存储及后处理总量为准。

### 2026-10-13：Prefill预算轴独立结构验收

固定现有单库与规则，采用未参与修改的3请求序列（prompt 101/111/107、输出3/2/4，同到达），目标请求池4、Prefill预算256。先公开展开，成功后才做一次validation角色原生运行；核对批次/请求进度、变换后的设备工作及关键端口，不以完整原生事件一致为门槛。验证资产不导入模板或成本；发现差异先记录，不修改规则后仍把本输入称为独立验收。

单次原生预算300秒，原始预留512 MiB、派生预留1 GiB，累计基线及停止条件在`data/redesign/s4-prefill-budget-acceptance/budget.json`。首次失败、预算到限或结构差异即停止，不循环采集。此任务验证预算配置轴，同时自然取得已有开销分段字段，不为计时单独采集。

结果：5批20动作、40个跨rank流程调用的请求进度/发布一致；10个准入入口、候选/请求检查、队列及horizon工作一致。首批两rank均在成功接纳两个请求后返回OTHER，对应目标InputBudget停止，没有把OTHER本身当通用等价条件。8918个kernel按名称/类型、输入输出shape/dtype/format一致；70次runtime复制方向/字节、1300个逻辑collective及13506个物理通信任务描述一致。目标图22470个发布前计算/复制/通信节点均到达Published；24个Publish设备节点中18个有后续模型，均先于下一模型全部kernel。未检查完整内部CPU/通信边或真实性能精度，不以耗时匹配代替结构。

验证期间没有修改模板、规则或成本参数，validation资产未导入生产。最初描述符比对包含目标额外语义注释字段，已统一为原生CSV实际提供的物理字段；这仅修正分析口径，未修改产品或目标。证据为同目录`verification.json`及`structure-comparison.json`、`communication-comparison.json`、`admission-comparison.json`、`publication-dependencies.json`，输入继续保留独立用途。本场景在该范围内收口，不继续逐内部事件扩张。

本次原生180550030360 ns，结束至后处理发布27730872770 ns；原始172370306 bytes、派生398053732 bytes，均在预留内。环境分段实测初始化约38.8秒、统一预热46.1秒、观测/清理15.4秒、停止/导出10.6秒；其余启动及产物处理在外层总量，不能用分段和冒充完整原生时间。本次是validation运行，分段字段沿共享SGLangCapture取得，不回填旧base数据。累计第81次（管线63、独立18）；无代码修改，周期仍为3。

### 2026-10-13：S3/S4主线阶段验收并进入S5

逐项核对实施计划的S3四项、S4四项完成条件，回读对应生产、控制、执行代码及单库/独立预算轴/池缩小/耗时反馈/采集恢复的原证据。结论是既定主线及实际规则域内阶段交付通过，可以进入S5；不以节点数量或complete字段单独判定，也没有增加范围拒绝条件。完整依据见[结构阶段验收](2026-10-13_structure_stage_acceptance.md)。

S5从目标节点工作描述与正式成本查询开始。NodeDurations仍是按manifest/span给定值，不能直接当真实性能模型；CPU、计算、通信和复制的服务边界、独立测量及适用域继续未完成。S6多个成功组、原生/后处理失败隔离、受影响组合重算和真实成本闭环保留，整个goal仍active，不写implementation_log完成总结。

进入阶段前完成代码层次、所有权、Python生命周期与成本边界审查，见当日审查第三节；周期计数归零。本轮无源码修改、模拟重跑或新增采集，累计资源不变；文档本地链接及diff检查通过。

### 2026-10-13：S5目标服务工作查询接入

ExecutionGraph为Service节点构造借用型ServiceWork，携带来源追溯、rank及实际目标kernel/通信/复制/发布描述，NodeDurations改为消费这一查询。仍按显式来源span给定值执行，不冒充真实成本模型；缺给定值时公开service_work，成本调用方不再只能看到来源编号。逐成员发布只携带该成员工作，避免描述成整批输出；等待与annotation不进入服务查询。当前一般CPU服务还没有完整的目标工作特征，留作S5后续交付。

Docker C++23构建及修改文件格式通过。完整给定耗时公开predict与既有输入的指标/执行摘要保持，组合报告差异仅为本轮关闭完整诊断后的artifact及execution_details字段；另在同一正式输入中留空一个矩阵节点成本，结果为needs_data/given_node_duration_missing，报告work3/source_span172320与目标kernel完全一致，输入shape为212×5120和5120×5120。证据`data/redesign/s5-service-query/verification.json`；这是条件执行回归，不是测量模型或成本精度验收。

同时审阅现有独立设备及通信采样manifest：前者仍有profiler开销未校准、通信服务未分离，后者仍有collective准入偏斜与服务未分离，两者readiness均为needs_data，未直接导入为合格节点成本。记录在同目录measurement-boundary-audit.json。后续先落实实际性能单元及测量边界，再接匹配/估计，不能把未知包络默认为服务。

随后补齐已有CPU阶段的模板交接：SourceGraph统一解析/输出SourceCpuPart，TemplateNodeObservation和编译资产保留control_part，普通及插入节点将实际来源阶段传给ServiceWork。没有新增CPU细分或按比例分摊阶段总耗时；旧粗阶段CPU样本仍有非runtime等待未分类、探针开销未校准及内部节点测量边界不匹配问题，不能直接作为合格成本。

当前单库从已有源图公开compile-templates重编译后，缺耗时查询的work1/rank0/source_span740918返回cpu_service/schedule_prefix，与源图cpu_control阶段一致；完整给定耗时的指标、执行摘要、状态及timing_basis与前轮完全一致。证据`data/redesign/s5-cpu-service-role/verification.json`。Docker构建、格式与diff检查通过；资产格式只保留当前合同，历史资产需显式重编译，不增加兼容读取。

以上连续交接实现为阶段审查后的第1–2批；无原生采集、未改.clang-format或原始数据，派生输入及重编译资产已计入累计账本，第二批派生净增1772013 bytes。整体goal继续active。

### 2026-10-13：CPU测量边界核对

检查`CpuSamplingObserver`、原生thread-local等待计数和`AscendSourceExtractor::observe_cpu_control/extract_worker_cpu`，并对当前单库源图作一次边界统计。调度前缀746段/34个owner-lane组，准备主体4382段/26组，执行276370段/26组，worker提交137050段/78组；这是现有源图服务切分数，不是应实现的成本模型数或新增采集需求。原生采样只读调用线程CPU时钟，无法覆盖worker；语义/提交切口又使阶段总值无法直接映射成节点成本。证据`data/redesign/s5-cpu-service-role/cpu-measurement-boundaries.json`。

据此明确性能合同：先按已有语义区域/操作确定可测局部单元，纯内部切口允许合并，对外提交/等待/状态端口必须保留；控制线程与worker分别计费，不按base时间比例或节点数分摊阶段总值，不逐源图节点新增wrapper。下一步是确定一个有实际目标特征和计费落点的单元后再测量，当前不为旧阶段样本盲目追加同类采集。此轮只形成边界证据和设计约束，无源码修改、无构建或原生采集；代码批次计数仍为2，整体goal继续active。

目标工作量交接随后落实到正式成本查询：运行时在staged work提交到稳定所有者后，向ExecutionGraph传递只读InvokeStep索引，ServiceWork只在本次查询借用目标决策。调度前缀control_context复用AdmissionWork/AdmissionEntryState和transition；不复制控制器状态、不重新推导计数、不将阶段上下文当作每个CPU节点的重复服务。首个候选仍为已有CPU准入区域，具体局部计费边界与测量接入尚未完成。

公开predict缺耗时查询返回等待3、候选检查2、horizon检查3、停止input_budget，与先前独立预算轴场景保存的目标invocation完全一致；完整给定耗时的metrics、execution_summary、status及timing_basis保持。证据`data/redesign/s5-admission-context/verification.json`和context-verification.json。首轮构建发现ServiceWork新增引用未在聚合初始化处传入，已修正；最终Docker构建、格式及diff检查通过。本批为阶段审查后第3批，无原生采集、无模板重编译或新测试文件，派生产物已入账；真实成本模型仍未完成。

### 2026-10-13：代表性建库的后处理失败隔离

确认后处理异常原先会退出整个PredictionWorkflow。CaptureProcessing现在在持有处理锁期间记录确定的processing_failure.json并继续非零退出；CollectionWorkflow优先消费已发布成功结果，否则将记录转换为CaptureProcessingFailed并保留原attempt详情；PredictionWorkflow只捕获此类型，将失败归属当前配置组后继续既有选择循环。没有处理进程记录的宿主异常/中断仍保留恢复状态，不被当作组失败。恢复检查已有失败记录，不自动重试；修复后的显式后处理仍使用原采集数据，再由已有资产导入入口交接。

Docker公开processing入口对不存在的capture输入返回1，记录FileNotFoundError且没有发布成功结果；同入口读取实际已有成功attempt时直接返回原processing_result，未重采或重提取。证据`data/redesign/s6-processing-failure/verification.json`。该检查只证明生产/消费边界中的处理进程行为，未验证真实多组失败后继续路径；S6仍保留此运行验收，不虚构采集状态或增加mock测试。Ruff、Docker Python语法及diff检查通过，未修改C++或.clang-format。本批为阶段审查后第4批，无原生采集，诊断派生文件计入累计账本；下一批或进入新阶段前做周期整体审查。

### 2026-10-13：原生失败与已有资产隔离验证

目的为验证S6公开入口的真实失败归属和已有资产隔离，不补采模板。声明两个配置组：不存在的设备98/99触发原生启动失败，设备4/5组复用现有单库与显式给定耗时。最多一次原生attempt、30秒，原始预留8 MiB、派生预留32 MiB；达到终态或时限即停，不重试。执行前累计口径与本次上限记录于`data/redesign/s6-native-failure-isolation/budget.json`，不会重置历史累计预算或超额。后处理失败后继续另一真实采集组不属于本次验证结论。首轮发现Docker runtime启动失败后留下Created/Error/ExitCode127，但代码既未结算也未保存启动命令耗时；原native monitor未运行，旧attempt缺失的启动耗时不补造。修正持久launch_result及明确失败识别后，另授权一次开发回归任务，追加最多一次启动、30秒、原始8 MiB和派生32 MiB；旧任务保留，不自动重试。本场景用于修改，不能称独立验收。

开发回归结果：修正后新任务只有1个attempt、2轮预测，启动失败计量367608220 ns；错误仅附给unavailable-devices组，已有库组complete且指标/执行摘要与首轮保持。结束后公开resume的attempt、round、collection_usage及组合结果完全不变。证据`data/redesign/s6-native-failure-isolation/verification.json`。两次均为容器启动失败，未运行native monitor或生成新模板；首轮旧任务缺持久启动耗时，不写零或事后估计，单列未结算事项。后处理失败后继续新采集及多个成功组仍缺运行证据。本批为第5批，已完成当日审查文档第四节的层次、风格、所有权与恢复逻辑审查，周期归零；未完成整体goal。

### 2026-10-13：没有新资产时复用预测结果

确认失败组不改变project资产，但旧循环仍重新预测所有组合。现将模拟调用移到首次进入及成功导入资产后，失败后沿用已有结果继续选组；持久selecting阶段使该位置的宿主恢复同样复用结果。没有新增共享可变仿真状态或另一个结果缓存，只读取原轮已发布prediction.json。

开发回归沿用明确无法启动的设备组和已有资产组，只新增一次启动尝试、30秒，原始预留8 MiB、派生16 MiB；停止条件为首次终态或超时，不重试、不获取新模板。执行前累计值和边界见`data/redesign/s6-no-change-reuse/budget.json`。目标是1轮预测、失败按组归属、已有组指标保持及resume幂等；不据此宣称成功导入资产后已实现受影响组合重算。

公开回归完成：预测由2轮降为1轮，仅1个启动失败attempt；失败仅归属设备错误组，已有资产组complete且指标/执行摘要与修改前相同。结束后resume的round、attempt、用量和组合结果保持，证据verification.json。selecting中途宿主中断未另做运行注入；成功导入后的部分重算仍未完成。Ruff、Docker Python语法和diff检查通过，无C++修改或新模板。本批为新周期第1批。

### 2026-10-13：自动导入资产后的分组重算

来源选择与代表分组均使用相同部署及supports_configuration_change关系；自动生成来源不跨此组消费。Python将组configuration_ids随active_representative持久保存，成功导入后仅将本组配置送入下一轮C++，按配置/workload身份替换组合结果并更新相应source_coverage。其他组保留原结果与诊断路径；原轮result.json仍忠实记录实际执行子集，聚合prediction.json保存全任务视图。显式资产替换没有此已知影响范围，仍完整重算。新增持久字段使用当前合同，旧任务不添加兼容猜测。

为验证成功导入而非仅失败分支，安排一个新部署组（设备0/1）自动建库，已有设备4/5组复用现库和给定耗时。设备已确认空闲；使用现有固定配方，仅1次原生attempt、300秒，原始512 MiB、派生1.5 GiB，以此前同配方实测约198秒/420 MiB/1.10 GiB为依据。成功、失败或额度结束即停止，不通过补采清除规则缺口；执行前累计值和追加上限见`data/redesign/s6-selective-recompute/budget.json`。目标是第二轮只执行新部署组、已有组结果原样保留、完整组计划/来源资格合并正确，并验证重复resume。真实性能成本及多成功组采集不由本次证明。

实际结果：唯一原生attempt完成，耗时197583238010 ns；原始保留441933492 bytes、派生1185923661 bytes、compiled资产118119626 bytes，均在本次额度内；原生结束至处理结果发布266218702010 ns（含处理编排，非CPU服务成本）。第二轮project/result只含new-deployment，任务聚合仍有两个组合和完整组计划；existing-pool-four整条组合记录与首轮完全相同，新组报告node_durations_missing，两个资产均loaded。source_coverage仅替换新组，重复公开resume不增加attempt、round、用量或改变组合。证据`data/redesign/s6-selective-recompute/verification.json`和accounting.json。没有补采、增加规则或以给定耗时冒充真实成本。

本批为最近周期第2批；Ruff、Docker Python语法、diff及文档本地文件链接检查通过，未修改C++或.clang-format。选择性重算限自动代表性资产交接；显式模板/性能资产替换仍全量重算，多workload及多成功组生命周期未由此场景单独证明，S5真实成本与S6最终闭环继续未完成。

### 2026-10-13：首个完整CPU成本单元的边界与目标成员

核对原生add_one_req_ignore_eos、相同hook上的admission_budget语义区域及当前单库源图：22个成员区域均为单入口/出口，且唯一CPU服务节点覆盖完整区域。由此选择整次预算成员调用作为首个可测单元，无需将整阶段成本分摊或先实现子图成本合并。源图与模板边界证据见`data/redesign/s5-admission-unit/boundary-audit.json`。

TemplateDefinition根据实际成员区域和服务边界提供槽位，ExecutionGraph区分普通来源槽与插入节点目标request_slot，ServiceWork查询提供该成员的输入长度、运行成员数、已接纳成员数和horizon检查数。区域不满足完整单节点条件时保持原有服务表达，不添加整个模板拒绝条件。TemplateGraph访问返回节点值，因此局部查询按值保留节点，不借用临时对象；没有改变拓扑、资产格式、给定耗时或成本数值。

Docker C++23构建、修改文件格式及diff检查通过。完整给定耗时的指标/执行摘要/状态/timing_basis保持；缺单元耗时查询使用目标101 token而非来源126 token。另用已通过的pool-three输入验证2来源成员→3目标成员的插入路径，查询正确为目标125 token、slot0/horizon1。17-token单请求诊断被已有mesh规则挡在成本查询前，记录为范围限制，未改规则或补采。证据verification.json、insertion-verification.json；这些是成本单元交接回归，不是实测模型或精度验收。

本批为周期第3批，无原生采集或模板重编译，派生净增50049246 bytes（主要是保留显式给定耗时并留空22个完整单元的输入），已入累计账本。下一步对同一原生方法落实无profiler调用测量、探针边界和必要特征资格，旧阶段样本仍不导入为合格成本。整体goal继续active。

### 2026-10-13：准入预算原生调用采样

CPU采样复用native_regions中admission_budget的hook，直接包围原生方法，不启用profiler或语义AST循环计数。单元文件与阶段样本分开，记录线程CPU调用时间、相邻时钟读取量级、runtime等待/探针计数、入口参数、是否已初始化req_states、实际返回及是否接纳。非SWA路径只有确认接纳后才可由原生代码推出完整horizon遍历次数；拒绝调用保留未知。新增wrapper时间从外围Schedule阶段扣除，原生调用服务仍包含在该阶段中；二者不能同时重复收费。时钟对照不自动从样本减去，manifest明确保持单元needs_data及调用/时钟边界未资格化。

安排一次公开cpu sample：单个双候选Prefill域（101/111 token，均本次完成），一次域预热、3次测量；仅1个attempt、300秒，原始8 MiB、派生16 MiB。额度参考既有CPU采样约174秒，当前不生成profiler trace或模板。执行前累计值见`data/redesign/s5-admission-sampling/budget.json`；成功、失败或额度到限即停，不自动重试或扩展网格。目的是取得可核对的真实单元观测，不宣称成本拟合或独立精度通过。

实际一次attempt成功，原生147719789880 ns，保留原始125661 bytes、派生51953 bytes，均在本次预算内；累计86次attempt（含3次未运行原生monitor的启动失败，历史一次启动区间仍未知），已更新原累计账本。双rank共16条单元记录，其中4条预热、12条测量；8个请求全部完成，阶段服务非负，单元未观测到runtime等待或原生探针计数。入口slot0为初始化、slot1为插入，确认接纳后的horizon分别为1/2，返回CONTINUE/OTHER与实际接纳并不矛盾。证据见`data/redesign/s5-admission-sampling/verification.json`与`accounting.json`。

正式调用约42–144微秒，相邻时钟读取0.71–0.92微秒；同一rank/特征的首次成员仍有约53–144微秒波动，不删除高值或加补偿。时钟对照不证明完整调用边界开销已去除；初始化、成员位置、horizon和长度共变，不能用两类均值拟合独立系数或迁移至有running成员/拒绝调用。该结果完成采样边界交接，未完成成本资格或独立精度验证；下一步依据原生算法确定最小可识别成本特征，不按长度扩展采样网格。Ruff、Docker Python语法及diff检查通过。本批为周期第4批。

同一交接批次继续收紧C++查询：提取AdmissionServiceWork值对象，ServiceWork从目标槽位生成类型化工作，诊断复用其describe；后续模型不必解析JSON或重复解释目标身份。Docker C++23构建和真实missing-duration公开预测通过，组合结果与改动前完全一致，证据`data/redesign/s5-admission-unit/typed-query-verification.json`。未改模板格式、控制规则或耗时。继续核对接纳路径后，首次初始化可由accepted_requests==0推导，已从同一值对象交付request_states_initialized，与原生采样分支对应；不增加状态存储或控制器计数。成本粒度收口为先验证初始化/插入分支与成员工作规模，只有独立误差证据才细分内部比较，不把排序/插入的每个细节预先变成门槛。真实缺耗时预测新增初始化查询，其余组合字段保持；16条已有原生分支记录符合该推导，证据`initialization-query-verification.json`。尚未单独运行后续目标成员的成本查询，不扩大证据范围。本项仍属于第4批采样—查询交接。

### 2026-10-13：准入经验基线的独立留出

先用已有12条正式CPU单元测量固定经验基线：按初始化分支、已接纳/运行成员数及horizon次数分组，双rank合并取中位数，不移除高值、不按长度或rank拟合、不外推。候选参数与原始范围写入`data/redesign/s5-admission-baseline/candidate-model.json`，尚不导入正式预测；调用时钟与边界开销仍保留为限制。

独立留出使用107/101 token的双候选、输出1/2 token，域预热后重复5次；与拟合输入的101/111、输出1/1不同，检验长度与输出安排是否可在该单元中不作为成本键。一次attempt上限300秒、原始8 MiB、派生16 MiB，累计额度见budget.json。成功、失败或到限即停止，不因验证误差调整中位数、追加规则或自动补采。该验证只覆盖当前两类准入工作，不证明有running成员或更多成员的成本。

实际一次attempt成功，耗时155250531500 ns，原始172241 bytes、派生62072 bytes，均在预算内；累计87次attempt，历史未知启动区间原样保留。取得4条预热与20条正式单元记录，全部匹配预先固定的工作键；未把留出数据混入拟合或模板资产。首次初始化/后续插入的MAE分别9064/6759 ns、WAPE分别17.80%/15.54%，最大绝对误差13120/10485 ns；候选估计仍保持55380/48325 ns，没有删除观测或调参。完整逐条比较见`validation.json`，资源见`accounting.json`。无事先约定的精度门槛，且调用/时钟边界尚未完成配对校正，故不标精度通过、不自动提升manifest资格。本次验证到此停止；后续成本资产接入须携带实际范围和测量限制，不能把这两个局部经验值当成全部CPU或完整S5。

### 2026-10-13：独立通信采样的物理任务交接

CollectiveCostNormalizer原先只发布collective包络；现从同一观测按rank/stream/task/context连接明细与唯一物理任务，补充task_samples及分类统计。数据、等待、标记分别保留，不把等待时长变成服务，也不分摊父包络；已知setup外的归属缺失保留缺口。解释仍在Ascend适配目录，核心控制与执行没有框架分支。

复用2026-10-09独立通信原始资产，在`data/redesign/s5-collective-tasks`建立显式硬件范围的导入引用，补齐真实已有clock/database/device文件地址；不改原始采集，旧API journal与缺失communicator日志未伪造迁移。公开normalization入口重提取出400条任务：208数据、96等待、96标记，其中正式测量数据任务156条。逐条原始trace核对物理身份、操作、字节和时间，通过唯一性检查；旧48条rank样本的描述与持续时间保持。证据verification.json和import-scope.json。无新原生采集或成本拟合，profile与内部停顿限制保持；硬件范围导入的缺失事实也继续报告。

Python全包Ruff、Docker正式归一化及diff检查通过。本批为周期第5批，已进行整体职责/风格/逻辑审查，见当日code_structure_review；周期归零。

### 2026-10-13：准入经验候选的正式C++生产与评价

新增公开fit-costs --performance-manifest：workflow读取声明的cpu_units并校验为公共AdmissionCostSample，核心AdmissionCostFitter按工作规模分组取中位数，CLI和Shell只转交参数。输出来源、样本数/范围、假设和原测量资格，不去异常值、不按长度或rank拟合、不外推；预热及不符合完整接纳/无嵌套等待条件的记录单列排除原因。无样本返回needs_data；有样本仍为candidate，不自动进入预测或更改原manifest。没有框架名称分支、第二套事件核或硬编码参数。

Docker C++23构建通过；公开Shell入口使用已有12条正式测量、排除4条预热，所得两组条目与独立验证前固定的基线完全相同。未读取留出资产，保留measurement_readiness=needs_data。同一拟合交接继续补齐observed_context：直接读取声明的prepared/native/worker环境记录，保留配置、部署、runtime和CPU亲和性，不把来源版本或CPU数量当作自动迁移许可。公开命令复核两组数值和原资格保持、上下文与真实记录一致，证据context-verification.json。既有公开plan入口保持可运行；证据`data/redesign/s5-cost-fitting/verification.json`。无新原生采集，正式模型加载/部署匹配与预测消费仍未交付。本批为周期第1批。

继续完成evaluate-costs公开入口：workflow共享输入读取，性能层只按固定候选查表计算误差，不调用fit；候选新增sample_sources供直接训练/留出文件重叠检查，无摘要或冻结副本机制。上下文是否相同作为观测报告，不自动授权迁移；缺工作不外推，精度资格不提升。公开Docker运行复现原独立20条比较，总体MAE 7911.5 ns、WAPE 16.76%，分组统计与原留出分析一致；实际训练manifest用于评价时按要求返回退出码2，不读取留出数据进行拟合。证据`data/redesign/s5-cost-fitting/evaluation-verification.json`。候选数值、范围及测量缺口保持，未新增采集。构建、Shell语法与diff检查通过；生产/评价交接为本周期第2批，预测成本消费仍未完成。

### 2026-10-13：整体成本需求与物理节点分类修正

对已有Prefill预算结构验收场景及旧独立设备采样做整体交接核对，无新采集。目标有184754个服务节点，按真实来源种类为CPU 162048、计算8918、DMA 6256、物理设备命令7532；CPU分布主要为execute 106656和dispatch 52892。计算265种名称/shape/dtype/格式描述中精确匹配106种、涉及3108个目标节点，但额外attention/KV/控制数据、部署和测量资格仍需独立处理。计数不是时延占比、成本模型数或补采清单。证据`data/redesign/s5-cost-coverage/coverage.json`。

此次核对暴露真实成本分类缺陷：ServiceWork以缺少设备描述作为CPU默认值，导致裸设备事件命令被误归类。ExecutionGraph现在从普通和插入模板节点保留SourceSpanKind，传入ServiceWork并写入诊断kind；CpuControl才显示cpu_service。物理DeviceMarker既有服务角色、Notify_Record通信工作及依赖保持，不修改TemplateNode::role，也不把物理命令置零。同步澄清性能文档中物理命令与逻辑Annotation的区别。

Docker构建通过。用已有图前缀701个来源服务的显式1000 ns假设构造小输入，真实公开预测在source_span 172300返回device_marker成本缺口；未创建大份逐span耗时副本。完整给定耗时场景的全部组合结果与此前一致，见verification.json。诊断kind与查询共享同一运行时字段，本轮未另行导出整张新图。计划按整体CPU/设备模型交付收紧，准入支线先收口，避免继续以字段和局部报告代替成本消费。本批为周期第3批，无原生采集和模板重编译。

### 2026-10-13：CPU操作窗口与提交端点的边界核对

使用已有单库source_graph，按同线程时间包含匹配CPU服务，并统计窗口内部的跨lane连接端点。74164个host窗口中43319个存在内部跨lane端点；43202个worker窗口中43192个存在。窗口完整包含的CPU服务去重后为317428个；未被窗口完整包含的execute/dispatch分别为96356/3380个，其他阶段也有剩余。这是全来源库的边界调查，不是上一节目标图的节点统计、耗时占比或收缩资格。证据`data/redesign/s5-cost-coverage/cpu-boundaries.json`保留方法、分类型计数与来源示例。

结论：已有operation关联可作为成本语义入口，但整算子host/worker总耗时无法直接替换现有服务，且仅操作窗口不能覆盖流程内CPU。测量交接应保留入队/设备提交端点，主线程与worker分别观测，并为操作外的语义区域保留工作；不按base墙钟或节点数分摊。代码核对确认现有CpuSamplingObserver只读取调用线程，runtime CpuSampling目前主要提供线程局部等待计数，未实现worker执行单元。下一步据这些实际边界落实runtime观测与公共成本单元交接，不新增逐来源节点模型，也不立即启动补采。

本轮无产品代码改动、构建或原生采集；分析产物增量计入原累计存储账本，原始事实未修改。S5未完成，周期代码批数仍为3。

随后落实观测层次：将SGLang采样器内的ctypes布局、原生模式切换和计数器读取移入Ascend runtime的CpuSampling，框架采样器直接消费快照，删除旧本地ABI和转发方法。字段、线程CPU计时边界、差值公式和输出合同保持。Docker内加载现有真实原生观察库，通过64字节ABI、连续读取及框架导入检查；Ruff与diff检查通过。证据`data/redesign/s5-cost-coverage/runtime-counter-binding.json`。该检查无设备等待，不证明完整采集或worker观测；没有补采。此代码职责调整为本周期第4批，下一批完成后进行整体代码审查。

继续核对真实原生入口，确定worker执行测量落在ReadQueue的manager.Call，而非包含空队列轮询/休眠的Dequeue。enCurrentNPUStream无条件产生correlation_id，队列复制保留它，同时将OPAPI_V2转换为OPAPI；因此无profiler观测可以复用原生队列身份，无须新造ID或按时间猜关联。已有profiler队列标记受开关控制，不能直接用于无profiler采样；Release也在原执行观测区间之外，不扩大成本边界。来源摘录为`data/redesign/s5-cost-coverage/worker-hook-boundaries.json`。从实际采集镜像读取安装来源保存为worker-runtime-provenance.json，固定torch_npu commit与当前安装脚本一致；旧镜像记录的旧适配路径保留原事实。未修改镜像、插桩或第三方源码，未启动采集；worker观测及内部提交计时仍待实现。

worker观测实现为本周期第5批：新增Ascend native WorkerSampling，queue.patch只包围Enqueue和ReadQueue实际任务调用，通过标量C ABI传原生correlation_id及task_type；异常/早退保留失败。Python runtime以可嵌套业务范围启用入队，pending原生ID将范围传给worker，初始化和无关队列不全局采集。记录各线程CPU/单调时钟、已有等待/探针计数和现有runtime拦截函数进出点；不跨线程减CPU时钟，不自动扣除全部探针。结果在区间结束后缓冲，业务发布后刷新cpu_workers，manifest保留未验证覆盖/匹配/观测开销等缺口。

采集容器内C++23观察库构建通过（只有既有SDK头警告），队列补丁在保留的原生构建源码上dry-run通过并编译NPUQueue.cpp对象；尚未重新链接部署torch_npu或重建正式采集镜像。真实观察库的Python绑定、嵌套范围、计数器读取及空缓冲刷新通过，全包AST、Ruff、Shell语法和diff检查通过。未调用伪造队列事件，也未启动原生采集，因此不能宣称记录关联或成本资格已验证。后续须先部署并验证完整提交端点交接，再安排有界采样，不沿旧镜像空产物反复试采。周期审查见当日代码结构审查，计数归零。

### 2026-10-13：worker部署准备与计算提交边界

既有主库单rank trace显示LaunchKernelV2 9341、KernelLaunchWithHandle 1024、其Big范围640及KernelLaunch 527次；这些是选择入口的已有观测，不是新采集或成本样本。按已安装SDK原型加入rtsLaunchKernelWithHostArgs、rtKernelLaunchWithHandle/WithHandleV2和rtKernelLaunch的worker进出点，参数及返回值原样传递，不读取tensor、不同步、不新增结构journal事件。libacl_rt_impl实际导入rts入口，记录在`data/redesign/s5-worker-sampling/launch-boundaries.json`；函数存在仍不证明实际拦截成功。SDK的rts头需要runtime/runtime搜索路径，已补CMake后构建通过。

部署前发现仅队列类型不足以标识算子，queue适配现在通过标量ABI传入原生任务名称，观测库持有字符串副本；非算子任务保留null，OPAPI_V2按真实布局读取opName。修正后观察库和原生队列对象均再次编译通过，无额外兼容入口。排除范围的入队现在在取锁前返回，避免无关入队竞争观测锁。

首次镜像构建启动早于该观测合同补齐，已显式取消并确认构建进程结束，日志保留为image-build-cancelled.log；不是采集重试，也未发布镜像。修正后以既有markov-workspace-20261012-sglang为基础重新构建markov-worker-20261013-sglang，复用原框架环境，仅重新安装当前runtime适配；构建日志为image-build.log，目前尚未完成。原镜像未覆盖。正式采集前仍需完成镜像安装核对和目标单元交接，无新NPU采集、attempt或采集预算变化。本批为新周期第1批。

镜像构建期间准备了一次有界观测验证，尚未启动：复用budget-256/TP2/设备4、5及真实token输入101/111，finished_request_count=1。公开Docker prepare-sampling已生成output_tokens 1/2，确认一次prefill后有decode；只设一个工作域，原生预热后测量1轮。目的仅为配对入队/worker、归属已有CPU阶段和验证runtime点覆盖，不用于拟合、留出评价或宣称目标成本映射已完成。输入、边界和停止条件见s5-worker-sampling/{project,measurement-requests,capture-plan,prepared-preview}.json。

本次最多1个attempt、300秒、新原始64 MiB、新派生32 MiB；累计上限更新为native 3347922312420 ns、raw 3713734967 bytes、derived 11196529951 bytes，已有使用量及87次历史attempt保持。镜像安装核对通过后才启动；达到一次尝试或额度即停，关联/点覆盖失败先查原生及合同，不自动重复采集。该步骤是观测实现验证，正式模型采样仍需目标单元交接，不降低S5成本要求。

镜像现已构建、安装核对并通过一次真实观测。`image-verification.json`确认固定Torch/torch_npu源码、queue补丁来源和原生库回调；旧镜像保持。唯一新attempt为`run-20261013-074241-423895-daa1199ddf184526b5ede8ca320bd7b9/performance/attempt-1`，原生耗时154254921370 ns、原始13812952 bytes；两rank预热/完成覆盖无缺失，累计88次。镜像软件构建约1792秒另记，不计为NPU采集时间。

`verification.json`对14636个enqueue和14636个worker按pid/correlation_id配对，全部归属已有同线程业务阶段；worker归属从队列身份传递，未按时间猜测。核对返回、名称、允许的原生任务类型转换、单元内线程时钟/等待/探针计数范围和进出点嵌套，无发现问题。8种runtime入口有真实记录，包括rtsLaunchKernelWithHostArgs、rtKernelLaunch、rtKernelLaunchWithHandleV2及通信/复制/事件入口；本次未调用rtKernelLaunchWithHandle，不能把导出存在当作真实调用证据。phase-assignment.json保留逐对来源，不复制请求负载。数据只用于观测实现的开发验证，原manifest needs_data和缺口未修改，未拟合或提升成本资格。

本次尝试到此结束，不因尚缺目标匹配或服务解释再采集。下一步用已有结构与新观测交接目标成本单元；主线程操作外服务、目标参数和探针开销仍需按S5完成，不将完整worker包络直接填入所有切分节点。


### 2026-10-13：成本提交边界交接核对

复用主库source_facts/source_graph和已完成的worker观测，没有新增采集。`data/redesign/s5-worker-sampling/template-port-examples.json`保留四类worker的跨线程端口，`submission-boundary-detail.json`按rank/native tid核对其内部调用（CANN profiler pid是合成泳道，不能当作OS pid过滤），`runtime-signatures.json`汇总已有预热与测量中的runtime调用序列。这些都是开发分析，不是成本样本或支持域验证。

已证实结构端口与新计时入口不能直接等同：示例Matmul的Node@launch从worker起点后56770 ns开始，Runtime@LaunchKernelV2从80800 ns开始；事件记录的AscendCL入口和Runtime入口也不同。通信高层launch还包围初始化及后续FFTS提交。上述均为来源墙钟位置，仅用于辨认边界，不能提取差值作为服务成本或目标补偿。新观测中Matmul的2056次worker均记录一次rtsLaunchKernelWithHostArgs，PagedAttention的256次均记录一次rtKernelLaunchWithHandleV2；单次调用数量不能替代端口身份与目标工作匹配。

因此暂不将worker总耗时按名称导入模型，也不增加逐节点模型或补采。下一实现落点确定为模板生产适配端的提交端口对应：复用已有原生任务身份及调用关系，向公共成本层交付明确单元；底层API名称不进入调度器。当前分析未修改结构或成本运行行为，S5仍未完成。


提交边界对应已落实在Ascend ObservationNormalizer：先核对原生HostToDevice flow的connection_id，再于生产调用内部、同线程寻找唯一runtime提交入口；成功时细化flow起点，原端点保留在profiler_source_intervals。无唯一对应则保留原始flow，不选择最近调用、不加入补偿时长。公共C++图提取继续消费同一flow合同，没有增加SDK判断、控制器分支或模板种类。

复用原始主库，43368条设备提交flow全部唯一对应，未出现入口晚于设备开始。一次归一化、Docker内提取/编译通过，图无环且无flow时间倒序，原有coverage、缺口类别保持。原库未覆盖写入；新资产与回归证据为`data/redesign/s5-submission-boundaries/{profile_manifest,extraction-result,verification}.json`，细化对应清单为`data/redesign/s5-worker-sampling/runtime-port-binding.json`。区域实例仍为3328，共享定义由原28变为27；这一变化来自同一批工作边界的重提取，没有增加规则变体。

同配置、池缩小和Prefill预算变化分别完整展开6/6/5批，24/24/20个控制动作，图无环、无组合缺口。历史same结果未完成，历史pool结果用了另一份来源和100 ns输入，因此没有将它们的数值差异判为本改动退化；补做当前程序、原主库、相同请求/配置及1000 ns给定耗时的对照后，三场景执行摘要及条件指标保持一致。原pool/budget详细记录的控制工作也保持；budget变换后的kernel描述保持。这里是已有场景的开发回归，不是新增独立验收或真实性能精度证明。

本批只重处理及仿真，新增采集为0，累计仍为88次。事先增加2 GiB派生额度，实际该目录保留2010438275 bytes（原始材料仅相对符号链接，不重复计量），在额度内。累计派生估计13178146583 bytes，原始及原生耗时不变。Ruff和diff检查通过。本次为上次整体审查后第2个实质代码批次；CPU目标单元及正式成本消费仍未完成，不写入goal完成总结。


### 2026-10-13：worker CPU测量单元规范化

先核对新边界下的源图：43202个操作worker窗口中，41154个已经由三个连续CPU服务段覆盖。因此主线优先直接对应既有服务段，不预建图合并器；其余窗口保留实际分段、等待和外部端点。证据为`data/redesign/s5-submission-boundaries/worker-service-boundaries.json`，此覆盖统计不代表模型已接入。

新增Ascend runtime的CpuWorkerUnits，由SGLang采样发布流程调用。阶段归属来自入队线程的唯一最内层业务区间，并通过原生pid/correlation_id连接worker；各线程的相邻边界产生不重叠分段，嵌套API不重复累计父包络。线程CPU、已观测等待和探针计数独立保留，不跨线程相减、不按总时长比例分摊、不自动扣出所谓纯服务。原始文件/行号与共享阶段上下文供后续匹配追溯，未配对/失败/归属不唯一保留缺口。manifest正式引用cpu_worker_units及其覆盖结果，成本资格仍为needs_data。

复用已完成的原生worker观测，一次重处理得到14636个单元（预热/测量各7318），全部与之前独立核对的阶段归属一致；enqueue均为一个测量段，worker分别有14556/72/4/4个单元包含3/5/7/9段。按原始行核对，线程CPU、等待和探针计数的分段和均还原总量；没有将测量边界数当作成本模型数。验证见`data/redesign/s5-worker-units/verification.json`。Docker内全包AST、NativeSampling导入及全包Ruff、diff检查通过；没有为此重新运行原生采样。

本批为上次整体审查后第3个实质代码批次。重处理预留32 MiB派生量，实际单元文件13976671 bytes；无新增采集，累计仍88次。目标操作特征与执行段的正式成本匹配尚未完成，本次不拟合、不提升成本资格、不宣布S5完成。


### 2026-10-13：CPU提交工作接入目标成本查询

Ascend生产端对完整覆盖worker、且中段精确对应已确认runtime提交边界的三个既有CPU服务，生成SourceCpuWork（原生操作、before/call/after、提交来源）。不合并额外切口、不删除等待、不增加服务节点。该描述通过模板节点与CBOR资产交接；旧格式需按当前归一化事实重新extract，不设兼容默认字段。数据中的SDK含义仍由Ascend适配端解释，公共图和控制器没有SDK名称分支。

ExecutionGraph在拓扑变换完成后，沿实际目标NativeFlow端口绑定提交根，取得当前kernel/传输等工作描述。提交后段使用实际返回端点定位调用，避免重复供体插入按来源编号串联；SourceCpuWork的来源提交编号不进入ServiceWork成本特征。通信根可能仍是逻辑包络，不能将关联成功宣称为完整通信成本描述。当前仍由NodeDurations提供显式给定耗时，没有借工作描述引入收费或模型常数。

一次旧事实重提取/编译产生123462个CPU服务描述（41154组三段），源图边界、节点与依赖数量保持，图无环。当前资产位于`data/redesign/s5-cpu-target-work`。同配置、池缩小、预算变化公开预测均完整展开、无组合缺口，与上一批同输入/耗时的执行摘要和指标一致。池缩小诊断中的56478个已标记CPU段均解析到目标提交根；同一次目标工作内存在6个重复使用的来源服务身份，37652个相邻段比较无提交工作串联问题。证据为该目录`verification.json`，不据此宣称复杂worker或所有插入规则已资格化。

通过明确移除Matmul worker的假设耗时，公开预测返回given_node_duration_missing及类型化目标CPU工作：before段对应目标kernel输入`212,5120;5120,5120`，而其来源是`253,5120;5120,5120`。来源与目标核对见`target-shape-verification.json`；该探针只验证查询交接，不作为拟合或性能精度证据。Docker C++23构建及diff检查通过。

本批为上次整体审查后第4个实质代码批次。只重提取和仿真，新增采集为0，累计仍88次；事先预留2 GiB派生额度，实际保留1125984057 bytes，累计派生估计14318113718 bytes。CPU实测模型匹配、主线程及复杂单元覆盖仍待完成，goal保持进行中。

## 3. 当前结构对照证据索引

以下报告位于 `data/redesign/s4-independent-structure`，对应第 73 次开发对照。给定耗时目标图为 341335 节点、789431 边；4 批/16 动作。只证明表中列出的内容。

| 已核对内容 | 当前证据与范围 | 报告文件（该目录下） |
| --- | --- | --- |
| 请求状态、内部工作与参数 | 32个调用状态、6768个kernel描述、34个传输方向/字节、512次workspace需求、8次发布一致；每rank扩容一次，终容量197132288 | native_kernel_comparison.json、native_transfer_comparison.json、native_workspace_comparison.json、native_publication_comparison.json |
| 计算顺序及端口 | 6744对相邻kernel依赖路径、6606个模型kernel→device_done、8条device_done→published路径成立 | native_order_and_port_comparison.json |
| 通信任务及直接依赖 | 1040个collective、9876个物理任务描述一致；10666条内部依赖和2596条跨rank notify依赖逐条一致 | native_communication_comparison.json、native_communication_dependency_comparison.json |
| 跨流event | 5200个record/wait局部顺序对齐，2080条显式依赖一致，无wait内同handle变更 | native_event_dependency_comparison.json |
| 主机等待资源 | 94次等待的计算/D2D前沿分布一致；两次设备等待各1823个通信任务描述一致 | native_wait_compute_frontiers.json、native_wait_transfer_frontiers.json |
| 等待的通信闭包 | 从原生流顺序、FFTS、notify和event重建21850个物理任务依赖后，94次等待处的通信祖先描述分布与目标图一致 | native_wait_communication_closure.json |
| 复制边界及计费 | 28次H2D的内部等待先于复制；34个传输可达发布；36个直接memcpy为annotation，24个所用来源区间无服务空洞、重复或等待再次计费 | native_copy_boundary_comparison.json、direct_copy_cpu_coverage.json |
| CPU启动连接及返回边界 | 6768个kernel的13536个CPU启动来源均通过原始flow身份唯一定位，目标保留begin→begin连接；其CPU返回点均不可达对应kernel启动点，没有新增返回屏障 | native_cpu_kernel_launch_anchors.json、cpu_launch_identity_and_returns.json |


等待比较部分是按阶段/rank/scope的分布，不是跨线程逐 wait 身份证明；通信闭包不包含全部 CPU 调度边。CPU启动身份对应不证明所有 CPU 服务逐项等价，更不证明其成本可迁移。无相关代码变化或具体反证时，不重复这些局部检查来累计进展。

## 4. 资源与恢复交接

截至本次整理，累计原生尝试 81 次：63 次管线、18 次独立诊断，不能解释为产品每次建库需要 81 次采集。当前累计账本为 `data/redesign/s4-native-cleanup/storage_accounting.json`：

| 项目 | 已用 | 该账本上限 |
| --- | ---: | ---: |
| 原生时间 | 2546633132370 ns | 2666083102010 ns |
| 原始数据 | 3204393103 bytes | 3568893709 bytes |
| 派生估计 | 9918188551 bytes | 10544450840 bytes |

该账本是既有任务组口径，不声称涵盖全部历史数据。最近目录 s3-source-eligibility（818018 bytes）、s3-mechanism-coverage（482331 bytes）、s4-prediction-adapter（638669 bytes）、s4-planning-adapter（82608 bytes）、s4-collection-adapter（300669 bytes）及 s4-extraction-adapter（6459 bytes）已计入，恢复时不得重复计量。本次未删除证据；s4-initialization-transfer 的两次原生观测及派生产物已累计，新增原生额度及原因见上文。历史逐次预算提升不作为今后惯性扩额的依据，需说明新证据能关闭哪个能力缺口。

最近代码质量周期审查在进入S5前完成，覆盖最近三批Python修改并贯穿核心职责；随后五批已在当日审查文档第四节复查，此后完成3批（无新资产结果复用、自动导入后的分组重算、CPU准入成员单元交接）。已推送检查点 `862731d`；后续工作区仍有未提交改动，本次未提交或推送。恢复开发时重新核对工作区与运行进程，不能把历史进程状态视为实时状态。

## 5. 历史证据地址索引

以下从原记录去重保留，以 `data/redesign/` 为共同根目录；花括号是历史文件组缩写，不是字面文件名。这是查找索引，不是已通过/独立验证清单，也不要求开发时逐项重读。目录内报告保留具体输入、局部结果和限制；现行结论以上文及当前计划为准。原记录只列目录的条目不补造文件名。

- `directory-layout`：`verification.json`
- `s3-admission-block-contract`：`verification.json`
- `s3-admission-control-contract`：`source_audit.json`
- `s3-admission-entry-order`：`verification.json`
- `s3-admission-policy-audit`：`source_audit.json`
- `s3-admission-policy-record`：`{contract_audit,verification}.json`
- `s3-admission-prefix-contract`：`{contract_audit,verification}.json`
- `s3-admission-work-capture`：`{raw_work_verification,capture_verification}.json`
- `s3-allgather-work`：`{normalization_audit,source_audit,prediction_audit`
- `s3-allocation-layout`
- `s3-allocator-branch-evidence`：`capture_plan.json`
- `s3-allocator-lifecycle`
- `s3-allreduce-work`
- `s3-attention-parameter-contract`：`parameter_audit.json`
- `s3-attention-parameters`：`project.json`
- `s3-base-coverage`：`run-20261009-165500-316751`
- `s3-base-coverage-project.json`
- `s3-base-coverage-workload.json`
- `s3-batch-filter`
- `s3-batch-filters`：`run-20261009-180516-413547`
- `s3-batch-filters-project.json`
- `s3-batch-merge`
- `s3-batch-output-region`：`contract_audit.json`
- `s3-batch-parameter-contract`：`parameter_audit.json`
- `s3-batch-transition-contract`：`{contract_audit,verification}.json`
- `s3-binding-consumption`：`run-20261009-154435-027805/run.json`
- `s3-binding-consumption-project.json`
- `s3-block-wrapper`：`run-20261009-152919-034785/base/attempt-1/profile_manifest.json`；`run-20261009-152919-034785/run.json`
- `s3-block-wrapper-project.json`
- `s3-cache-input-parameters`：`cache_input_parameter_audit.json`
- `s3-call-clock-boundaries`：`run-20261009-194153-817354`；`run-20261009-194153-817354/base/attempt-1`
- `s3-collective-completion`：`completion_audit.json`
- `s3-collective-deployment`：`allgather_entry_contract.json`；`allreduce_entry_contract.json`；`capture_container_evidence.json`；`native-logs`；`small_algorithm_comparison.json`
- `s3-collective-parameters`：`parameter_audit.json`
- `s3-collective-sampling`：`run-20261009-080543-518320`
- `s3-collective-sampling-project.json`
- `s3-collective-submission`
- `s3-collective-workloads`：`collective_identity_audit.json`；`collective_rank_work_audit.json`；`hccl_mechanism_audit.json`
- `s3-communication-contract`：`communication_audit.json`；`source`
- `s3-communication-groups`
- `s3-communication-node-work`：`node_work_audit.json`
- `s3-communication-operations`：`operation_audit.json`
- `s3-communication-production`
- `s3-communication-regions`：`region_audit.json`
- `s3-communication-templates`：`template_audit.json`
- `s3-communicator-config`：`normalization_audit.json`
- `s3-component-regions`
- `s3-concat-arity`
- `s3-cost-input-isolation`：`verification.json`
- `s3-cpu-region-scopes`
- `s3-cpu-sampling`：`run-20261009-072014-953846`
- `s3-decode-allocation-layout`
- `s3-decode-length-rows`：`verification.json`
- `s3-decode-positions`
- `s3-decode-update-contract`：`{contract_audit,verification}.json`
- `s3-device-sampling`：`run-20261009-073115-536639`
- `s3-dma-work-contract`：`verification.json`
- `s3-eager-cache-copy`：`remaining_kernels.json`
- `s3-embedding-parameters`：`embedding_parameter_audit.json`
- `s3-event-wait-projection`
- `s3-extend-allocation`
- `s3-ffts-dependencies`：`run-20261009-185006-876203`
- `s3-filter-branch-boundaries`：`source_audit.json`
- `s3-filter-region-contract`：`source_audit.json`
- `s3-filter-scope-binding`：`verification.json`
- `s3-forward-input-evidence`：`source_basis.json`
- `s3-forward-positions`
- `s3-fused-qkv-parameters`：`fused_qkv_parameter_audit.json`；`native_split_qkv_rmsnorm_rope.py`；`project.json`
- `s3-greedy-sampling`
- `s3-grouped-release`
- `s3-host-clock-audit`：`host_clock_audit.json`
- `s3-host-frontier`：`host_frontier_edge_audit.json`
- `s3-host-frontier-clock`
- `s3-host-wait-audit`：`device_sync_scope.json`；`{representatives,raw_trace_check,vendor_export,association_check}.json`
- `s3-idle-control-contract`
- `s3-idle-schedule`：`run-20261009-181931-249408`
- `s3-idle-schedule-project.json`
- `s3-indexed-write-branch`
- `s3-instance-topology`：`topology_audit.json`
- `s3-joint-serving-capture`：`component_hook_verification.json`；`inline_binding_audit.json`；`successful_capture_outcome.json`；`{corrected_capture_outcome,component_binding_audit}.json`
- `s3-kt-base`：`run-20261009-105819-038606`；`run-20261009-105819-038606/base/attempt-12`
- `s3-kt-runtime-*`
- `s3-kv-parameter-contract`：`parameter_audit.json`
- `s3-kv-production-bindings`
- `s3-kv-target-work`
- `s3-last-location-region`：`{contract_audit,verification}.json`
- `s3-logits-fragment-boundaries`：`fragment_boundary_audit.json`
- `s3-logits-materialization`：`cpu_operation_audit.json`
- `s3-logits-operation-inputs`：`input_contract_audit.json`
- `s3-logits-output-parameters`
- `s3-logits-parameters`：`baseline_result.json`
- `s3-logits-selection-contract`：`contract_audit.json`
- `s3-logits-transpose`
- `s3-logits-view-replacement`：`replacement_audit.json`
- `s3-mechanism-facts`：`verification.json`
- `s3-membership-production`：`membership_audit.json`
- `s3-memcpy-cpu-association`：`cpu_coverage.json`；`{verification,postprocess_verification}.json`
- `s3-merge-phase-contract`：`{source_audit,verification}.json`
- `s3-merge-region-contract`：`{contract_audit,verification}.json`
- `s3-metadata-region`：`prepared_input.json`
- `s3-metadata-source-evidence`：`capture_plan.json`；`native_region_audit.json`
- `s3-model-output-norm`
- `s3-model-parameter-contract`
- `s3-model-parameters`：`build.log`；`parameter_mapping_audit.json`；`project.json`；`unqualified-project.json`
- `s3-model-regions`：`capture/run-20261010-050332-777604/base/attempt-1`；`node_durations.json`；`prediction_project.json`；`project.json`
- `s3-native-copy-association`：`source`
- `s3-native-path-qualification`：`verification.json`
- `s3-normalization-parameters`：`project.json`
- `s3-npu-allocation-branches`
- `s3-operation-contract`：`operation_contract_audit.json`
- `s3-operation-cuts`
- `s3-operation-ports`
- `s3-operation-regions`：`operation_region_audit.json`
- `s3-operation-removal`：`removal_plan_audit.json`
- `s3-output-policy-observation`：`verification.json`
- `s3-page-table-layout`
- `s3-position-layouts`
- `s3-position-values`
- `s3-prefill-concat-replacement`：`verification.json`
- `s3-prefill-filter-coverage`：`run-20261009-183204-607507`
- `s3-prefill-filter-coverage-project.json`
- `s3-prefill-input-region-audit`：`region_audit.json`
- `s3-prefill-structure-mechanisms`：`mechanism_audit.json`
- `s3-prefix-request-binding`：`verification.json`
- `s3-prepare-operation-cuts`：`verification.json`
- `s3-publication-coverage`：`run-20261009-171356-351565`
- `s3-publication-coverage-project.json`
- `s3-publication-output-branches`：`source_audit.json`
- `s3-qkv-slice-parameters`：`qkv_slice_parameter_audit.json`
- `s3-region-ownership`：`prediction_result.json`；`project.json`；`run-20261010-041055`；`run-20261010-041055/extract_result.json`；`run-20261010-041055/native_capture.json`；`run-20261010-041055/profile_manifest.json`
- `s3-region-production`：`binding_audit.json`；`structure_gap_inventory.json`
- `s3-region-request-slots`：`verification.json`
- `s3-relay-layout`
- `s3-representative-configurations`：`verification.json`
- `s3-representative-pool`
- `s3-request-pool-contract`：`parameter_audit.json`
- `s3-request-pool-domain`
- `s3-request-publication-contract`：`{prepared_input,contract_audit}.json`
- `s3-request-region-identity`：`contract_audit.json`
- `s3-request-table`
- `s3-request-table-qualification`
- `s3-rope-parameters`：`project.json`；`rope_parameter_audit.json`
- `s3-rotary-dataflow`
- `s3-sampling-positions`：`contract_audit.json`
- `s3-sampling-region`：`contract_audit.json`
- `s3-schedule-entry-boundary`：`source_audit.json`
- `s3-schedule-state-domain`：`source_audit.json`
- `s3-scope-identity`：`run-20261009-140024-246870/base/attempt-1/profile_manifest.json`
- `s3-sequence-capacity`
- `s3-sequence-maximum`
- `s3-serving-bindings`：`run-20261009-154010-712196`；`run-20261009-154010-712196/run.json`
- `s3-serving-bindings-project.json`
- `s3-serving-boundary-coverage`：`source_audit.json`
- `s3-serving-coverage`：`remaining_kernel_inventory.json`
- `s3-serving-mechanism-coverage`：`remaining_mechanisms.json`；`remaining_mechanisms_current.json`
- `s3-serving-parent-boundary`：`verification.json`
- `s3-sglang`：`run-20261009-004459-898044`；`run-20261009-004459-898044/base/attempt-4`；`run-20261009-033816-444061`；`run-20261009-035109-095735`
- `s3-sglang-project.json`
- `s3-single-request-prefix`：`verification.json`
- `s3-source-composition`：`resource_role_evidence.json`
- `s3-stream-wait-cpu-association`：`verification.json`
- `s3-support-policy`：`commands.json`
- `s3-template-production-bindings`
- `s3-template-production-boundary`：`verification.json`
- `s3-topology-boundaries`
- `s3-worker-region-scopes`
- `s3-workspace-allocator`
- `s4-admission-entry-contract`：`queue_work_audit.json`；`{source_audit,verification}.json`
- `s4-admission-latch`：`{source_audit,verification}.json`
- `s4-admission-member-work`
- `s4-admission-policy`：`{source_audit,verification}.json`
- `s4-admission-region-domain`：`{span_kinds,verification}.json`
- `s4-admission-sequence`
- `s4-admission-source-work`：`extraction_verification.json`；`inline_composition_verification.json`；`verification.json`
- `s4-admission-work`：`verification.json`
- `s4-allocation-addresses`
- `s4-attention-addresses`
- `s4-batch-assembly`：`run-20261009-155120-887013/run.json`
- `s4-batch-assembly-project.json`
- `s4-batch-template-selection`：`verification.json`
- `s4-call-clock-boundaries`：`run-20261009-195230-490467`
- `s4-candidate-qualification`：`verification.json`
- `s4-candidate-selection`：`verification.json`
- `s4-cardinality-qualification`
- `s4-collective-cohort-qualification`：`verification.json`
- `s4-collective-groups`：`verification.json`
- `s4-collective-operation-ports`：`verification.json`
- `s4-collective-port-mapping`：`mapping_audit.json`
- `s4-collective-replacement`：`verification.json`
- `s4-collective-replacement-boundary`：`boundary_audit.json`
- `s4-compiled-template-asset`：`verification.json`
- `s4-control-parameters`
- `s4-cpu-order`：`run-20261009-160242-221144/run.json`
- `s4-cpu-separator`：`verification.json`
- `s4-cross-rank-template-selection`：`verification.json`
- `s4-decode-allocation-replacement`：`verification.json`
- `s4-decode-branch-inventory`：`branch_inventory.json`
- `s4-decode-context-domain`：`{source_inventory,verification}.json`
- `s4-decode-prepare-domain`：`{source_inventory,clone_qualification,verification}.json`
- `s4-decode-transition-scopes`：`{scope_verification,verification}.json`
- `s4-decode-update-region`：`{source_qualification,verification}.json`
- `s4-definition-sharing`：`run-20261009-143151-453942/prediction/round-1/result.json`
- `s4-duration-source-set`
- `s4-explicit-sampling-boundary`：`verification.json`
- `s4-ffts-dependencies`：`validation/run-20261009-191432-547363`
- `s4-filter-membership`：`source_update_inventory.json`
- `s4-filter-selection`：`run-20261009-182051-345469`
- `s4-fixed-asset-validation`：`{workload.json,100,200}`
- `s4-fixed-warmup`：`verification.json`
- `s4-frontier-assembly`：`run-20261009-155702-317424/run.json`
- `s4-given-early`：`run-20261009-164048-370789/run.json`；`run-20261009-164838-051525/run.json`
- `s4-given-missing`：`run-20261009-164132-279894/run.json`
- `s4-given-short`：`run-20261009-164048-102413/run.json`；`run-20261009-164351-538249/run.json`
- `s4-host-frontier`：`run-20261009-193105-718967`
- `s4-idle-schedule`：`run-20261009-182754-454575`
- `s4-idle-worker`：`verification.json`
- `s4-independent-arrivals`：`selection.json`
- `s4-independent-merge`：`selection.json`；`verification.json`
- `s4-independent-sequences`：`{selection.json,staggered,early-finish,joint-prefill,cross-page}`
- `s4-independent-template-production`：`verification.json`
- `s4-indexed-broadcast-parameters`
- `s4-indexed-variant-replacement`：`replacement_audit.json`
- `s4-inserted-bindings`：`verification.json`
- `s4-inserted-boundaries`：`boundary_audit.json`
- `s4-inserted-parameters`：`parameter_audit.json`
- `s4-invocation-waits`
- `s4-joint-serving`：`schedule_prefix_audit.json`；`verification.json`
- `s4-kernel-parameters`：`profile_manifest.json`
- `s4-kernel-parameters-execution`：`run-20261009-165305-976042/run.json`
- `s4-kv-page-identities`
- `s4-kv-pages`：`run-20261009-180014-705380`
- `s4-kv-pages-project.json`
- `s4-kv-region-branches`：`verification.json`
- `s4-length-domain-gap`：`atb-source/audit.json`
- `s4-local-topology`：`run-20261009-144120-119533/prediction/round-1/result.json`
- `s4-logical-page-pool`：`verification.json`
- `s4-materialization-planner`：`planner_audit.json`
- `s4-multisource-donors`：`cohort/prediction/run-20261012-082107-755549`
- `s4-native-cleanup`：`project.json`
- `s4-node-bindings`：`verification.json`
- `s4-node-roles`：`run-20261009-145318-732558/prediction/round-1/result.json`
- `s4-observed-admission-work`：`verification.json`
- `s4-observed-coverage`：`run-20261009-170231-195602/run.json`；`run-20261009-170617-679205/run.json`；`run-20261009-171154-537234/run.json`；`run-20261009-171944-420913/run.json`
- `s4-observed-coverage-project.json`
- `s4-operation-boundary-ownership`
- `s4-operation-ports`
- `s4-operation-replacement`：`replacement_audit.json`
- `s4-page-boundary-sequence`：`{selection,verification,page_layout_timeline}.json`
- `s4-page-table-rewrite`：`{kernel_comparison,physical_verification,verification}.json`
- `s4-parameter-coverage`：`coverage_audit.json`
- `s4-prefill-filter-coverage`：`run-20261009-184232-161457`
- `s4-prefill-input-budget`：`{source_audit,verification}.json`
- `s4-prefill-only-base`：`run-20261009-115811-362211`
- `s4-prefill-release`：`verification.json`
- `s4-publication-contract`：`verification.json`
- `s4-publication-cpu-progress`：`{source_inventory,normalized_topology,topology_verification,verification}.json`
- `s4-publication-cpu-work`：`{prediction_result,verification}.json`
- `s4-publication-membership`：`{six-request,run}`
- `s4-publication-output`：`observation_contract_check.json`；`verification.json`；`{source_calls,source_regions,inventory}.json`
- `s4-publication-output-policy`：`verification.json`
- `s4-publication-rebind`：`{100,200}`
- `s4-publish-residual-work`：`loop_boundaries.json`；`{residual_work,audit}.json`
- `s4-qualified-planner-boundary`：`verification.json`
- `s4-region-order-connections`：`verification.json`
- `s4-region-resource-inventory`
- `s4-region-resource-ports`：`verification.json`
- `s4-region-resource-removal`：`verification.json`
- `s4-release-completion-ports`：`verification.json`
- `s4-release-group-selection`
- `s4-release-list-domain`
- `s4-release-membership`
- `s4-release-transfer`：`verification.json`
- `s4-representative-capacity`：`verification.json`
- `s4-request-length-domain`：`{100,200}/verification.json`
- `s4-request-pool`：`run-20261009-173035-353049/run.json`；`validation/run-20261009-174410-623297`
- `s4-request-pool-domain`：`run-20261009-173316-605567/run.json`
- `s4-request-pool-durations.json`
- `s4-request-progress`：`{verification,progress_fields}.json`
- `s4-request-relay`：`run-20261009-175324-224244`
- `s4-request-slot-operands`
- `s4-request-slots`
- `s4-resource-frontiers`：`run-20261009-160753-511119/run.json`
- `s4-resource-wait-import`：`verification.json`
- `s4-retained-resource-frontiers`：`verification.json`
- `s4-schedule-region-ports`：`wait_boundary_audit.json`
- `s4-schedule-rewrite-gap`：`verification.json`
- `s4-semantic-subgraph-cuts`：`verification.json`
- `s4-shared-node-structures`：`verification.json`
- `s4-shared-region-structures`：`verification.json`
- `s4-single-request-prefill`：`verification.json`
- `s4-source-boundary-identity`：`prediction/run-20261012-073547-668811`
- `s4-source-cost-provenance`：`missing-cost/run-20261012-072650-180000`
- `s4-source-selection`：`run-20261009-142644-862580/prediction/round-1/result.json`
- `s4-stream-wait-semantics`：`prediction_result.json`；`project.json`；`run-20261010-043110`；`run-20261010-043110/extract_result.json`；`run-20261010-043110/profile_manifest.json`
- `s4-structure-summary`：`run-20261009-161052-253056/run.json`
- `s4-subgraph-rebinding`：`broadcast_port_facts.json`
- `s4-target-connections`：`verification.json`
- `s4-target-graph`：`profile_manifest.json`
- `s4-target-graph-execution`：`run-20261009-161957-301197/run.json`
- `s4-target-stream-waits`：`verification.json`
- `s4-template-connections`：`run-20261009-154735-071716/run.json`
- `s4-template-connections-project.json`
- `s4-terminal-region-ports`：`verification.json`
- `s4-transition-contract`：`verification.json`
- `s4-transition-cpu-domain`：`{verification,prepare_inventory}.json`
- `s4-transition-latch`：`verification.json`
- `s4-transition-latch-parameter`：`source_merge_entries.json`
- `s4-transition-replacement`：`{region_application,verification}.json`
- `s4-transition-selection`：`{region_application,verification}.json`
- `s4-wait-frontier`：`profile_manifest.json`
- `s4-worker-controls`：`{prepared_input,prediction_result,verification}.json`
- `s4-workspace-growth`：`device_wait_audit.json`
- `s4-workspace-serving`：`project.json`
- `s4-zero-admission-domain`：`{source_qualification,verification}.json`
- `s5-delayed-arrivals-prediction.json`
- `s5-module-base`：`run-20261009-090446-723531`
- `s5-prediction-project.json`
- `s5-prediction.json`
- `s6-automatic-project.json`
- `s6-combination-gap-isolation`：`verification.json`
- `s6-exploration-set-contract`：`verification.json`
- `s6-prediction-host-recovery`：`verification.json`
