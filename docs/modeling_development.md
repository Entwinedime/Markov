# Modeling 开发与使用

本项目从一个 base 的真实采集构建 source DAG，根据 target 配置推演缓存行为、工作量和耗时。
用户提供平台/框架、model、base config、workload 和 target config 集合，流程负责采集、构图和预测；
已有 profile 是复用方式，不是首次使用必须手工制作的输入。

~~~text
base 配置 + model + workload → base 采集 → 原始 DAG → 目标工作与依赖规则
独立性能采集 → 性能数据库 → 各类成本模型
两者汇合：prepare-hicache 填入迁移/预测成本，仿真并报告完整 HTTP
有 target 实测时：evaluate 独立评分，不参与应用预测
~~~

预测不需要 target trace。参数原理见 [HiCache 数值模型](hicache_io_cost_model.md)，
验收口径见 [验证说明](validation/hicache_validation.md)，当前结果及数据资产见 [工作进展](work_progress.md)。

10-10 已清理不再使用的历史实验资产。本页旧证据路径仅描述当时验证，不保证仍可运行；
现存采集、校准、静态回放输入及最新稳定对照以工作进展的[数据资产](work_progress.md#数据资产)为准。

10-10 的稳定对照及 10-12 的上一轮整理已完成完整 HTTP、源回放与相关代表验证；具体范围和限制以
[工作进展](work_progress.md)为准。10-13—10-14 代码分层重构已完成 clean 构建及 186 格预测和独立评分，
完整 HTTP 与重构前逐项保持；结构及静态诊断的范围另列，不以旧成绩代替新验证。

## 1. 当前能做什么

- SGLang：预测 HiCache 配置变化引起的 I/O、控制流程及 Prefill/Decode 变化，输出完整 HTTP 正式窗口时间。
- KTransformers：与 SGLang 共用 profile manifest、DAG 构建和仿真；没有 HiCache，不接受 HiCache 预测。
- NodeScale：框架无关的可选节点耗时缩放，默认关闭，不能替代 HiCache 或计算阶段建模。

完整预测不排除 gap、未归属成本或 Prefill/Decode，但部分残余等待仍沿用 base 近似。
“执行完成”不是“误差达标”，也不证明所有目标的成本都能可靠外推。

部署信息与服务成本依据分别准备；变化操作若缺少适用的独立成本依据，可能需要补采，并非先采完整物理包。
按需补采尚未覆盖所有写入、淘汰和局部返回分支。
已验证的同 TP 预测不意味着支持 TP=2→TP=4 扩图：更换 TP、模型或资源环境需要相应的 base 和适用校准。
采集命令省略 TP 参数时，组环境匹配和控制校准均采用 SGLang 的默认 TP=1，不要求用户重复声明。
详细限制以运行结果和成本来源报告为准。

共享控制补采按实际实验去重：本地提前返回和 best-effort 等待缺口共用一次 best-effort 实验。
若同时缺少预取查询成本，先完成等待实验，再从已准入的完整/轻量配对来源导出查询成本；
没有适用来源才安排新采集。一次 prepare 不为同一个实验反复补采；仍未覆盖的需求保留在缺口报告中。

## 2. 输入与职责

### 正式使用的输入

应用用户提供平台/框架、model、base config、workload 和 target config 集合，另指定输出位置与允许的采集开销。
现行配置中，框架、模型及 base 启动参数由 `profile_suite` 指向的采集配置声明，workload 由其中的 `bench` 声明；
平台资源可由采集记录或 `resource_environment` 指定。已有 `base_manifests` 是复用入口，不代替这一首次使用合同。

平台性能维护负责独立采集、数据库和模型定义。测量可以来自其他独立 runtime trace 或物理采集，
但复用须满足实际平台、实现、工作变量和计时口径。应用流程消费适用模型，不让用户逐 target 挑参数或连接内部模块。
数据库保存测量事实，成本模型将目标 DAG 的工作描述转换为耗时；base 则保留结构依据与确实未变工作的迁移成本。

两条主线在成本填充与仿真时汇合，不要求先完成 target profiling，也不表示设备采集并行。
应用仍需引用已有成本包，或数据库与训练声明；独立性能准备有自己的入口，不要求先建立 target 组。
采样域和模型定义须明确，不保证从任意空库自动辨识全部操作，详见独立性能准备。

### 代码分工

同一份信息尽量只有一个所有者；观察到的源执行与预测出的目标执行不是两份需要互相同步的状态。

| 信息 | 所有者 | 不负责什么 |
| --- | --- | --- |
| 用户流程及缺数据恢复 | Python prepare、cost_preparation、acquisition | 组织阶段，不实现图算法或成本公式；默认不启动补采 |
| 源节点、调用、请求和原始依赖 | source_dag 与 trace 适配 | 不决定 target 分支，不把源耗时当目标完成时刻 |
| 目标工作、缓存状态与条件规则 | structure 与 HiCache model | 不查询成本，不因缺样本省略合法分支 |
| 独立模型与未变成本迁移 | Python modeling/cost、C++ cost | 只返回成本、来源和范围，不选择资源或改变结构 |
| 资源绑定与时间推进 | HiCache runtime、FutureDag、simulation | 执行既定规则，不重新解释 trace 或拟合参数 |
| 源回放、结构比较、性能评分 | 实验报告与独立 evaluate | 不成为应用预测输入，也不回写模型或触发补采 |

输入、阶段衔接及仿真粒度的原则见第 9 节；具体文件只在修改相应职责时阅读。

### 源观测代码的阅读顺序

以下 C++ 路径相对 `src/modeling/trace_graph`，头文件在 `include/markov/trace_graph` 对应层级。
这些职责已通过本轮统一构建与完整数值回归。阅读说明并入本主文档，不恢复已经不在工作区中的单独代码指南。

1. 从 `src/source_dag/build.cpp` 看 manifest 输入如何按原顺序构图和合并。`io/trace_manifest_input.cpp`
   负责通道、时钟和正式窗口；Ascend 事件角色在窗口裁剪前解释，SGLang 观测在构图前分流。
   Level0 的原生事件补充由 manifest 的 `cann_runtime_trace_files` 明确登记；建模不搜索目录，也不把整份 CANN 设备时间线重复入图。
2. `adapters/runtimes/ascend/event_semantics` 与 `adapters/frameworks/sglang/trace_semantics` 解释原生名称。
   `TraceEvent` 保存有限的路由、调用和同步角色；这些是内部源解释结果，不是成本、新的采集字段或版本凭据。
   构图器据此分别保存执行节点与只读观测，归一化同线程嵌套区间，并建立提交和同步依赖。
3. `source_dag/index`、`stream_order`、`call_regions` 解释共同邻接、CPU 区间、设备提交与调用归属。
   `SourceDagIndex` 借用不可变图，不读取缓存事实或成本；CPU 区间与邻接索引分别在首次查询时构建，
   不为只查 CPU 区间的观察提前物化全部依赖。缓存属于当前索引实例，图被修改后必须重建索引。
4. `adapters/frameworks/sglang/source_calls` 产出注意力、分配和通信等源工作，`models/qwen3_calls`
   与 `qwen3_layout` 解释模型角色和布局。Ascend 算子对应及同步读回在 `operator_bindings`，
   HiCache 事实和控制包络在 `hicache_observations`；不再把这些能力藏在 DAG patch 的通用索引里。
5. 目标规划消费这些已解释的工作；独立测量导出读取服务区间。源墙钟、CPU 服务、队列等待、
   设备完成和缓存发布不能合并成一个耗时，也不能由 target 实测补全缺失语义。
   Prefill/Decode 的节点归属按 logical input 与原始半开时间区间查询索引；
   重叠区间仍视为归属冲突，不用“取最近阶段”代替原有因果与线程条件。

### 目标执行与工作流的阅读顺序

从 `modules/hicache/runtime/window.cpp` 的 `HiCacheExecution::run` 看本次执行：准备源依据、
原子替换源片段，再由一个仿真时钟驱动准入、展开、成本绑定与完成。这个对象拥有本 target 的状态，
源图和配置只借用；各组件按明确顺序完成绑定，所有源替换结束后共用最终保留的资源依据。
计算调用只持有尚未展开的模板和工作计划；展开后，节点与依赖由 DAG 持有，局部副本立即释放。
准备总数仍包含待展开与已展开调用，报告在生成工作时累计动作数量，不为计数另存全量计划。

目标工作在 `structure/hicache` 与 HiCache model 中决定；具体 SGLang/Ascend 张量程序在适配层。
`runtime/host_program` 生成共同 host/worker/device 工作，`host_cost_binding` 只绑定数值。
`cost/source_migration` 迁移已证明未变的成本，`cost/operator_model` 查询独立响应，
`cost/io_service` 估计相对服务时间，`runtime/io_completion` 才将其放入执行时钟。
`adapters/frameworks/sglang/allocator_preparation` 只观察源分配工作；源中位数的静态推算留在
`modules/hicache/diagnostics`。结构报告由 `reporting/hicache_structure` 消费结果，不参与规则或成本选择。

Python 从 `applications/prediction.py` 看直接应用，从 `applications/accuracy/cli.py` 看矩阵实验，
从 `preparation/performance.py` 看独立性能维护。`modeling/predictor.py` 是统一数值接口，
只消费已经准备的输入；`modeling/invoke.py` 负责经公开命令调用 modeling 容器。
`ScenarioRequest` 与 `PerformanceRequest` 在边界解析 JSON，内部读明确字段；准备选定的新成本包路径
作为结果传递，不改原输入文件。两条主线共用 `CpuServiceInputs`、采集生命周期及
`cost_preparation` 的入库、离线训练能力。数据库事实和模型选择规则保持原样，默认预测不加载采集器。
这些职责属于 10-13—10-14 重构，clean 构建及 186 格行为回归已通过；不能据此宣称完成新的平台或模型验证。

当前 Python 入口有三种用法：`predict` 直接使用已准备输入，`prepare-hicache` 先准备再预测，
`experiment --request <experiment.json>` 在上层组织矩阵。示例见 `configs/modeling/accuracy_example.json`：
`groups` 引用已有组声明，`execution` 控制并行及报告，`evaluation` 单独引用正常对照与可选结构证据。
全部选中预测结束后才进入正常采集和评分；`--predict-only` 停在预测，`--score-only` 只评分已有结果。
独立评分也可用 `prediction_dirs` 直接引用任意已有预测目录，此时不需要 `groups`，不执行输入准备或预测。
正常对照也可通过 `evaluation.normal_suites` 显式声明，使用关闭 profiling、指定 `run_root/run_id` 的正式采集 suite。
重复次数在 suite 中声明，不由模型自行追加；所有采集仍串行，失败保留，不按耗时筛样本。

Base 与 CPU 准备由 `preparation/base.py` 的 `prepare_inputs` 完成，不需要 target。
公开的 `prepare-hicache --calibration-only` 因此可省略 `target_configs`，不再填虚假 self。
准备过程写入 `input_summary.json`；应用的 `group_summary.json` 引用它并记录最终预测状态。
`modeling/invoke.py` 返回本次退出状态、结构化报告与实际墙钟；数值接口 `predict` 不负责排名或评分。

### 常见修改从哪里入手

先判断变化的是输入解释、执行规则、数值还是报告，再沿该职责修改提供者和实际调用方。
不要为了对齐模块，让后面的阶段重新判断同一个原生字段。

| 要修改的内容 | 主要位置 | 为什么不需要改其余阶段 |
| --- | --- | --- |
| 一个原生 trace 字段或调用名 | `adapters/frameworks/sglang`、`adapters/runtimes/ascend`；模型角色在 `models/qwen3_*`，探针在同级 framework 适配包 | 对外仍提供事件角色、调用观测和工作描述；通用 DAG 不识别新的原生名字 |
| 一个合法 HiCache 分支 | `modules/hicache/model` 与 `structure/hicache`，执行衔接在 `runtime` | 先生成工作与依赖，再查询既有成本接口；不能为了缺模型改成另一分支 |
| 一种成本形式 | Python `modeling/cost/models` 的离线训练与包输出、C++ `cost/operator_model` 的查询 | 消费已有 `OperationWork` 的变量，不改缓存状态或节点生成；I/O 服务另由 `cost/io_service` 估计 |
| 一份实验报告 | C++ `reporting/hicache_structure` 或 Python `applications/accuracy` | 消费已完成的领域结果；不修改工作、系数或准备状态，不触发采集 |

CMake 同样按实际职责装配：共同图、源索引、工作和数值向内独立；HiCache rules 决定策略，
SGLang observations 解释源依据，HiCache execution 将二者与成本、仿真连接，reporting 在外层消费结果。
静态推算及 oracle 回放只在 validation 构建中装配，Release 的正确性不依赖这些诊断。

源样本与独立测量仍有不同的读取方式，但得到的 host 操作计划通过同一个落图入口执行。
资源顺序由操作计划统一给出：先是执行的 stream，再是等待的 stream；同一资源重复出现时不能去重，
显式 event completion 则另外绑定。main、worker、设备服务与 residual gap 也不能合并为一个成本。

源片段替换结束后，再统一观察保留下来的 worker 队列，
加载、层等待、写完成确认与写入共用这份资源依据。各组件不在删除中间片段后分别认定最终队列。
加载和写入的 stream 插入端点也在此时按同一份最终源图刷新。加载提交、分配入口与空加载分支
共用入口规划，先通过完整 mutation 应用改边及成本替换，再登记执行回调。
非活跃层等待也先规划整组插入再应用；allocator preparation 的源坐标在创建时提供，不再事后修补。
候选分支的资源不足仍在目标实际需要时报告；已确定执行的调用则立即要求可用资源。
未启用的层等待仍按源提交顺序定位设备流；查找后续层界限时只访问同一 logical input、同一 forward 的调用，
保留原来的层号与精确时间条件，不为每个调用遍历整份 workload，也不改变目标分支或成本。
当前验证范围及尚未解决的问题见[工作进展](work_progress.md)。

### 输入来源

profiling 与 modeling 只通过 `profile_manifest.json` 交接，不扫描目录猜输入。
运行期间不要修改已选 profile。

一个 base 组可以包含多个 workload，但每个 workload 只选一份 base profile；
这些 profiles 必须属于同一个 base HiCache 配置，模型、TP、后端和资源环境相容。
配置名、workload 名只是身份，不作为拟合变量，也不能代替实际配置与环境核对。

环境比较区分运行资源与采集控制：`HOOK_EMISSION_TIMING` 和
`SGLANG_NPU_PROFILER_SERIAL_EXPORT`、`SGLANG_HICACHE_IO_TIMING` 不作为资源匹配维度，原采集配置仍保留这些开关。
成本适用性也不比较诊断接口的访问凭据值；原部署来源不改写，模型、TP、执行参数和资源仍须匹配。
这不放宽模型、TP、设备或线程资源匹配，也不表示观测开销已全部消除；CPU 校正仍须绑定实际源采集。

target 文件只含 `{name?, hicache}`，示例见
[hicache_target_example.json](../configs/modeling/hicache_target_example.json)。
target 不得携带成本、观测或评分字段。页大小及策略在输入边界校验。
当前调度器加载遵循 HiRadixCache 的 10-token 最小 host 命中规则：更短的命中不发出分配或 H2D，
但提前返回和已有 prefix 拼接仍须计费。统一验证范围见工作进展，不将未观测分支当作已验证。
当前 schedule_policy 未传 `mem_quota`；其他调用方的 quota 限制不属于已经实现的执行路径。
省略预取策略时，输入解析统一补为 `timeout`，不继承 base 的策略；成本需求检查与执行使用同一个值。

完整 HTTP 评分需要 workload 报告声明正式窗口。只有 bench-serving 总时长、没有 trace 起止坐标时，
不能据此裁剪 trace。普通 build-dag 可保留完整 trace；需要正式窗口的构模或评分会报告缺失。

HiCache 预测和结构观察会保留最后一次显式 idle barrier 之后的准备请求，
它们与正式请求共用执行时钟，但不计入正式 HTTP。无需用户手工拼接图；范围来自同一 manifest 的 workload 报告，
原始 trace 必须包含相应准备工作。源回放和 HTTP 摘要单列 `preparation_request_count` 与执行时钟上的 `formal_start_us`，
结构报告的 `request_context` 区分准备/正式数量，`observed_request_start_us` 是首个选中请求的原始观测起点，
不是预测执行时间。I/O 效果和计算操作使用同一准备范围，不能一边保留暖机、一边从正式窗口重新编号。
普通构图、成本导出和历史静态回放不因此隐式扩大各自的选择范围。
当前验证及未覆盖范围见工作进展；局部执行成功不证明目标结构完整或正常 HTTP 精度达标。

## 3. 选择命令

所有命令从仓库根目录运行：

| 需求 | 命令 |
| --- | --- |
| 一个 base 预测一组 target | `scripts/model.sh prepare-hicache --group <group.json>` |
| 独立采样、入库与模型准备 | `scripts/model.sh prepare-costs --request <performance.json>` |
| 通用 source DAG 构建、仿真 | `scripts/model.sh build-dag` |
| 已完成预测的独立评分 | `scripts/model.sh evaluate` |
| 导入独立测量并离线训练 | `scripts/model.sh cost-db import/train` |
| 已有成本包直接预测 | `scripts/model.sh predict` |
| 声明矩阵并独立评分 | `scripts/model.sh experiment --request <experiment.json>` |
| 有限 HiCache 配置搜索 | `scripts/model.sh search --request <search.json>` |

`prepare-cpu-service`、`export-hicache-operation-costs` 和 `calibrate-hicache` 是同一流程的可独立排查阶段，
不是用户必须手工串联的另一套流程。具体参数用对应命令的 `--help` 查询。

`prepare-hicache` 在宿主机协调容器；设备采集进入框架容器，构模与预测进入 modeling 容器。
一次批量预测共用一个 modeling 容器，但整个 prepare 不保证只启动一次容器。
`prepare-hicache` 与 `predict` 均支持 `--trace-threads`（源读取/构图）和 `--trace-file-threads`（文件解析），默认各为 1。
它们与 `--model-run-jobs` 的目标并发数独立；增加线程前须考虑大图内存，不会因此并行设备采集。
modeling 容器默认限制为 320 GiB（内存与内存加交换区上限相同），可通过
`TRACE_SIM_MODELING_MEMORY_LIMIT` 按平台设置；这不能替代并发控制。每个目标有独立可变图，
峰值会随执行增长，不能用源图大小或早期采样估计整批峰值。当前研究首项恢复使用 4 个目标工作进程，后续常规使用 8；仅下一高复用/roomy 根据已完成同源八并发任务的 141.34 GiB 全程峰值安排 12，实际峰值仍需观察。超时补全及有限参照使用 8，一次一个预测批次。
本轮外部 watchdog 在容器达到 280 GiB 或主机可用内存低于 96 GiB 时停止本轮预测容器，
保留中断与计费；320 GiB 容器硬限继续生效。

同一 manifest 和 CPU 校正的目标在一次调用内共用源准备：trace 读取、原始 DAG、客户端锚点、
可选的原始回放，以及 CPU 校正只准备一次。源组顺序执行；`--model-run-jobs` 限制该组的并发目标数，
不再跨不同源组同时构图。Linux modeling 容器在源准备的读取/构图线程退出后派生各目标进程，
每个目标独占可变图、懒加载参数缓存、执行时钟和成本查询统计，不在磁盘上保存可复用的源图缓存。
改变 manifest、窗口、校正或通道须重新准备；单格的 `runner_config.json` 仍可通过公开入口单独复现。

源准备失败影响整组；某个目标缺工作或成本时，只写该目标的失败报告。默认不再启动后续目标，
已经启动的目标正常收尾；独立 `predict` 可用 `--continue-on-error` 继续。
`prepare-hicache` 会执行整组目标以汇总共享缺口，不接受这个开关。批次逐目标返回结果，不等整组结束才显示进度。
普通运行清除成功日志；失败保留目标日志和必要的 `source_preparation.log`，详细诊断才保留成功日志。
生成的 `target_plan.json` 只是当次调度输入，不是版本、有效性凭据或冻结副本。

## 4. 构建普通 DAG

SGLang 和 KTransformers 都使用：

~~~bash
scripts/model.sh build-dag \
  --profile-manifest <profile_manifest.json> \
  --output-dir <dag-output>
~~~

该命令不要求 HiCache model。需要应用 NodeScale 等可选变换时，追加
`--model-config <model.json>`；文件直接包含 `node_scale` 等模型配置，不必另写 runner 配置。
省略该参数时只构建并仿真 source DAG。普通 DAG 路径不提取 HiCache 成本观测，也不执行
HiCache control、去 gap 两次分项回放；摘要不输出这些未计算的指标，不能把它们理解为零。
成本构建的专用观测入口、历史静态 HiCache 回放和评分专用 phase carrier 仍按需执行这些步骤。
普通后端与校准操作导出共用 manifest 输入的规范化和 DAG 构建实现；逻辑输入编号、窗口外上下文
及线程预算由同一处处理，不再由两个入口分别维护。
需要复现已生成的运行配置时：

~~~bash
scripts/model.sh build-dag --config <runner_config.json>
~~~

`--config` 原样复现文件中的运行选项，不能同时传直接构图的输出、模型、线程或 DAG 导出参数；
这些参数只用于 `--profile-manifest` 形式。混用会报错，不再静默忽略用户传入的参数。

批量预测直接使用已解析的运行对象，同时保存同一对象的 `runner_config.json` 供上述命令复现。
文件入口负责校验外部配置；内部执行不再先拼原始字典再重新解析。正式窗口保存在执行参数中，
来源仍是 source manifest 指向的 workload 报告，不额外保存一份无人消费的窗口 metadata。

显式 DAG Chrome 导出使用完整仿真的实际起止时间，包含 CPU 校正；不能把原始节点 duration
当作校正后的运行时长。节点参数同时保留 lane、顺序 CPU 间隔和就绪延迟（微秒），便于区分服务与等待。
依赖 flow 的跨度可能还包含其他前驱造成的等待，不能将所有 flow 长度相加当作关键路径成本。
缺少 HiCache 成本模型的任务在规划阶段跳过；执行适配层只处理已具备模型的任务，直接生成 C++ 模型配置，
不再保留无模型执行分支或外包一层随后拆掉的配置字段。

提供配置中启用的模型就会应用相应变换；只回放 source 时应省略或关闭模型。
不要用旧的 mode 字段假定已声明的变换被屏蔽。

## 5. 一个 base 组

### 首次准备

先准备一个普通采集配置：`server` 声明模型与 base 推理参数，`bench` 声明 workload，
`metadata.config_id/workload_id` 用于选择和展示。不要求 matrix，也不要求包含 target 采集。
模型路径沿用 profiling 的声明规则，不在组输入里再保存一份。

组输入用 `profile_suite` 引用该配置，流程负责记录所需 tokens、采集 base、接收 manifest 并构图。
只有一个 base 时可直接采用所声明的 workloads；多个 base 的实验文件仍须用 `base_config` 明确选择，
按需用 `workload_ids` 缩小范围。显式空列表不是“全部 workloads”。

普通预测优先引用平台维护者已准备的成本包，不要求应用用户先维护数据库或编写逐操作训练声明。
例如尚未采集 base，但已有适用成本包：

~~~json
{
  "profile_suite": "<base-experiment.json>",
  "target_configs": ["configs/modeling/hicache_target_example.json"],
  "cost_model": "<platform-cost-package.json>",
  "base_capture_budget": {
    "wall_seconds": 1200,
    "server_starts": 2,
    "requests": 100,
    "tokens": 100000
  },
  "cpu_service_capture_budget": {
    "wall_seconds": 600,
    "server_starts": 1,
    "requests": 100,
    "tokens": 100000
  },
  "output_dir": "data/modeling_runs/my_hicache_group"
}
~~~

预算数字只是例子，应按实际允许的采集规模填写；CPU 预算使入口能自动安排同源轻量回放，已有校正可显式复用。
资源声明方式见下面的复用例子；实际资源必须与成本包匹配，不能照抄其他平台的标签。
仅有数据库时，按后文用 `cost_database` 和 `cost_training` 替代成本包。新的独立成本补采另行声明，
不从 base 或 CPU 预算借用。运行方式与下面的复用例子相同。
模板 workload 不必预写 token 回放参数；没有已有计划时，入口先记录 tokens，再采集受控 base trace。
显式 replay 计划或组的 token bundle 可跳过记录。两次运行使用各自的新报告目录，不覆盖原 workload 输出；
不需要 HiCache 状态检查的普通 token 模板也不要求配置矩阵文件。
当前仍使用串行模板请求；可记录逐请求的自然 EOS 或长度上限输出，再按实际序列和原结束条件回放。
输出 token 数不是 Decode 执行次数：重叠调度可能在处理结束结果前已启动下一轮，连单 token 输出也可能有 Decode 分配。
分配、计算与释放必须依据实际执行观测和框架规则。生命周期需要的新页若缺少对应分配边界，报告结构缺口及请求、页宽、
已扩展/已 Decode/已提交 token 数；增加成本样本不能补齐这种结构信息，也不能把缺失分配当成零成本。
已在原始 trace 中观测到的窗口前成功分配作为状态上下文保留；它初始化尚未结束的暖机请求，不生成正式窗口的成本节点。
目标策略在该请求的结束阶段新增写入时，由 scheduler 的计算流与 worker 角色绑定资源，不要求源中存在同类写入模板。
输出上限用于预留预算，实际 token 用于完成后的耗用和回放；单条请求可覆盖输出上限。
正式采集前，先在 workload 配置中控制请求数量和逐请求输出上限，并保留本次需要验证的执行路径。
这两个量直接影响 trace 和 base DAG 的规模；程序不按实验名强制限额，也不在构图时悄悄裁掉请求或节点。
若改变工作量，重新准备匹配的记录、profile 和 CPU 校正；实验中的 target 预测和正常测量使用同一实际序列，不能沿用原负载真值。
这不是对尚未生成内容或任意在线 EOS 的预测，也不表示并发请求或较长 Decode 的完整建模已通过本轮验收。

性能数据库独立于这一组长期维护；训练声明负责选择适用部署和各类模型，整组共用一个结果包。
空库、新后端或不适用的模型仍需独立性能采集，现行入口不能仅凭空库自动采齐全部计算、I/O 和控制成本。
数据不足与结构不支持分别报告，不启动 target 实测来补洞。

这里有两种使用职责，而不是要求每个预测用户维护一套模型：
用户提供本次平台、模型、base、workload 和 target 集合；性能维护者为平台准备可复用的数据库与模型定义。
库中有适用数据时直接复用，没有时进行独立性能采集。实际查询由目标工作描述选定操作、实现、口径与变量，
不是由用户为每个 target 手工挑模型。现行配置仍要引用成本包或数据库/训练声明，尚未实现只填平台名就自动发现这些资产。

### 已有采集的最小起点

复制 [hicache_group_example.json](../configs/modeling/hicache_group_example.json)，替换实际路径：

~~~json
{
  "base_manifests": ["<base-profile>/profile_manifest.json"],
  "target_configs": ["configs/modeling/hicache_target_example.json"],
  "cost_model": "<independently-trained-cost-package>.json",
  "resource_environment": {
    "device": "<device model and allocation>",
    "host": "<CPU model and process affinity>",
    "storage": "<storage medium and filesystem>",
    "implementation": "<framework runtime and operator implementation>"
  },
  "output_dir": "data/modeling_runs/my_hicache_group"
}
~~~

target 也可以直接写成 `{"name": "...", "hicache": {...}}`。已有 base 时不需要矩阵 suite，
base/workload 身份可从 manifest 推导；没有显式 metadata 名称时使用配置名称或 run 身份。
多个 workload 应显式使用相同 config_id 和不同 workload_id，评分时与 target 身份对应。
普通单配置没有这些标签时，准备入口按配置的 name/id 识别 base，从所引用模板的 id 识别 workload；生成采集配置会保留这些身份，
不会因运行名称变化而把新 manifest 当成另一份 base。

先检查输入，再执行：

~~~bash
scripts/model.sh prepare-hicache --group <group.json> --dry-run
scripts/model.sh prepare-hicache --group <group.json>
~~~

最小配置使用已有独立成本包，不启动自动成本补采。成本包仍须满足部署条件和目标操作的实际需求，
缺少模型或测量覆盖时明确报告缺口，不能保证任意成本包都能完成预测。
普通预测不恢复物理或控制校准文件；这些输入只在显式共享补采需要使用时才解析与准入。

只有数据库而没有成本包时，删除 `cost_model`，改为声明：

~~~json
{
  "cost_database": "<independent-cost-database.sqlite>",
  "cost_training": "<training-request.json>"
}
~~~

其余 base、target 和输出字段不变。同一入口先离线训练，再用生成包预测全部 target；
不要求用户先执行另一条训练命令。训练请求仍要明确部署、模型定义和 HiCache 所需 I/O 依据，
不是仅提供任意数据库就能预测。新包写入组输出的 `cost_model.json`，自动排除当前 base 的采集身份与 manifest。
用户提供的共享输入包不会被恢复流程覆盖；同时声明成本包和数据库时优先使用已有包。

`--dry-run` 不启动设备采集、CPU 配对准备或 target 预测，但会写准备摘要，不是完全无写入的语法检查。
`--calibration-only` 只准备 source 输入及其同源 CPU 校正，可不提供成本包；成功状态为
`source_inputs_prepared`，不代表独立成本齐全或目标已可预测，也不执行成本库训练。
修改观测提取规则后，成本库通过显式 `cost-db import --replace-captures` 重导入完整的选中采集，
随后重新训练成本包；不存在 `--refresh-observations` 参数。正式预测会重新构建源图，不能以旧预测输出代替当前代码结果。

### 已有校准与预算

只有工作、实现和资源条件确实未变的操作才能迁移 base 实测；变化或新增操作查询独立成本包，
不再按 base 数据临时拟合后外推。成本包已有依据时直接预测，独立数据库不足时才考虑共享补采。
不能把其他 base 或 target 的观测改名后当成独立校准。
已有独立物理测量能提供缺少的服务参数时，直接复用，不为缺少运行时倍率强制再采 workload；
构模摘要会注明这项估计尚未验证推理负载下的竞争影响。CPU 控制、计算和执行分支缺口仍独立处理。
预测阶段的普通设备释放缺口也可进入共享补采：使用同一 `budget` 计费，经过完整/轻量配对与 CPU 校正后导出，
供整组使用并恢复预测。预算不足会保留失败格和具体成本需求；不会借用备份释放成本，也不无限重试同一缺口。
补采完成后与首次准备共用观测准入、成本检查和构模路径；不另外拼装模型，已有适用观测继续复用。
已有适用的校正释放文件可直接声明在 `control_calibrations.release_host`；独立导出命令支持 `--operation release_host`。

在组配置中按需加入以下字段。显式开启控制补采时，生成器还需要平台几何（包括存储批次页数）：
可由已有 `physical_calibration` 声明或已准备的 platform inputs 提供。
成本数据库里存在物理 I/O 测量，并不会自动把它变成组配置的平台输入；缺该输入时会报告
`fixed calibration requires platform physical calibration`。这不意味着需要重新测量带宽，
可以直接声明已有的独立平台报告。应用预测使用完整成本包时不要求这一补采输入。

~~~json
{
  "physical_calibration": {
    "report": "<physical-report.json>",
    "measurement_sources": ["<original-measurement.json>"],
    "measurement_description": "Target-independent platform measurements."
  },
  "budget": {
    "wall_seconds": 1200,
    "server_starts": 3,
    "requests": 100,
    "tokens": 100000
  }
}
~~~

这只是字段补充示例，须与 base、targets 和 output_dir 放在同一组配置中；预算值由实际可接受开销决定。
已有独立 profile 使用 `cost-db import --profile-manifest` 入库，再离线训练成本包；
组配置不维护一份没有消费方的校准 manifest 清单。

各类采集的预算分开声明：

共享 workload 补采生成一个显式实验，不再构造单行单列矩阵；server、bench 和普通配置/workload 标签直接声明。
仍保留 suite 结果清单和 token bundle，用于确认实际产物及完整/轻量回放；不要求用户手工维护这些生成输入。

| 情况 | 组字段 | 必要输入 |
| --- | --- | --- |
| 缺 base profile | `base_capture_budget` | 普通 `profile_suite` 配置；多 base 时明确选择；已有 token plan/bundle 可复用 |
| 缺 base 的轻量 CPU 回放 | `cpu_service_capture_budget` | 可从已选 base 自动规划；默认复用 base 报告记录的 token plan |
| 缺共享 workload 依据 | `budget` | base 的合法 token 输入 |
| 缺物理服务或 CPU 原语测量 | `physical_capture.budget` | 对应物理采样域、设备与 CPU 绑定 |

前三类使用 wall_seconds、server_starts、requests、tokens；物理采样使用
wall_seconds、container_starts、logical_io_bytes。省略 workload budget 等价于零预算。
启动、失败、重试和无效 trace 都计费；墙钟保留 30 秒用于超时清理，不能换目录绕过累计预算。
普通采集和 base 轻量回放共用产物记账：终态 manifest 明确说明 workload 未启动时，请求和 token
不扣预留量，启动次数与已用墙钟仍计费；缺少该证据时保守计费。失败命令不会因产物存在而变成成功。
每项补采按自己的规模检查累计预算，不因某个大实验超限而跳过后续较小实验或已有测量复用。
例如物理 I/O 与锁定候选 CPU 原语共用物理预算，但 CPU 原语不消耗 I/O 字节配额；控制 workload 使用另一份预算。
成功补采后继续预测，新出现的需求仍按相应账本检查。被拒绝的同一项需求本次运行不重复尝试；
流程不能自行突破预算、清空账本或把未完成测量当作零成本来继续。
经用户授权调整累计上限时，只更新组配置的预算，历史成功、失败和重试用量仍保留。
某项缺少采样声明、适用采样器或物理预算时，保留该项缺口，其他独立需求仍可继续补充；
这类未启动采集的情况不视为进程失败。同一项需求本次运行只尝试一次，不反复生成相同实验。
例如只声明了 `eviction_cpu` 的组遇到 I/O 缺口，会提示补充 `physical_capture.page_token_sizes`，
不会因为缺少 I/O 采样域而抛异常、中断本来独立的 CPU 或控制补采。
进程失败、未完成清理等其他停止原因仍终止后续采集；已取得的成本保留，并在可构模时统一用于重新预测。
base 补采按 profiling 的正式展开规则选择实验，保留 experiment 覆盖与 `$unset`；
每个 base/workload 必须唯一匹配一个已声明实验，不从两个矩阵轴重新拼出被排除的组合。
`profile_suite` 也可指向普通单实验或显式 experiments 文件，不要求 matrix；使用
`metadata.config_id/workload_id` 标明要采集的 base 与 workload。只有一个 base 时无需重复选择；
已有显式 token plan 时只回放一次；
使用 `{forced_token_plan}` 时，才从所给 bundle 取计划，或先捕获 tokens 再回放。
target 若用实验配置名简写，读取展开后的实际 server 命令；同名实验的 HiCache 配置不一致时，
改用明确的 target 对象或文件，不任取其中一个。标签仍不进入成本拟合。

例如声明物理采样：

~~~json
{
  "physical_capture": {
    "page_token_sizes": [32, 64, 128],
    "devices": [2, 3],
    "cpu_sets": "<rank-0-cpus>|<rank-1-cpus>",
    "budget": {
      "wall_seconds": 900,
      "container_starts": 2,
      "logical_io_bytes": 100000000000
    }
  }
}
~~~

页域是平台的显式采样域，不从 target 列表反推。设备与绑核须匹配 base 环境。
真正需要补采时才从 base 部署读取 KV 几何和存储范围，不要求普通预测声明 `physical_capture`；这不启动测量，
也不提供任何测得的成本。宿主机无法读取 base 模型的 config.json 时，报告具体路径及已有校准替代方式。
修改采样次数、页域或采样绑核不会使这份部署信息失效。
实际 I/O 补采仍须声明页域、适用放置和预算；没有页域时不会自动沿用账本中的旧 I/O 测量。
仅补锁定淘汰候选 CPU 原语时声明 `physical_capture.eviction_cpu` 的 heap_sizes、batch、repeats，
不要求 DMA 页域，逻辑 I/O 字节为零。原理及外推限制见[锁定候选成本](hicache_io_cost_model.md#淘汰时跳过锁定候选的-cpu-成本)。
另一组已有适用测量时，可在 `physical_capture.eviction_cpu.reuse_ledger` 显式填写其
`physical_capture_ledger.json` 路径。只复用环境、CPU 绑定和采样域相符的成功测量；
来源不相符则报告缺口，不静默换成新采集。旧账本及耗用留在原处，新组引用原始报告，不复制或清零历史用量。

### CPU 采集开销校正

CPU 成本校正比较同一个 base 的完整采集与轻量回放，不拿 target 正常耗时拟合 base。
新采集的 base 轻量回放与共享控制校准的轻量回放使用同一验收规则：manifest/workload 完成、
没有采集错误、forced-token replay 验证通过，且仅含 LD_PRELOAD 通道。命令成功不能替代这些条件；
CPU 配对所需计时文件继续由对应采集路径检查，失败尝试仍计入预算。
绑定 forward 校正时，先按原线程和时间范围选出节点与跨界 gap，再按原节点顺序分摊整数预算；
校正区间按线程批量写入。这样避免长 Decode 对每轮重复扫描整图或逐条搬动区间数组，不改变校正公式。
层调用按线程、请求、阶段和 batch 一次分组，再按 forward 顺序查找；不为每份测量重新扫描全部层调用。
输入归属、区间重叠和整数预算检查在正式构建中保留；逐节点/整段 gap 的成本表示恒等复核只在
`TRACE_GRAPH_DEBUG=ON` 的显式诊断构建中运行，不能把这项复核当作目标预测精度。
Python 配对也按原 PID/TID 对同步区间一次建索引，forward、layer 和主机片段共用范围查询；
查询保留跨界和重叠候选，由原有语义校验拒绝，不因索引而静默裁掉异常观测。
两份输入必须有可对应的 token、请求、rank、forward 和层调用；没有配对依据的成本保留原值。
正式窗口的 CPU 配对同时按正式请求身份选取 scheduler 工作。预热的结果处理可能延伸到正式 HTTP
窗口内，但不能因此要求它对应一个正式 forward；这些未选中的工作仍保留在 source DAG 中，不删耗时。
独立校准显式包含准备请求时，仍按完整请求集合配对，不依此过滤准备工作。
共享控制补采恢复时，轻量回放继承所选完整采集实际使用的 token bundle 和 workload 输入，
不改用后来新捕获的输出 tokens。已有轻量采集也只在 bundle 对应时复用；无关尝试仍计入累计预算。

已有修正文件使用 `cpu_service_costs: ["<cpu_service.json>"]`。
需要自动补轻量回放时，声明 `cpu_service_capture_budget` 并省略 `cpu_service_pairs` 即可：
流程为尚无 CPU 成本的已选 base 自动建立配对，TP 来自各自的实际 server command，未指定 TP 时采用框架的单 rank 默认值。
预算仍须覆盖实际请求和 token，缺少必要计时或可验证的 token 回放依据时会停止，不会改用 target 数据。
CPU 轻量回放的 dry-run 也会核对累计预算，并显示当前这一份 base 回放的启动、请求和 token 需求；
这不是整组所有 base 的预算估计，也不代表已验证实际采集能完成。缺少预算或存在未完成采集时，
入口直接显示停止原因，详细用量仍保存在 `group_summary.json` 的 `cpu_service` 中。
已有 light profile 或希望显式指定配对时使用：

~~~json
{
  "cpu_service_pairs": [
    {
      "profile_manifest": "<full-base>/profile_manifest.json",
      "light_manifest": "<light-base>/profile_manifest.json",
      "tp_size": 2
    }
  ]
}
~~~

同一 source 不可重复绑定或同时声明已有成本和重新准备。
声明 pair 后，pairs 与已有成本文件合计须覆盖所选 sources。同源修正由全部 target 共享。
显式空 `cpu_service_pairs` 表示不自动创建配对；未声明 CPU 预算时也保持不自动采集。
缺 light 时需要 CPU 回放预算，默认直接使用 base 报告记录的 token plan，不要求再制作 bundle。
若显式提供 `forced_token_bundle`，它必须与 base 的原始计划一致；生成的轻量配置仍引用 base 计划，
不再先改回占位符、再由采集入口用 bundle 替换。base 本身缺少必要计时，不能仅补 light 修复。
轻量回放保留 base 的 workload 命令原文，包括 token-plan 路径；每次预算内尝试只允许启动一次服务，
不能在一次记账中隐藏多次启动重试。配对比较忽略输出用的 `run_id` 和启动重试次数，
但仍要求实际推理配置、请求及 token 计划一致。

单独排查：

~~~bash
scripts/model.sh prepare-cpu-service \
  --profile-manifest <full-manifest> --light-manifest <light-manifest> \
  --tp-size <tp> --correct-recorder --output-dir <cpu-output>
~~~

输出 cpu_measurements.json、cpu_service.json 和 cpu_service.retained.json。
retained 报告说明不能安全扣除的 recorder 成本；缺报告不能解释成全部扣除。
Release 默认只保存保留条数、保留 CPU 时间与 recorder 校正汇总；逐条 `rows` 明细仅在
`TRACE_GRAPH_DEBUG=ON` 构建中输出。两种构建生成同一份正式 CPU 成本，Python 准备流程不依赖明细。
正式 cpu_service.json 按区间逐条写出，每条记录一行，避免为序列化再拼一份完整区间表；字段与读取合同不变。
元数据在大区间数组之前。Python 编排只读取源、配对与窗口等头部信息，不为核对来源展开几千万个区间；
实际成本仍由 C++ 完整读取并验证，按线程批量合并。写出时复用线程身份与字段结构，不改变区间数值。
校正只处理已测量区间，不清零 residual gap，也不是逐条指令的开销归因。
host、scheduler、输入处理和层等待配对直接生成统一的 CPU 测量记录；source 在配对入口绑定一次，
合并及追加 recorder 修正时共用线程区间重叠检查，避免重复扣费，不再经过字段转换中间层。
CPU 准备只构建一次 source DAG：先绑定 forward/host 的配对成本，再在同一张图上筛选尚未覆盖的 recorder 写入。
host 校正保持原来的整数预算分配顺序，得到区间后按线程批量合并，不逐条搬移已有的大型区间表。
批量合并只改变存储方式，不改变时间坐标或成本；与已绑定区间重叠仍报错。
HiCache CPU 节点索引按紧凑的时间、空点标记和节点 ID 排序，避免比较时反复访问大型 trace；相同时间下仍先放空点，再放非空叶节点。
预热的最后一次 forward 可能跨过正式窗口起点。正式请求配对按请求身份排除这类已声明的准备调用，
不把它当作缺失身份；主机方法同样按所属请求选择配对，不要求预热调用在两次运行中都跨界。
原始 DAG 仍保留窗口内的实际执行，未配对的预热成本不扣除。跨起点的 batch 包络标为 `causal_head` 上下文，
不要求窗口外的层等待具有完整节点，也不将整次预热 forward 纳入目标计算变换。
独立校准显式使用 `--include-setup-requests` 时，准备请求仍在其测量范围内。
窗口结束处，两个 rank 可能在 HTTP 端点前后分别进入同一次非阻塞 load/write 确认。
裁剪前先按 group、members 和 collective_index 闭合同轮同伴，保留所需外层调用、原生 worker 和返回边界。
这只是已启动检查的因果上下文，不延长 HTTP 端点，也不纳入后续轮询；缺失同伴或不完整调用仍由正常观察器拒绝。
窗口外的新轮询不会仅因其原生事件等待与后台事实重叠而被纳入图。HTTP 之后的事件同步须属于已开始的语义调用或已证明的同伴，
否则不保留其原生等待，避免把未纳入的下一轮检查变成窗口内写入的悬空消费者。被明确保留的写确认则仍需先替换其等待尾部、再替换写入。
起点也可能截在同一次加载 MIN 的两个 rank 入口之间。若同伴和签名完整、两侧外层调用前后均为空队列，
且每个 rank 都在其首个建模缓存输入前返回，则保留源图中的检查尾部，不重建窗口外的前缀，也不重复发布状态。
这类未变化工作单列为 `retained_empty_opening_rounds`；不满足上述条件的半轮检查仍报告不完整。
每份采集的同步事件在输入处统一转换为纳秒区间，由 forward、host、输入、scheduler 和 layer 观察共用；
各观察器不再重复解析 Chrome trace 或反复换算时间单位，仍分别检查自己的区间归属和成本扣除。
原始写入保存在 cpu_measurements.json 的 recorder_writes 中；C++ 输出扣除及保留统计，Python 不再读回成本后第二次构图。
forward、HiCache host 和 scheduler 的观测与配对分别放在 `calibration/step_timing.py`、`host_timing.py`、
`scheduler_timing.py`；host、scheduler 和输入处理共用成本记录的生成规则，层等待仍单独保留整数预算。
测量文件保留原始时间坐标、配对成本及修正依据，不再导出无消费者的 forward 墙钟差、同步耗时差和 probe 外包区间差；
这些诊断项不参与成本扣除，删除它们不表示对应开销已被消除。

### 共享控制成本

组级 `control_calibrations` 声明已有操作成本文件：prefetch_wait 按策略、write_host 按页大小和写策略索引，
load_index、load_submission、layer_wait 等按操作名声明。导出用法见[共享 CPU 操作校准](hicache_io_cost_model.md#共享-cpu-操作校准的采集开销)。
独立导出直接产生操作级测量，不携带用于生成目标结构的局部图模板；
Python 编排不按节点名称二次分解计时，预测只消费离线成本模型。
正式查询不读取 `prefetch_query_calibration`：结构模块按执行组生成本地采样、MIN 和应用结果的顺序，
再查询独立模型 `prefetch.query` / `sglang.storage_hit_min`。缺少模型报告带工作描述的成本缺口，
不要求 base 出现过查询，也不从成本文件读取通信参与者。
组流程先从已声明、同环境且有配对 CPU 修正的预取等待校准来源重新导出查询成本，不要求等待策略相同。
没有这类来源时使用共享 wait-complete 校准实验，复用匹配的原始采集或在预算内完成三阶段采集，只导出查询成本，
自动加入 `control_calibrations.prefetch_query`。也可用 `--operation prefetch_query` 手动导出并声明。
本轮导出提供带原始查询观测位置的 `operator_observations`，入库后按显式训练请求发布模型；
旧查询文档没有这些行时需要从所声明的独立 trace 重新导出，不把文档字段猜成原始测量身份。
统一成本包的四项预取查询口径各有 40 条独立观测，已用于完整 60 格预测。
数据库优先复用也已通过公开组入口验证：旧包缺项后使用库内独立数据重训，完成预测且没有新采集，
证据为 `three_stage_refactor_20261003/database_refresh_replay`。
这些证据不代表所有缺项都能自动构造实验补齐，设备补采仍受支持分支、环境和累计预算限制。
查询局部区间仍是观测墙钟包络，不能把配对文件存在解释成所有查询开销都已剥离。

显式声明与组内已完成导出合并，同一操作以显式声明为准。组输入先核对来源、环境及预取策略族，
需求扫描使用已经准入的集合，不重复读取校准文件；具体操作、资源与依赖语义由 C++ 消费端核对。
导出成功不等于全部 target 分支齐备；未做 CPU 校正的依据仍标记为 profiled 成本。

## 6. 准备流程与失败处理

正常使用时，prepare 取得或采集声明的 base，准备已选择的同源 CPU 修正，再使用适用的独立模型预测整组 target。
已有成本包时，不训练、不打开成本数据库，也不提前恢复控制或物理校准文件；
只有显式以数据库和训练请求代替成本包时，才先做离线成本准备。已有成本包仍须满足部署、来源隔离和实际操作需求；
合法分支缺少成本时，失败格 HTTP 时间为 null，需求保存在 `prediction.missing_costs`，不删操作或填零。

只有显式开启 `automatic_cost_acquisition` 并声明数据库、训练请求与累计预算，才在真实成本缺口后进入补采：

1. 先核对缺口是否有训练定义、目标工作是否提供了所需变量；缺结构或缺变量不靠采集补洞。
2. 尚未在本次准备中训练时，用已有独立数据库刷新一次成本包，排除本组 base，再重跑整组；已经训练则不重复此步。
   没有新设备采集，也不消耗采集预算。
3. 仍缺数据时，按操作需求选择一批共享实验并严格串行采集；完成后统一入库、离线训练和重新预测整组。
4. 同次准备不重复尝试同一实验。已做实验仍未解决的缺口保留并停止，不按 target 分数重训或扩大实验。

`prepare.py` 负责源准备与预测，`cost_preparation.py` 负责组级离线训练，`acquisition.py` 负责可选恢复；
首次训练与缺口恢复共用成本准备，不各自维护训练命令、排除规则和发布逻辑。
具体实验由控制、计算与物理采样模块完成。
一次恢复会话只读取和解析一次训练请求，缺口分析与后续训练使用同一组模型定义。
控制补采先生成包含实验与导出项的 `ControlCapture`；执行只消费该计划，不再次按需求匹配导出类型。
等待实验可以同时提供加载索引、提交和层等待观测，query 保持最后处理以复用同组配对。
这些匹配只描述当前采样器的覆盖，不决定 target 分支或模型适用性；未知实现不能单独触发一个假定可用的实验。
需要修改定义时重新运行 prepare；采集后的组输入和累计预算仍按实际状态更新，不冻结文件。
补采得到的成本包由整组 target 共享，不读取 target trace 或逐格误差。更新成本包后，不沿用旧包下已经完成的预测格。
离线训练可发布本次有依据的部分模型；未用模型缺数据不提前阻断执行，实际缺口继续明确报告。
所有新包写入组输出，用户提供的共享输入包不被覆盖。训练失败时停止，不能把旧文件当成本次训练成果。

### 补采选择与输入

| 需求 | 采集方式与主要边界 |
| --- | --- |
| I/O 服务 | 只测所需的 DMA、预取、已有键写入或新键写入；DMA 双向需求合并，同批服务沿用前一项的报告 |
| Prefill/Decode 与 allocator 准备 | 用 base 的合法 token 构造常驻前缀和不同长度续写；不保证覆盖所有特化、首次编译或页大小 |
| HiCache 控制 | 针对等待、写回或普通释放构造整组共享的 token 记录、完整回放和轻量回放，再做同源 CPU 校正与操作导出 |
| 锁定淘汰候选 | 测量已支持的 CPU 原语，不伪装成完整推理运行 |

计算与控制 workload 不要求先有 I/O 物理测量。控制 workload 的批大小来自当前部署的后端规则；
base 输入必须来自自身的 token 计划、已完成报告或显式 token 模板，不能借用其他 cell 或编造词表编号。
首次 token 记录不带 profiler，只提供回放输入；完整与轻量回放才构成 CPU 配对，三次运行全部计费。
等待实验可共用一次配对采集导出 load index、load submission、layer wait 和 query，不能为每个原语重复采一遍。

I/O 补采要求显式 `physical_capture.page_token_sizes`，不从 target 列表推导采样域。
已有物理测量在使用时核对几何、设备、NUMA 放置和环境。没有报告时，从 base 部署读取元数据；
仅在采样器需要文件输入时写 `platform_inputs.json`，不把该文件恢复为有效测量或校准状态。
元数据没有服务系数；采样绑核和 membind 限制只在真正需要测量时检查，不成为普通预测的前置要求。

独立采样统一用 `--base-report` 指定部署信息或已有报告：
存储命令用 `--service prefetch|existing_write|new_write`，DMA 命令可用 `--directions host_to_device|device_to_host` 选择方向。
只补所选服务，保留其他成本和来源。纯新页、纯已有页及混合批次分别要求对应依据，未用服务缺席不会强制补采。
重复测量不能填补缺失分支或缺变量；当前采样器也不覆盖任意执行路径，无法解决时保留具体缺口。

### 摘要、预算与恢复

`group_summary.json` 直接包含各目标的耗时、执行状态与限制；需要详细来源或故障日志时再按其中路径查看：

| 状态 | 含义与下一步 |
| --- | --- |
| `predicted` | 本次组预测全部执行；成本覆盖与独立精度验收仍需另外检查 |
| `preparing` / `capturing_base` / `preparing_cpu_service` / `preparing_costs` / `predicting` | 本次输入准备、base 采集、CPU 校正、独立成本准备或预测；不把上一轮成功时间当成本轮结果 |
| `planned` / `source_inputs_prepared` | dry-run 或 calibration-only 已结束，没有执行 target，也没有训练 |
| `interrupted` | 本次准备被中断，已完成采集保留；先核对本次进程已收尾，再重跑同一命令，未完成目标不作为有效预测 |
| `needs_independent_cost_model` | 采集前已发现未声明成本依据；引用成本包，或提供数据库与训练请求；首次建设走 `prepare-costs` |
| `structure_corrections_required` | 目标工作或依赖规则不完整，不能用补采代替结构实现 |
| `cost_model_definitions_required` / `cost_corrections_required` | 缺训练定义、工作变量或存在不适用的数值响应；先查看修正说明 |
| `acquisition_requires_database_and_training_request` | 已开启自动补采，但未声明数据库或离线训练请求 |
| `acquisition_incomplete` | 没有得到可用的新证据；检查 `captures` 中的预算、采样限制和失败原因 |
| `shared_training_incomplete` | 导入或训练没有完成，旧成本包不能证明本次成功 |
| `prediction_incomplete` | 本次预测未全部执行；查看实际缺口、退出状态和日志 |
| `cpu_capture_budget_exhausted` / `cpu_capture_failed` | 源 CPU 轻量回放未完成；查看 CPU 计划中的限项、尝试及日志 |

`captures` 按顺序记录本次共享补采，`operation` 标明需求；它不是第二份预算账本。
base、CPU、物理及共享 workload 的各自账本记录跨次累计用量，失败、重试和无效输出均计费，不重置历史。
预算只约束新采集：已有匹配测量可复用；某项预算被拒绝时，其他不同需求仍核对自己的限额。
只增加 profiler API 的启动/停止等待时限，不改变工作量或通道，已完成的 base/独立采集仍可复用；
旧输入及实际时限保留在账本。执行、结束条件和采集通道等变化仍需要匹配，失败采集不因此变成可用输入。
中途已经完成的测量保留并可入库，不能把整个批次宣称为全部完成。

未收尾记录需要先核对其声明的进程、容器、产物和日志，不能因记录旧就删除账本重启。
阶段状态在进入长任务前发布，但不是进程存活证明；中断后仍按下述账本和容器核对，不能只看 `preparing` 等字段。
正式准备新预测时先撤下旧预测的完成摘要，避免采集尚未完成就评分旧结果；dry-run 和 calibration-only 不撤下已有预测。
入口查询具体容器，但 running 字段不是活进程证据；容器不存在或查询失败也不能证明清理、用量结算和产物有效。
当前不承诺完全自动的中断恢复。命令失败保留原停止原因，损坏产物作为附加错误；缺少可读用量时按预留预算计费。

复用的是原始采集，不是“文件存在即成功”的派生结果。控制导出由当前实现重新生成，成功后才原子更新引用；
失败保留原文件但本次准备仍失败。不同真实完整/轻量配对分别保存，不按重复使用的实验名称覆盖。
同源轻量采集可在正式运行或 dry-run 中复用；没有可复用采集时才需要新预算。

`prepare_wall_seconds` 已包含本次源准备、采集、构模和预测，不能再叠加分项耗时。
采集墙钟预算包含容器运行、清理及产物验证。旧资产的首次采集耗用可能未知，不能据此宣称完整冷启动成本。
dry-run 不启动采集和预测；calibration-only 只准备源输入。两者都不证明 target 服务需求、成本覆盖或完整 E2E 可用。

## 7. 独立成本库、预测与结果

### 独立性能准备

这是平台维护入口，不是每次应用预测的前置步骤。应用用户通常只引用维护者交付的适用成本包及资源声明；
已有依据可以跨任务复用，新增测量和模型定义由维护者在这条独立流程内处理。
当前仍需显式声明采样方案与训练规则，不支持只填平台名就从空库自动建齐全部成本；缺规则不能靠补采替代。

平台维护者用一个入口组织独立性能主线，不需要构造 base/target 预测组：

~~~bash
scripts/model.sh prepare-costs --request <performance.json> --dry-run
scripts/model.sh prepare-costs --request <performance.json>
~~~

最小输入是数据库位置、训练声明和输出目录。有适用数据时只做离线训练；
需要采样时，在同一个请求里声明普通 profiling 配置和允许的累计开销：

~~~json
{
  "database": "<platform-library.sqlite>",
  "training": "<deployment-model-definitions.json>",
  "output_dir": "data/modeling_runs/platform_performance",
  "profile_suite": "<independent-sampling-config.json>",
  "capture_budget": {
    "wall_seconds": 1200,
    "server_starts": 2,
    "requests": 100,
    "tokens": 100000
  }
}
~~~

预算只是示例。采样配置声明模型、运行参数和独立 workload，不放 target trace 或评分输入。
入口严格串行处理配置中明确列出的实验；普通单实验即可，不要求矩阵。自动回放目前限于 SGLang 模板负载，
未知执行分支或采样器不由新成本样本自动获得支持。
采集、实际启动和失败复用现有预算/进程规则；没有 token 计划时先记录，再完成 profiler 回放。
独立采集开启 Torch shape 记录以提取实际工作变量，不改变 base 默认采集；CPU 服务必须另有同源轻量依据。
同一输出目录重复运行只复用相同输入的完成采集，不清空账本；新采样定义应保留累计用量。

已有独立 profile 使用 `profile_manifests`，已有测量行或证据使用 `observations`、`evidence` 文件。
runtime profile 测量提取目前限于 SGLang；这不限制 KTransformers 的共享构图入口，也不表示其算子测量已接通。
`cpu_service_costs`、`cpu_service_pairs`、`cpu_service_capture_budget` 与应用准备共用同一校正规则，
允许 CPU pair 显式声明 `include_setup_requests`。库维护的 `include_setup_requests` 单独选择导入的测量窗口。
`resource_environment` 是实际资源的显式声明，不能覆盖采集记录的冲突设置。
已有 `control_calibrations` 可直接准入；新增控制分支需有覆盖其工作的独立采样与测量，不从拟合缺口臆造观测。

I/O 维护可声明已有 `physical_calibration`，或用 `physical_capture` 的页域、设备、CPU 放置及物理预算安排采样。
`physical_services` 明确选择 `physical/prefetch_stages`、`physical/load`、`physical/write_device_to_host`、
`physical/write_host_to_storage_existing`、`service/write_host_to_storage_new` 中需要的服务，默认不启动物理采样。
新物理采样从本请求的独立 profile 取得部署信息，必须与训练部署一致；不读取预测组或 target。
采样器仍限于当前 Ascend/page-first/file 路径，不能把执行入口齐备称为任意平台的全部成本就绪。

默认导入去重，不提供隐式覆盖旧观测的开关。修正提取规则需要下面的显式完整采集替换动作。
输出的 `performance_summary.json` 给出当前采集、校正、导入与训练结果；`cost_model.json` 是可复用包。
缺数据库/测量、缺变量或模型缺数据均明确报告；部分包只包含有依据的响应，缺项不记零。
ready 只对应本次声明的模型，不证明所有 target 或完整 E2E 可预测。dry-run 不采集、导入或训练。
库维护不读取评分，不触发应用预测，也不覆盖请求引用的共享成本包。

### 导入与离线训练

数据库保存测量和来源，参数只保存在导出的成本包。当前 base 不自动入库，也不在线拟合。

这条性能主线可以在某次预测之前准备，后续继续积累适用的独立数据，不按 target 创建专用数据库。
同一库可以保存多个平台或模型的观测，训练时仍按部署、实现和计时口径选择；
跨模型/平台的共享须有明确适用依据，不能因操作同名就混合拟合。模型包供一组 target 使用，
新增测量不自动修改已导出的包。应用用户可以消费已有包，维护者才需要管理采集与训练定义。

显式导入及离线训练：

~~~bash
scripts/model.sh cost-db import \
  --database <cost-database.sqlite> \
  --profile-manifest <independent-profile>/profile_manifest.json \
  --cpu-service-cost <same-capture-cpu-service.json> \
  --resource-environment <resources.json> \
  --output-dir <import-output>

scripts/model.sh cost-db train \
  --database <cost-database.sqlite> \
  --request <training-request.json> \
  --output <cost-package.json>
~~~

没有同源 CPU 校正时省略相应参数，CPU 样本仍保留 profiled 口径，不能作为正常服务模型的依据。
物理 I/O 或已导出的控制/原语测量通过 `--evidence` 导入，不能把其文档路径当成另一份 runtime profile。
物理声明提供几何、测量条件和原始文件来源。导入读取这些明确来源，保存存储工作点、DMA 设备时间及
预取阶段测量；历史报告中的 `service_models` 不入库，也不从系数反造样本。原始文件缺失就停止。
物理采集报告只发布来源和条件，离线成本层才从库内测量拟合服务响应。训练不回读外部测量文件。
导入和训练不启动设备采集，不读取评分结果；所选部署和来源必须独立于本轮 base 及评分采集。
部署比较排除纯观测开关，但 CPU 亲和性、设备及实际执行开关仍参与适用性判断。
性能库的只读选样与预测共用部署语义：诊断接口的访问凭据不区分性能平台，不要求复制到新的训练声明。
原始部署记录仍保留，模型、TP、后端及资源条件不因此放宽；没有匹配事实时依旧报告缺测。

重复导入按原采集与测量位置去重，不合并不同真实采集中的同一工作点。
新 runtime/control 采集默认以原 manifest 路径建立身份，重导入沿用库中已登记的身份；
suite 内的局部 run 名可以在不同采集中重复，不能据此合并。重命名副本通过 `--capture-aliases` 明确原身份。
普通 runtime 入库不要求测到完整 HiCache 写入，只导入实际可归属的操作；显式写入/释放导出仍要求相应测量。
修正提取规则时，显式使用 `--replace-captures` 重新导入该采集的完整派生观测：
在同一事务中替换，失败回滚，保留来源别名及原始 trace。它不是追加某个控制原语的开关；
只提交部分数据会移除该采集未提交的旧派生样本。默认导入和自动补库不隐式替换。
替换后须显式重训，已有成本包不会自动改变。

### 性能库的事实归属

数据库长期维护独立测量，不随某个 base 或 target 新建一套。部署、采集身份和来源分别保存；
操作测量只记录原位置、实现、工作变量、计时口径和微秒数，通过引用关联其部署与 manifest。
控制导出和 CPU 原语中的操作行不再作为整份 JSON 重复存储；库内额外证据只保存 I/O 训练需要的
运行时服务测量及物理依据。图模板、诊断布局和临时报告字段不决定一次测量是否重复。

普通重导入不能改写同一采集的环境；同一位置/口径的真实工作或耗时变化仍是冲突。
需要更正历史环境声明时，可通过 `--replace-captures` 完整替换库内派生观测和声明，
但同一批输入仍须一致。这不修改原始采集，不等于实测证明，也不会更新已发布成本包。
别名和完整/轻量依赖随事务保存，训练留出不打开历史 trace。原始测量文件不由入库流程删除。
`import_summary.json` 中 `operators` 计操作行，`evidence` 只计额外的 I/O 聚合，不计控制报告副本。

之前使用整段部署和来源字符串的数据库，需要显式转换一次；普通导入或训练不暗中转换旧库：

~~~bash
scripts/model.sh cost-db migrate \
  --database <previous-database.sqlite> \
  --output <new-database.sqlite> \
  --summary <migration.json>
~~~

转换只读旧库，完成后才发布新文件；输出已存在就报错，绝不覆盖。更改组配置中的 `cost_database`
引用后，导入、训练与覆盖查看只使用当前表示。已发布的成本包仍可直接预测，不必为转换重训。
转换不读原 trace、不重新提取成本、不证明模型覆盖或 E2E 精度，也不恢复已经清理的原始文件。
物理证据例外：显式转换会读取声明的原始物理文件，将旧参数表示改为测量事实；文件缺失时整个转换失败，旧库不变。
已经完成表结构转换、但物理证据仍保存参数的库，使用 `cost-db import --evidence ... --replace-captures`
重新导入其完整物理声明。普通训练不兼容读取旧物理参数；已发布成本包仍可直接消费，重导入不会自动改包。
同一库的维护与训练顺序执行，训练期间不改写所选数据库；当前分操作族读取不提供跨进程在线更新协议。

### 训练请求与缺口

训练请求声明 `deployment`、`models` 和 `excluded_captures`/`excluded_manifests`。
仅维护 I/O 时可省略 `models`，声明 `io_evidence` 选择一个物理测量集合；不必附带无关算子。
同一集合可以引用多份独立原始测量。未测服务和不能辨识的参数列为缺口，不因采集成功就算模型就绪。
留出覆盖本轮全部 base、结构评价、正常 HTTP 评价及相应轻量配对，不能仅排除完整侧。

共同的公式、变量和范围规则只需声明一次：`operation` 和 `timing_scope` 可分别写一个名字或明确的名字列表。
列表按操作与口径的组合展开；不是按前缀匹配，不从已有 trace 自动猜模型形式。
每个组合仍独立选样本、训练和记录适用域，不共享系数。不同规则分开声明，不能用列表覆盖差异。
例如 Decode 中 K/V 写入采用相同的常数模型和工作范围，但 CPU 服务与残余区间仍分别建模时，可以写成：

~~~json
{
  "operation": ["decode.attention.key_write", "decode.attention.value_write"],
  "implementation": "ascend.scatter_nd_update_",
  "timing_scope": ["cpu_service", "residual_interval"],
  "features": [],
  "coverage_features": ["elements", "locations"],
  "form": "constant"
}
~~~

以上声明对应四个独立响应，不是把四种成本合成一项；实际操作和口径必须有相应观测。
只有底层工作确实等价时，才另用 `observation_operations` 显式共享测量。
离线训练和共享补采使用同一套声明展开，I/O-only 请求不必补一个空的算子清单。

| 模型字段 | 含义 |
| --- | --- |
| `operation`、`implementation`、`timing_scope` | 选定操作、实现和口径；操作/口径可用明确列表，同一展开后的三元组只声明一个模型 |
| `form`、`features` | 常数、线性或单变量分段线性，以及实际进入公式的变量 |
| `coverage_features` | 只限定已测工作条件，不增加系数；默认空 |
| `observation_operations` | 显式选择有机制依据的等价观测名；默认只使用自身操作，不改写或复制原始样本 |
| `allow_extrapolation`、`extrapolation_features` | 明确允许的外推；后者只放开列出的坐标，分段线性不允许外推 |

`implementation="*"` 仅为所请求口径下已有样本的实现分别训练，不合并不同后端。
其他实现的缺失口径在 `wildcard_unmeasured_implementations` 中说明；查询这些口径时仍会失败。
若要求某个实现必须齐备，应使用具体名称。CPU、worker、派发、残余和设备口径也不能互作替代。

训练按声明的操作族分批读取，保留其他口径以区分“缺操作”和“缺口径”；不会为小模型集合
加载整个平台的全部观测。响应按声明展开顺序输出：先操作、再口径；同采集同工作点的聚合与拟合方式不变。
一次非常大的操作族仍需要对应的内存，这不是恒定内存训练或任意规模的保证。

缺数据时，训练分别报告 `missing_operation`、`missing_timing_scope`、`missing_features`，
并列出已有口径。不自动删变量、换公式或补零；工作点不足、秩亏和无效响应也需离线审查。
自动补采使用同一模型定义：缺声明、目标工作缺变量或已有响应无效时，先报告修正需求，
不为它们启动重复采集。具体恢复规则见第 6 节。

`cost-db inspect --database <cost-database.sqlite> --output <coverage.json>` 展示各口径的样本、
采集和工作点数量，以及变量范围、出现次数与单一取值。样本多不等于变量充分变化。
`training_fit` 是同次采集/同工作点中位数上的训练内误差，附工作点数、参数数和剩余自由度；
观测总量为零时 WAPE 为 null。零残差或范围覆盖不证明未见组合准确，不触发自动换公式。

`omitted_observed_features` 列出未用于拟合的变量，`unchecked_observed_features` 才是连范围也未声明的变量；
`collapsed_distinct_work_groups` 记录因省略变量而合并的工作组。这些是模型定义审查信息，不是自动增参指令。
变量、分段口径和典型模型的机制依据统一见[成本模型](hicache_io_cost_model.md)，本页不再逐算子重复维护公式。

### I/O 与来源

`io_evidence` 选择适用的独立物理证据；I/O 训练也使用同部署、未被留出的历史 runtime 观测。
`control_calibrations` 声明可显式入库的测量，预测不直接读它们或按邻近规模挑选整段模板。
所有来源与完整/轻量依赖随成本包保存，参数只由离线训练产生。

`training_sources`/`training_source_manifests` 保守记录算子训练及 I/O 选择接触的来源。
数据库采集身份不等于 manifest 中可重复的 suite-local `run_id`。自动留出 base 和预测准入按实际 manifest 来源关系判断，
已登记的原采集别名及完整/轻量依赖也随包传递；不因两个独立采集的 run 名称相同而排除其中一个。
I/O 中失败的拟合也可能影响物理公式选择，不能从隔离名单删除；
`io_training.captures` 与方向来源记录实际贡献者，`considered_runtime_captures` 和
`considered_source_manifests` 记录被考察数据。两端点估计不把中间点当成系数来源，
被拒绝页宽也不能标为已测覆盖。

`io_training.measurement_support` 说明实际调用字节及页宽范围，没有 runtime 依据时保持空，
不能用默认倍率伪造运行覆盖。`measurement_count` 是发布操作模型的原始测量数，
`candidate_measurement_count` 是留出后的候选数，都不是成本覆盖率。
包可以只含有依据的服务；未使用服务缺席不强制补采，实际合法操作缺成本仍失败并报告需求。

### 单独预测

~~~bash
scripts/model.sh predict \
  --source-manifest <base/profile_manifest.json> \
  --target-config <target.json> \
  --cost-model <cost-package.json> \
  --resource-environment <resources.json> \
  --cpu-service-cost <optional-same-source-cpu_service.json> \
  --output-dir <prediction-output>
~~~

没有 CPU 修正时省略该参数。source、target 和 CPU 参数可重复传入，任务由所选组合决定；
验证代表格时只传对应输入。默认失败后不启动后续任务，已启动任务正常收尾；
`--continue-on-error` 继续其余目标。预测不因旧报告存在跳过本次执行。

| 文件/字段 | 用途 |
| --- | --- |
| `group_summary.json` | 准备状态、停止原因、实际成本包及逐目标结果；预算与诊断路径另附 |
| `cost-package.json` | 离线参数、训练来源、缺失模型及部署范围 |
| `import_summary.json` | 本次导入和去重结果 |
| `preflight_summary.json` | source trace/事实错误 |
| `workflow_summary.json → prediction.cells` | 输入身份、执行状态、完整 HTTP 时间和成本局限 |
| `model_runs/<model_run_id>/run_summary.json` | C++ 执行结果 |
| `model_runs/<model_run_id>/runner_config.json` | 单格复现输入 |

组摘要的 `prediction.cells` 直接保存逐目标耗时、状态、成本覆盖及保留近似；
完整查询来源和复现身份在 `prediction_summary` 指向的 workflow 摘要中，不复制完整诊断。
每格保存 source manifest、source 配置、target 配置与字段、workload 和 `model_run_id`；
不额外维护一份任务索引，也不将 Python 身份回写 C++ 摘要。
开始时保存未完成任务，每完成一格更新同一摘要；中断后可以查看已完成结果，但它不是预测缓存。
历史静态结果的旧身份仍由诊断读取，不批量迁移历史资产。

完整 HTTP 使用 `http_client.e2e_us`；`simulated_e2e_us` 或 `full_graph_e2e_us` 可能包含请求结束后的后台工作。
失败、dry-run 和未执行格为 null，损坏摘要直接报错，不当成空对象或零耗时。
`model_run_error_count` 是命令失败数，`prediction.completed_count` 是完整执行数，二者不同。
`EXECUTED` 不证明结构一致或精度达标；`cost_coverage=partial` 说明限制，不是逐操作覆盖率。
`source_observations_status=not_requested` 仅表示没有额外导出报告，不表示没有使用源依据。

终端逐 target 显示预测秒数；无结果显示 `not predicted` 和原因。来源、外推和保留等待仍需查看摘要。
结果中的 `next_action` 区分结构规则、独立成本及普通执行失败，`failure_log` 指向本次失败日志。
`configuration_comparison` 按同一 source/workload 排列已执行目标；目标未全部完成时不声称找到了最快配置。
相对 base 的收益仅在有 self 预测时提供，并注明是预测参照；没有它时为 null，不拿带 profiler 的 source 墙钟冒充正常实测。
该排序不是已验证推荐或置信区间，完整限制仍由 cells 和独立评分说明。
直接预测只比较用户给定的候选；下述搜索入口可从有限参数列表生成候选，不实现连续或在线优化。
更换模型、TP、后端或资源环境前，要重新核对执行规则和独立性能依据；同一模型的成本外推也不等于已测工作点。
是否适合扩大搜索，还取决于可区分收益、预测本身的耗时和首次准备成本，不能只看 E2E 误差。
默认 `--diagnostics off` 使用 Release，只保留 compact summary、失败日志和复现输入；
`--diagnostics full` 使用 validation，增加明细和成功日志，不改变模型。
当前执行式结果没有完整组件归属及 target 操作映射，不能用历史静态 oracle 回放证明其整图正确。

### 用结果选择配置，以及何时扩大搜索

先确认候选都执行成功，并查看成本外推、保留等待和不支持分支；再比较完整 HTTP 秒数。
应用输出的排序只是预测，不带已验证的置信度。很小的时间差不能直接当成可靠收益，
更不能为得到一个“最优配置”忽略失败项或缺成本的候选。

实验中的 `configuration_selection` 按预先约定的正常重复测量判断：

| 状态 | 应怎样使用 |
| --- | --- |
| `INSUFFICIENT_SEPARATION` | 当前样本不能区分快慢；保留 base 或给出候选组，不强排第一名 |
| `SUPPORTED_ON_MEASURED_PAIRS` | 只支持已可区分配置对的顺序；重叠组内部仍不能下结论 |
| `INCORRECT_ORDER` / `UNRESOLVED_ORDER` | 暂不扩大搜索，先查工作/依赖、成本范围与剩余近似，不根据分数自动调参 |

后续搜索先限于已支持的 HiCache 页大小、容量、写入和预取策略等显式候选，
模型、TP、后端、资源和给定工作量不变；配置合法也不意味着每个工作点都有适用成本。
当前目标是完整 HTTP 正式窗口时间，不是并发吞吐、任意在线输出或其他延迟指标。
有限候选生成与预算搜索由下述入口完成；它不等于已经找到真实运行的全局最优配置。

扩大候选集合前还应比较采集、CPU 准备、预测墙钟和少量正常实测的成本。
本次准备时间与累计账本有重叠，不能相加；性能库首次建设成本未知时也不能记零。
部署前可独立确认少量代表或优选候选，确认结果只用于评价，不回写本次模型；
若修改了规则或参数，再用新的未见输入验证，不把原验证集继续称为首次泛化。

### 配置搜索

搜索与准确性实验是同一预测模型的两个调用方。用户给出固定场景、完整 target 模板、有限取值和预算，
搜索层负责选点和排序；模型仍只负责根据准备好的输入计算目标操作、成本和完整 HTTP 时间。

正式例子为 [search_example.json](../configs/modeling/search_example.json)。把 `scenario` 换成本次 Base 准备声明，
可以引用已有 manifest，也可以通过 `profile_suite` 和原有预算首次采集；无需填写 `target_configs`。
再选定其中一个 workload，修改模板、变量列表及输出位置：

~~~bash
scripts/model.sh search --request <search.json> --dry-run
scripts/model.sh search --request <search.json>
scripts/model.sh search --request <search.json> --resume
~~~

`--dry-run` 仅写 `search_plan.json`：不采集、不训练、不构图、不预测，也不覆盖已完成的搜索摘要。
计划能说明已知缺口，不能预先保证所有目标操作都具有成本。当前首轮 HiCache 搜索使用 SGLang，
保持模型、TP、后端、资源放置和请求/token 序列不变，不把 KTransformers 当作支持 HiCache。

**输入怎么写。** `space` 接受 `page_size`、`l1_capacity_pages`、`l2_capacity_pages`、`write_policy`、
`prefetch_policy` 和 `prefetch_threshold_pages` 的有限列表。页大小单位为 tokens/page，其余三个容量/门槛值用页。
也可分别使用 `l1_capacity_tokens`、`l2_capacity_tokens`、`prefetch_threshold_tokens`，与对应 pages 字段互斥；
token 值须能被每个所选页宽整除，入口逐页宽转换，使变更页大小时保持 token 容量和准入门槛。
不同时把 host 比率作为独立变量。host 页数必须大于设备页数，这是当前支持合同，
不是对实际内存足够的保证。预取容量上限由真实的 host/device 容量差派生，不是自由参数。
所有未搜索值来自完整模板；Base 参照也需有可解释的实际页容量与门槛。
资源额度与源 Base 不同时，可用 `reference_config` 引用空间内的同资源参照 target。
它与 self 一样在搜索预算内先行预测；摘要的 `same_resource_reference` 和对应缩短率与 source Base 单列，
避免把增加资源的收益全算成策略收益。

`search.initial_indices` 可预先指定空间内的固定策略索引：在 Base 和同资源参照之后优先预测，
计入同一预测预算，不读取既有目标评分。成功预测的指定项也进入自动正常筛选；
空间外的源 Base 另做一次正常采集作为评分基准，不参与部署候选选择，仍保留其 self 预测。研究实验用这一项纳入开发阶段已选定的固定策略。

timeout 的三个秒单位参数只在 `prefetch_policy=timeout` 分支展开。例如：

~~~json
{
  "prefetch_policy": ["wait_complete", "timeout"],
  "timeout": {
    "prefetch_timeout_base_sec": [0.1, 1.0],
    "prefetch_timeout_per_ki_token_sec": [0.0],
    "prefetch_timeout_max_sec": [2.0]
  }
}
~~~

在当前建模的 HiRadixCache 中，这个字段组不会为 wait-complete 生成两个相同执行点。
需要保留参数的联合关系时，可将 `timeout` 写为 `{"profiles":[{...},{...}]}`，每项显式给出上例的三个字段，
不与独立参数列表混用。恒定等待及达到上限后完全等价的超时函数只生成一个点；实际启动值仍使用所声明的一组参数。
重复取值按实际配置去重，不按名称、文件或预测时间去重；同一语义也用于未变成本迁移和正常配置核对。
设备/host 启动参数经同一个 SGLang 配置解释器换算和取整，结果同时保存完整 `hicache` 值、`server_flags`，
以及提供其余固定运行参数的 `unchanged_runtime_from`；不要把它当作脱离原场景的部署保证。

`objective` 默认形态为 `{"metric":"http_e2e_sec","workload":"..."}`。需要组合负载时，
改为 `{"metric":"http_e2e_sec","weights":{"workload_a":1,"workload_b":2}}`；权重在入口归一，
每个正权重 workload 必须有对应 Base，全部完成后才能给配置总分。失败负载不从权重中删掉。
完整 HTTP 包括 gap、未归属成本和 Prefill/Decode，但保留源等待等现行近似，不是这些部分都已准确建模。

**怎样选点。** 空间能在预算内算完时用 `enumerate`；否则用 `stratified_random` 并声明整数 `seed`。
它先按写策略、预取策略与页大小分层，每层抽一点，再按剩余组合数分配；预算不够覆盖分支时明确列出未覆盖项。
`random` 只作为同口径的均匀随机比较参照。
`local` 先分层覆盖，再根据已有模型预测，选至多四种不同机制的较优配置，各提出至多六个合法邻域点，
同时保留随机探索；页宽/门槛、写入/停止策略可以联合改变。它不读取正常实测或重新拟合成本。
`coordinate` 使用相同的初始覆盖、预算和随机探索，只关闭邻域中的联合改变，作为单变量改进对照。
自适应批次默认覆盖至少 32 个配置，分支更多时扩大到分支数；多负载按实际配置×负载次数计费。
每个批次仍使用现有预测入口，源图准备及失败均累计，不能宣称复用了常驻源图。
静态随机流的前缀保持稳定；自适应流受实际批次边界影响，恢复会保留已有结果，但不承诺与一次更大预算调用的后续序列相同。
这些方法共用一套空间和预测路径，不引入代理模型、网络服务或另一份缓存仿真。
候选按需生成，不为抽少量点先物化整个笛卡尔积。

**预算和恢复。** `search.max_predictions` 计算配置×workload 的实际尝试数，包括失败和重试；
self 参照另计，并在空间包含相同执行点时复用一次。每个多负载候选先预留其全部必需名额。
`max_wall_sec` 是累计搜索墙钟，包含源图构建和结果整理；到期停止实际调用并回收其专属容器，清理会有少量额外墙钟。
准备采集/训练、搜索、正常确认分别计时，完整等待时间另记，不把已有性能库的历史取得成本记成零。
没有当前报告的启动失败保守计费；有正式开始记录时，未启动项不当作已完成或耗时为零。

`--resume` 是用户明确确认“输入、代码和成本仍是同一依据”的动作，不是文件摘要证明。
只补未完成单元，保留失败目录与累计消耗；预算可提高，不清空历史。
改过规则、workload、成本包或训练依据后用新输出目录，不能沿用旧成功值。
上次进程突然消失且记录未结算时，先核对进程与容器，不凭旧状态文件自动重启。
搜索不在运行中逐候选补库或换参数；缺口留给独立性能维护，新依据对应一次新比较。

默认产物为 `search_summary.json`、输入及逐批正式预测报告。摘要包含合法空间与拒绝原因、完整候选、
已算/缺成本/结构缺口、未选择点和未覆盖分支，以及预测排名和全部并列最优。
`global_model_optimum_proven` 仅在整个合法空间都有完整预测时成立，它仍不是实际运行的全局最优。
`completed_source_preparations` 和对应墙钟只累计已完成源准备；中断部分包含在总搜索墙钟内，不伪造精确分项。

**可选正常确认。** 默认 `confirmation.enabled=false`，不读取 target 实测。
只声明 `confirmation.enabled=true` 和独立 `budget`（wall_seconds/server_starts/requests/tokens）时，
流程从 Base 配置自动生成正常采集输入，不要求用户逐候选编写 suite。
先按模型选出同资源参照、排名代表及机制不同的候选；源 Base 在空间外时只作为评分锚点，不参与候选胜者选择。
`screening_repetitions` 默认 1，试跑后确定候选，再用 `repetitions`（默认 5）次新样本独立确认候选和同资源参照。
两阶段共用累计预算，筛选样本不混入最终均值，预测排名不因实测而改写；观察范围仍重叠时不确认收益。
样本范围不是置信区间，没有收益时不强制推荐新配置。未配置同资源参照且 source 不在空间内时报告 `REFERENCE_REQUIRED`。
失败候选不赋零耗时；自动确认保留失败、停止该配置剩余试跑，并继续其余配置。最终候选失败时仍完成参照的新样本，不推荐该候选；参照或源评分基准失败则报告测量未完成。已记录的同阶段失败不会因 resume 静默重试，筛选样本仍不混入目标最终均值。
Ascend 自动确认在每次实际启动采集前等待其他 NPU 应用退出；已完成的相同采集直接复用，不等待设备。等待单列到 `resource_wait.json`，不消耗采集运行预算。
这是启动前的占用检查，不能保证整批期间独占设备；需要持续独占时仍应由外部调度器预留资源。

也可显式提供 `profile_suites` 和独立 `budget` 采集声明的对照，或用已有正常测量的 `target_profile_manifests` 只评分。
正常 suite 仍是公开 profiling 输入，须关闭 profiling、通道及计时开关，
标签和实际参数与生成的候选对应；重复次数在 suite 中预先声明。确认使用选定 Base 的实际 token 回放，不重新记录目标 tokens。
显式输入时，搜索结束后先写选择集合：Base、同资源参照、`top_count` 个排名代表及一个竞争候选，再打开正常输入或采集。
优先选一个预测分数不同的竞争候选；并列预测仍保留，确认某个代表不等于确认整个并列组。
只采声明集合、保留失败和慢样本，评分走公开 `evaluate`，不回写成本或改变预测排名。
正常资源/模型条件不同会拒绝比较，实测排序不符也如实保留，不据此偷偷重新搜索或拟合。
单次观测可以检查流程，不能据此确认稳定收益；样本数、范围和精度 gate 仍须单独阅读。

代码从 `applications/search/request.py`、`space.py`、`algorithms.py`、`workflow.py` 顺序阅读；
结果整理在 `results.py`。只有显式正常确认时，CLI 才进入独立的 `confirmation.py`；搜索引擎不导入 evaluator。

## 8. 独立评分与历史回放

结构明细的依赖保存为稀疏图：`precedence_identities` 对应邻接表 `precedence_successors` 的前若干个节点，
其余节点只表示共享的中间依赖分叉，不是新增模型操作，也没有跨 trace 身份。
不归属具体操作的单出口链可以收缩，分叉则保留，避免后台队列展开成上亿条操作对。
评分按语义身份匹配端点，再分块比较它们之间是否存在路径；未匹配操作仍可以作为路径中间节点。
默认只累计差异数量，显式 full 诊断才列出差异端点。数量指全部可达的匹配操作对，
不再是旧报告中的局部关系证据条数；关系是否一致的判断不因冗余边而改变。
这只改变报告表达及计算方法，不修改模型 DAG，不证明中间操作身份、物理资源或未覆盖 CPU/control 依赖正确。

结构提取和逐格比较都遵守 `--model-run-jobs` 的上限，默认仍为一。前者调度独立的 C++ 构图，
后者使用进程池处理 CPU 密集的可达关系比较；进程间只传文件路径和紧凑结果，不传整份图。
并行度也决定同时加载的大图数量，资源不足时应降低该参数；这不改变串行 profiling 的要求。

~~~bash
scripts/model.sh evaluate \
  --prediction-dir <completed-predictions> \
  --profile-run-dir <target-profile-suite> \
  --output-dir <separate-score-output>
~~~

可重复传入 prediction-dir 评分多个 base，也可用 target-profile-manifest 精确指定 target。
只重跑缺成本或执行失败格后，用 `--retry-prediction-dir` 显式提供复验输出。
评分按同一 source manifest、运行身份及目标配置替换原未完成格，保留原失败目录与原因；
失败的复验仍占同一个格子，不缩小分母。已完成格不能被替换，也不从多个预测里挑误差更小的一次。
这只选择已执行结果，不运行预测或修改成本；仍须等全部选定格完成后才打开 target 输入。
对执行式预测评估正常运行耗时，追加 `--normal-http`，用重复的 `--target-profile-manifest`
传入关闭 profiling、channels 和 TIMING 探针的正常运行。需同时提供各 base/workload 的正常测量，
作为“不改变 base 耗时”的比较基线；不能用带采集的 base 墙钟代替。
计时开关显式设为 `"0"` 属于关闭，不因字段名包含 `TIMING` 被拒绝；计时输出路径和启用的开关仍不能用于正常真值。
同一配置与 workload 的重复运行按正式窗口耗时取算术平均，结果保留报告路径、样本数和最小/最大值。
评分核对请求顺序、失败请求及已报告的 token 回放失败，不排除 gap、未归属成本或计算阶段。
不加该参数时仍使用单份 target profile，重复身份会报歧义；历史静态 oracle 诊断保持此路径。
所有选中预测完成后才读取 target；输出必须与预测目录分离。
评分不采集、不重预测、不改模型，也不触发 refit。
可重复使用 `--target-config <label>` 显式选择确认子集；默认仍评分目录中的全部格，
选中格未完成就拒绝读真值，不因其他配置失败而把它们混作性能较差的候选。

独立评分增加 `configuration_selection`：按 source/workload 比较候选排序和选择损失。
只有正常样本范围不重叠的配置对才计为可区分；重叠表示样本尚不能分辨，不代表等效或统计置信区间。
报告分别列出可区分和无法区分的配置对，后者不强行判断预测顺序正确或错误。
报告倒序、预测并列与推荐候选相对实测最低均值的损失，预测并列时保留全部候选，不事后选择最好的一项。
不同 base 可以复用相同真值，排序对的数量不是独立实验数量；排序结论不修改原有完整 HTTP 门槛。

完整 E2E 对比预测 HTTP 时间与 target 正式 HTTP 窗口，不排除任何成本，
不以 target DAG 回放或预测图后台终点为真值。报告总面板、逐 base、self 和 base-wall 参照；
同时按 workload 和 target 配置汇总，并列出最大 APE 与有符号偏差（预测减实测，正数表示高估）。
加权有符号偏差用于判断整体偏高或偏低，不能代替不允许正负抵消的 WAPE；
缺失格不从验收中删除。具体阈值及当前成绩统一见[验证说明](validation/hicache_validation.md)。

`by_cost_query_domain` 另按独立操作查询分组：查询工作点已测、各变量在范围内但组合未见，或存在域外查询。
同一格同时出现后两种情况时归入域外组；未记录查询与明确没有查询分别列出，不当作已有覆盖。
这只分类该查询报告，不包含 I/O 载荷模型、成本迁移或保留 gap；“各变量在范围内”不等于联合工作点已测，
任何分组都不是全图成本覆盖率或可信度证明。分组只帮助说明结果范围，不改变精度门槛。

`gate_summary.json.costs` 分开记录运行开销：`recorded.current_invocation` 是本次准备和最后一次预测尝试的墙钟，
`recorded.cumulative_acquisitions` 直接引用原有采集账本的累计用量，包括已记账的失败与重试。
本次准备可能已经包含其中的采集时间，不能把两部分相加；没有账本或值为 null 表示未记录，不表示零成本。
早期 CPU 离线处理、复用 profile 和性能库的首次取得成本未齐时，仍为 `INCOMPLETE_COST_EVIDENCE`，不声称首次使用更便宜。
这里不创建另一份用量账本，也不影响性能评分或触发新采集。

普通评分不再生成历史逐组件结构、phase 成本、delta 和去 gap 汇总，也不为这些报告构建 target DAG。
`gate_summary.json` 的 `acceptance_scope=full_http_formal_window`；PASS 只表示完整 HTTP 与 base-wall
门槛通过，不证明成本覆盖完整或各组件准确。成本覆盖限制继续保留在逐格执行结果中。
历史细分报告及其数据不删除；不把停止输出的指标记为零或视为通过。

### 显式静态成本回放

静态成本回放仅用于诊断。在 validation runner 的 `cpp_trace_graph` 中设置
`hicache_static_replay: true`，可以先用当前规则生成不注入 target 成本的静态对照；
Release 不接受该选项，正常 HiCache 预测仍采用执行式路径。随后成本回放共用正式 runner 的
manifest、窗口和源 CPU 校正，不能漏掉 CPU 校正后再声称“只替换了目标成本”。
这不保证旧静态资产相容，也不把静态诊断当作当前执行式预测的精度证据。
静态标量控制成本须显式提供，不能直接拿执行式模型的细粒度控制文件替代。
目前，包含无源坐标的重建 gap 的静态图不能再叠加源 CPU 校正；这种输入会报错。
不带 CPU 校正的静态诊断必须从一开始就使用同一未校正对照，不能中途丢掉校正项以通过恒等性检查，
也不能将其耗时当作正常 HTTP 预测。
显式 `model_summary.json` 中的 `hicache_dag_patch.target_effects` 保留严格配对所需的目标工作量、
操作身份和资源顺序，直接来自模型计划。它不是重新执行一套缓存逻辑，也不恢复已删的细分评分表。
其中操作数量指实际传输服务次数；零载荷终止控制仍在 I/O 成本记录中单独计费。

相容的静态预测资产可显式使用 `--oracle-cost-replay`，前提是已有完整操作映射和诊断数据。
先验证相同预测成本回放，再执行五种真实 target 成本替换；结果只用于诊断，不计入预测精度或回写参数。
`--oracle-max-runs 1` 表示每个 base 选一个 cell，每格包含一次基准和五种成本变体。
只选择结构严格匹配的格子；没有合适输入时报告 `NOT_RUN` 及排除原因，不填零、不影响已完成的独立评分。
现行执行式预测缺少这套完整映射，不能仅开启 diagnostics full 就获得静态回放能力。

诊断摘要保留必要的目标操作计划、patch 成本、source attribution、图规模、ownership 和失败原因。
同成本恒等性比较计时、图规模及归属汇总，不声称比较逐段关键路径或证明完整 target 图同构。
保留去 gap 的诊断时长，不再生成逐家族/逐请求分段、分项 WAPE/P90 或另一套总 gate。
这不改变正式预测对 residual gap 的保留。

09-12 旧聚合 Prefetch 参数不被当前分阶段解析器接受；旧结果只作历史证据，不恢复旧规则或转换参数冒充复现。
09-27 已在相容的 C3→C5/W1 输入完成严格配对及真实成本回放，具体证据与失败范围见
[验证说明](validation/hicache_validation.md#现行静态回放的已验证范围)，不能据此承诺所有静态输入都可用。

## 9. 开发边界与实现位置

正式预测从 `cli/module_pipeline.cpp` 进入 `runtime::execute_hicache_window`。
下面按三阶段解释合同；新增能力应先确定在哪个阶段缺少依据，不要同时往所有层添加分支。

### 新增一个操作或执行分支

1. 先确认需要哪些事实。已有 trace 包络、张量元数据、提交关联或框架部署声明足够时不加探针；
   确实缺少输入，再在采集适配中补轻量观测，并由 source DAG 层解释，不能按最近时间或名字猜语义。
2. 在 `structure/` 或 HiCache 状态模型中定义执行条件、工作量和依赖。
   用已有 `TensorOperation`、事件及资源角色生成操作；新结构不要求 base 曾执行过同一分支，
   也不能由有没有成本样本来决定是否合法。
3. 对变化操作声明成本请求。变量、实现和计时口径由工作决定，模型形式、测量来源与适用范围由
   `modeling/cost/` 维护；C++ `cost/` 查询离线包。新的采集只是补成本，不替代第二步的执行规则。
4. 由运行时连接资源、沿唯一 DAG 时钟展开并绑定成本。已有工作只有语义确实未变时才迁移；
   缺规则报告结构限制，缺测量报告成本缺口，不能退回整段缩放或填零。
5. 扩展实验报告的语义对应与覆盖说明，再用公开入口的真实输入验证。
   源回放、目标结构与完整 HTTP 各自回答不同问题；评分不能回写结构或模型。

沿职责修改以下合同，不必同步维护一套逐算子工作流：

| 衔接 | 合同与实现位置 |
| --- | --- |
| 源调用 → 目标工作 | `source_dag/phase_calls` 建立调用与 `PhaseCallPosition`；`structure/hicache/phase` 规划计算和通信。身份属于源图，跨 trace 使用逻辑请求，不比较局部编号 |
| 结构 → 成本 | `TensorOperation` 表达工作和前驱；生成器声明 `OperationCostRequest` 与 `HostOperationSlots`。计算、分配、释放、传输及写确认共用 `bind_host_operations`，不另建成本操作清单 |
| 成本 → 数值绑定 | C++ `cost/host_operation` 一次估计程序的全部口径，缺口一起报告；运行时只填槽位。多启动各自计费，CPU-only worker 不虚构设备或启动/返回成本 |
| 独立输入 → 预测 | Python `TargetHiCacheConfig` 与 `HiCacheCostModels` 分别准入策略和成本包；包共同持有响应、部署、来源，预测不读数据库或训练 trace |
| 测量 → 数据库 | `ControlMeasurement` 保存原采集与选择；`cost/package` 准入和分组，`CostDatabase` 原子去重与持久化。CLI 只选输入输出，不实现提取或拟合 |
| 程序 → 执行 | `HiCacheHostExpansion` 保存局部节点、worker、流与等待；`FutureDag` 沿同一时钟展开。Python `model_executor` 只安排任务，不变换 DAG |

源成本提取共用 `source_dag/tensor_cost`，各族提供自己的程序、对应和冷启动限制。
找到算子不等于获得有效计时；只读导出共用索引，需要切分的导出用私有图并重建索引。
性能导入只导出实际操作的工作变量、计时和 I/O 依据，不另建无人使用的 Torch 几何目录。
通信描述在源观测边界准入为调用与参与者工作，执行与测量共用它；不经诊断 JSON 还原调用。
结构报告比较实际工作、独立观测及必要依赖，不再重复生成 Attention/allocator 的假设程序或原始调用清单。
HiCache replay 保存缓存状态及请求/轮次观测，不持有算子调用目录。纯 I/O 曲线数学由训练和目标投影共用，
不反向依赖预测编排。trace 坐标保留纳秒精度，不能与校正后的 service 或预测时钟混用。

### 三个阶段怎样合作

| 阶段 | 负责的事情 | 不能做的事情 |
| --- | --- | --- |
| 原始 DAG | 合并源 trace，保留原观测、请求身份、资源和调用归属 | 读取 target 真值或应用目标成本 |
| 结构规则 | 根据 target 配置和 workload 生成操作、工作量、依赖、资源角色及条件分支 | 读取回归系数，以成本缺失为由删除合法操作 |
| 成本与执行 | 迁移确实未变化的成本，查询独立模型，沿同一 DAG 时钟执行 | 用成本模型补结构规则，或把缺测当成零 |

时间相关分支不能事先全部展开。例如预取是否超时，取决于实际完成时刻。
第二阶段先给出条件和后续操作规则，第三阶段提供服务耗时；执行器到达完成点后再调用结构规则。
它们共用一个时钟，不另算一份只用于报告的“理想目标图”。

常规队列检查目前仍迁移源窗口内的检查机会，再按目标执行时刻计算可确认数量；
它不是完整调度器模拟，也不主动生成 HTTP 结束后的无限空闲轮询。
所以已执行 DMA 可能在有限检查机会之后完成，预测图中仍为未发布，而真实服务在后续轮询中已发布。
这项边界单列为结构近似，不能强制把设备完成当成 host/storage 发布，也不能借此延长 HTTP E2E。
本轮对应证据和差异数量见工作进展，不以 60 格完成宣称任意调度变化已被覆盖。

结构侧使用 `OperationWork` 描述操作及必要 shape，不携带参数、数据库路径或目标耗时。
`CostMigration` 按对应源节点记录工作是否未变；结构模块先移除已替换区域，再拒绝剩余的变化工作。
成本绑定因此只沿用这些对应节点的源成本，变化或新增操作走独立模型，不留一条不可达的“迁移中预测”分支。
成本提供者只读取工作描述并返回各计时口径的数值、来源和覆盖范围；执行层填入已生成的槽位。
操作增删、资源位置和等待关系仍由结构侧负责，经过正式 mutation 接口及组件归属检查。

`structure/tensor_program` 是 Attention、metadata、普通计算与 allocator 共用的操作程序。
它记录子操作、前驱、执行方式和后端子启动；`runtime/tensor_program` 绑定资源与成本。
设备消费者等待设备完成，纯 CPU 消费者等待主机返回；异步设备依赖不提前阻塞 CPU 提交。
Attention 的两次逐层缓存检查也只在 K/V 主机提交之后发生：未启用回载时直接返回，启用时向设备流
提交 load event wait。它们都不要求 CPU 先等 K/V 写入完成；设备侧先后关系由 stream 和事件依赖保留。
阻塞读回可以先准备参数，到复制入口才等待设备，不能把整个调用都放到等待之后。
声明了多个子启动时分别计费，不能再叠加父操作的设备总时长。

### 仿真需要多细

判断标准是能否改变后续工作、并发或完成顺序，不是 trace 中有多少条记录。
缓存状态与执行时间是两个尺度：状态按必要的请求、页范围及批次更新，服务成本可按有明确工作量的逻辑操作估计。

| 保留边界 | 可以使用粗粒度的内部工作 |
| --- | --- |
| 缓存命中、容量分配/释放、提交、完成与发布 | 同一状态结果的连续页用范围表达，不逐 byte 模拟 |
| CPU 提交、worker 排队、设备流、同步及通信参与者到齐 | 无外部消费者或资源切换的内部计算，用张量调用或逻辑服务的响应表达 |
| timeout、取消、重试和其他会改变后续工作的条件 | 已确认分支的固定管理工作用典型成本，不为每条 Python 语句增加回归 |

一个 Attention 调用的耗时可以用 shape 模型估计，但不能连同前后回载、同步和缓存发布一起压成一个数。
一个 I/O 批次可以按页数与字节数估计服务，但若逐批发布会让前台提前继续，就必须保留发布边界。
同样，worker 返回与设备完成可能重叠，不能合并成串行总耗时。

当前后端有明确子启动及交错依赖时仍展开这些启动，不因成本包齐全或代码量目标而直接折叠。
合并前检查区域是否有外部依赖、资源切换、状态变化及条件分支；证据不够时保留现行粒度。
父成本拆成子成本后不能再收费一次。CPU 服务、残余区间及无法解释的 gap 仍分别管理。

### 当前结构规则的范围

下面说明已经实现的规则及主要边界，不表示矩阵验证了每一种布局或所有后端。

| 操作族 | 目标工作怎样得到 | 主要边界 |
| --- | --- | --- |
| Attention 与页表 | 请求上下文、目标 token 数、页大小及真实调用布局；页索引、连续化、除法和逐层计算分别生成 | 当前部署的单请求、非投机、非 MLA/滑窗路径；分块调度及改变 graph dispatch 不隐式沿用源模板 |
| 普通投影 | 真实二维 Linear 的 M/N/K、dtype、stride、bias；packed token 行数改变时更新 M | 当前已识别的非量化 Qwen3 角色；未知模型、量化、重排、空 token 和新 graph 布局需另外支持 |
| 融合激活及归一化 | SwiGLU 的 rows×width；RMSNorm 的行数、宽度、epsilon 及残差布局 | 融合 add-RMSNorm 与 batch-invariant 的 Add→RMSNorm 分开；不能仅按函数名推断融合 |
| Q/K、旋转与 Decode 融合 | 实际头数、Q/KV 宽度、旋转布局；reshape 是否复制由共享存储及行距决定 | 已识别的 packed、无 bias 路径；特殊 mrope dtype/layout、offsets、新形状编译准备等仍受限制 |
| Embedding | 实际索引、局部词表和输出几何；分片索引掩码、查表、清零及 TP 归约分别归属 | 已识别的非量化 int64 索引路径；工作量不是物理访存量，索引局部性尚未解释 |
| logits 与采样 | Prefill 末行选择、单行输出投影、TP 收集、输出整理和 argmax 分开 | 普通单请求、末 token、贪心路径；输入 logprob、随机采样、LoRA 和自定义处理不套用普通规则 |
| eager 输入准备 | 根据实际注册 buffer 和目标 token 行数分组复制，入口、组间推进、末尾整理各自计费 | 单请求、连续一维注册布局；无 pipeline 代理输入，多张量转换及自定义回调不自动支持 |
| allocator | 每请求 sequence/prefix 长度、目标页几何及 free/release 状态；准备、分配、索引写入分别生成 | 只使用已声明的后端 ABI；naive/JIT、首次准备、排序、失败重试均有独立规则与成本需求 |
| Prefill 接纳 | 声明的生成预留、未命中输入和目标可用页共同决定是否等待；待完成写入经非阻塞确认后重试 | 当前串行、非分块路径；失败匹配的缓存拆分保留，新增接纳 CPU 复用粗粒度独立调度区间并单列近似 |
| 通信 | 参与者、载荷、输入事件、传输、完成事件及等待规则 | 普通 HCCL all-reduce 和末 token 单行 all-gather；就绪后的完成区间不是通信带宽，也不包括源侧 peer 等待 |

布局相同不等于成本必然不变，名称相同更不能证明几何相同。未被模块接管的阶段节点只有在工作不变时
才可迁移；工作变化但没有生成规则时报告结构缺口，不再使用 `prefill.common/decode.common` 整段缩放兜底。
共享 sin/cos 准备按真实起始层归属，不逐层重复收费；Decode 的融合 QKV/归一化/RoPE 不再叠加独立 Q/K RMSNorm。

页表等张量操作按后端规则展开，而非按源启动数复制。例如 int32 页表除法的 Slice/AsStrided 选择
取决于页步长及末页是否完整，随后才做转换和除法；源导出使用同一规则并核对原生标签和实际顺序。
标签及数量相同仍不是 dtype、layout 或完整依赖正确的证明。新形状、后端和资源的适用性必须另行核对。

实验中的原生 worker 对应使用实际提交关联区分同名 `Node@launch`，再在同一请求、阶段和
操作族内对应重复调用。复制节点沿原始节点取关联，不能把通信启动和计算启动混排编号。
缺少唯一提交归属时保留其图路径并报告无法对应，不用两个未知归属证明一致。
生成张量操作、通信完成点与剩余原生操作分别比较；其中一项一致不代表整图一致。

独立目标构图也不是完整依赖真值：当前基础跨 rank 合并连接 HCCL，但没有重建全部 CPU
通信的参与者到达/返回关系。预测图补入这类关系时会出现额外可达端点对，不能仅凭数量
判成错误串行化；需要沿实际路径核对，也不能反过来将所有跨 rank 差异一概判为正确。

HiCache 结构评分区分提交、设备完成和缓存发布。D2H 的 `start_writing` 返回只证明已提交，
不能直接证明 host 缓存可见；`completed_page_count` 按对应的确认调用统计发布页数，与预测状态口径一致。
已纳入末请求生命周期的后台提交可以晚于 HTTP 结束，仍属于该生命周期的结构工作，
但不因此扩大 HTTP E2E 窗口。未发布的页与未执行的传输不能混为一谈。

### 源观测、资源与计时

源图由 `source_dag/build` 构建，源回放由 `source_dag/replay` 在私有副本上执行，
先于 CPU 开销校正、target 变换及 NodeScale。回放中的队列顺序和模拟状态不能写回预测输入。
同输入、同成本包的报告开关对照已实际完成，具体证据见工作进展；不能把单格对照当成全图结构证明。

源观测保留 Torch shape 包络、framework 轻量描述和真实提交关联。通过同线程包含关系及 Correlation 边
建立唯一归属，不按 kernel 名、最近时间或同进程窗口重叠猜测。后台清理恰好落在 Prefill 窗口内，不足以归给 Prefill。
缺 shape、重复轮次、未知 rank、边界冲突和缺少完整调用均明确报告，不能由缺失推断为零工作。
没有设备提交的视图等 CPU 调用，仅在完整包络落入同线程、已绑定请求与轮次的 forward 时建立阶段归属。
这只补充调用身份，保留实际 CPU 工作与成本，不生成虚构的设备操作。

已有 manifest 墙钟参考时，在通道合并和窗口裁剪前统一坐标，保留 vendor/framework 锚点和原始文件。
导出图的模拟 ts/dur 与 observed_start_us/observed_duration_us 分开，原始 pid/tid 及纳秒余量也保留；
新增节点不伪造原观测。窗口前的描述可用于解释跨界调用，但不恢复窗口前执行或扩大 HTTP 计时范围。
跨起点的 Torch CPU 父包络若已有完整窗口前子调用，裁切时保留其父包络身份，不能因子调用被裁掉而把整段墙钟变成 CPU leaf。
未直接测量的返回尾部继续作为间隔处理，不补回已完成子调用的服务，也不把父包络和子调用重复计费。
贪心采样在 forward 返回后执行，阶段归属使用带请求身份且包含 forward/采样的完整 batch，不能按最近时间猜测。
HTTP 结束前已开始的 batch 保留其调用、提交及因果尾部；不保留随后新开始的普通 batch，也不扩大 HTTP 窗口。
结构替换按完整已准入调用处理，不能替换了窗口内生产者，却留下窗口后的旧消费者。

write/load 确认同样保留已开始调用的 MIN、返回及后台通信尾部，不补入其后的空闲轮询。
跨正式起点的检查只有在显式 barrier 或完整观测证明被裁前缀是空队列准备时，才接纳窗口内确认部分；
执行时仍要求目标对应队列为空，源计数不作为目标确认数量。
Gloo 没有直接提交关联时，只接纳完整同步调用内同进程、同操作的唯一 worker，缺失或重复仍报告错误。
窗口前已经入队、窗口内仍执行的 worker 任务，以独立观察 lane 上的零服务入队点保留初始 FIFO 顺序。
只有唯一原始跨线程提交能提供这种依据；观察点不是缺失成本的零值替代，也不恢复窗口前 CPU 服务或增加 HTTP 端点。
提交与 worker 时间相同时仍按提交导致执行的方向连接，不能用区间长度或节点排序反转因果关系。
缓存插入跨过正式起点时，起点之前已发生的进入与路径推进步骤在首个 HTTP 可用点初始化。
窗口内剩余步骤仍绑定各自的精确 CPU 边界，并依赖该初始化；不在正在运行的原生调用中制造切点，也不恢复窗口前服务耗时。
跨起点的设备传输沿唯一原始提交身份归属，提交元数据保留 pid/tid 与原区间；初始入队点也保留原调用者信息。
worker 任务跨起点而内部原生提交已结束时，在裁切前保留包含关系证明的排队身份，并传给该提交关联的设备事件。
这些上下文只解释窗口内保留的传输和完成事件，不把窗口前已经完成的传输重新计费。
跨起点写回只移除同线程可精确归属的剩余调用区间，以及原始提交身份证明属于它的初始排队任务和保留传输。
CPU 入口仍在真实调用者的 lane，并依赖起点状态初始化；不以 HTTP lane 充当提交线程，也不要求被裁掉的半段成为完整模板。
探针把传输量放在调用的 start 记录时，与跨窗口 return 精确匹配的 start 描述也保留为非执行元数据。
丢失描述不能被解释为实际传输量为零。

源图对已有明确同步依赖仍使用历史 10 微秒服务近似，原阻塞区间另存。它不是独立测量，
不能导出为训练样本，也不能再次按原阻塞区间扣除采集开销。
`execution_cost_bound` 防止最终执行成本重复校正，不表示测量准确或覆盖充分。
`dependency_audit.observed_clock_order` 比较原始设备完成和阻塞调用返回；时钟矛盾不因 HTTP 回放误差小而消失。

调度准备流与 forward 流由各自的实际提交确定，同一 CPU 线程不代表同一设备流。
I/O 所属主机调用沿 Correlation 追溯，不能用较晚的设备 submit_ts 或设备执行时刻替代调用锚点。
CANN 的 `connection_id` 为 `-1` 或其 uint64 表示时，代表没有有效关联：原字段和事件仍保留，
但不参与提交关联索引。两个缺失编号相同，不是两个操作存在依赖的证据，也不能跨队列批次连边。
写入切点依据唯一 Stream 顺序；无 CPU 关联的零服务 PLACE_HOLDER_SQE 只有在全部活动边均为 Stream 时
才可保留并跨过。存在服务或流外依赖时仍拒绝猜测，不通过删除未知消费者解除错误。

### HiCache 控制与成本边界

源写入和淘汰片段在完成归属检查后，只保留资源角色及插入位置的观测身份，不再长期存放一份源拓扑和成本副本。
目标容量步骤另由状态和执行规则生成，不能借源片段的页数或分支决定目标工作。
执行摘要中的 `observed_write_fragments`、`observed_eviction_fragments` 是源资源依据的数量，不是目标模板数或执行次数。

写回确认按 FIFO 批次生成：每批确认后即可发布 host 状态并提交 H2S，容量释放仍等整个阻塞检查结束。
阻塞与非阻塞路径的队列、局部事件生命周期不同，不能共用整段墙钟或从源模板决定释放次数。
加载使用持久事件池，不套用写回的事件销毁规则；多批次间推进也必须有独立成本依据。

host 加载在 lookup 保存目标命中范围，到 admission 才判断容量与展开操作；
期间后台新发布不能扩大已匹配范围。源检查的 true/false 只标识原分支，不替目标决定命中。
前序资格和初始化依赖完整源观测，前序 I/O 完成时刻仍有推演近似，不能当作完整执行式重建。
保留 CPU 通信的批量计划在同一未变源图上共享 caller 顺序索引；每轮仍独立检查节点归属、计时和已有 mutation 的交集。
这个索引的生命期不跨图修改，不把顺序检查缓存当作后续图变换的有效性证明。
allocator 准备依据特化、缓存路径及首次加载情况，不把源编译重叠作为目标特征；
跨 rank 编译竞争仍是未解释的成本变化。

生成器同时给出工作描述与数值槽位，计算、分配、释放、传输和写确认共用数值绑定，
分别查询 CPU、worker、dispatch、设备及 residual。相同重复工作可共用响应，不合并执行节点或依赖。
同步前准备与等待后的同步服务分开，无法从真实等待中分离的成本不入库；已可测的其他片段可独立保留。
模型缺失时返回明确缺口，不用整段墙钟或零值补齐。各类公式及近似见成本模型文档。

### 数据与报告的边界

base 仅提供对应且未变化的成本，变化或新增操作使用独立数据库离线训练的成本包。
预测不打开数据库、不自动导入 base、不读取 target；共享补采默认关闭。
成本包须留出整轮 base 与评分采集；完整/轻量任一侧被留出时，整份测量不能入训练。
别名用原采集身份表达，不用摘要或内部版本号证明有效。部署与测量口径的要求见第 7 节及成本文档。

独立测量可用 `--include-setup-requests` 扩展到全部有时间戳的受控请求，但配对校正必须覆盖同一范围。
它不改变预测或评分窗口，也不授权将窗口外未经校正的区间标为正常 CPU 成本。
部署声明包含框架、模型/TP、dtype/量化、graph/编译与资源环境；未知或冲突条件不能假定兼容。

实验用 `--experiment-reports` 保留源回放和实际展开结构，再独立 `evaluate`；三份报告的含义见[验证说明](validation/hicache_validation.md#三份报告分别证明什么)。
结构构图可按 `--model-run-jobs` 并行，默认一、各输出独立，不启动设备采集或放宽 profiling 串行规则。
代表 target 只证明已比较范围，其他格明确未请求；完整 HTTP 仍须全部选中格的正常真值。
操作、工作量、资源与完成点可达性分别比较，不先填 target 成本，也不合成整图相似度。
HiCache effect 包括“不需要执行”的决策；Decode 机会由目标侧分配调用、prompt 身份和后续 forward 独立关联，
不是复制预测结果。缺关联单列 target_observation_blockers，不能当作没有工作。

原生和重建调用分区比较，未对应工作、逻辑等待及其他合成节点另计。
可达性可以穿过中间节点，不证明中间工作或物理身份已对应，也不证明全图同构。
默认保留紧凑摘要；显式 diagnostics full 才保留详细问题清单。重新运行同一目录时清理旧报告，
防止关掉报告后误读旧文件；dry-run 不执行这种清理，原始采集不受影响。

### 核心存储与执行约束

TraceGraph 头文件统一在 `include/markov/trace_graph/`，实现在 `src/` 的对应模块；
内部辅助头不因移动目录成为公开 API，引用使用完整 `markov/trace_graph/...` 路径。

trace 读取器和参数查询共用 `json_scan.hpp`，保留共享缓冲区与按需解析，
不为简化而构造整份 trace JSON。转义由现有 JSON 库解码；通道合并保留默认 pid/tid。
跨 rank 合图保持各输入顺序，事件与语义侧表重新编号，输入合同在合并后去重。
CPU 队列统一扫描提交关系建立 FIFO，不逐线程反复扫描整图。

FutureDag 不回滚已开始工作，事实身份与执行节点身份分开。
写入、分配和回载通过 replay 统一索引查事实；Decode 派生事实入表后先重建索引，再绑定边界。
正式窗口只改变测量范围，不清空活跃请求、缓存或 KV 所有权。
空 radix 插入仍保留请求尾页，退出时释放；不足一个页的请求仍有分配和 Prefill 工作。
窗口前已发生的生命周期推进只初始化状态，窗口内剩余步骤继续执行，不恢复窗口前服务。

分配迁移还需长度、batch、新页数、队列、指针偏移和准备历史相符；未知字段不视为相等。
Decode 的 last_loc 来自 int32 请求索引表，不套用 extend 的 int64 假设。
没有新页且无需 dtype 转换时不生成设备工作。冷 Triton 准备/装载与热调用分开，
观测区间取并集，CPU 与残余分别计量，不将历史静态聚合成本放进正式预测。

预取检查 MAX/MIN 按实际参与者生成，请求广播保留通信组顺序与 root。
scheduler 接收、写确认、加载确认和存储检查的顺序来自框架规则，不复制测量步骤列表；
只有执行到重试才查其成本。目前空选择循环仍是近似，尚未解释任意动态队列负载。

预测执行器不跨调用保存状态。串行和并行共用有界任务队列，补发前检查整批完成结果；
已启动任务正常收尾，未启动格明确标记。没有有效 HTTP 窗口的运行不能进入精度评分。
其余不可违反的边界见[项目约束](project_constraints.md)，历史日志不替代当前接口说明。

## 10. 容器与构建

SGLang 推理、profiling 和设备校准使用 sglang；KTransformers 采集使用 ktransformers；
观测提取、DAG、构模、预测和评分使用 modeling。公开脚本负责调度。

同步读回缺少可靠的非等待开销时，可显式采集空闲流上的 runtime 原语：

~~~bash
scripts/model.sh calibrate-hicache idle-sync \
  --devices <空闲设备列表> --cpu-sets '<每设备CPU集合，以竖线分隔>' \
  --output-dir <采集输出目录>
~~~

入口在 SGLang 容器构建正式采集程序，按指定设备串行测量，不加载模型或运行 workload。
计时前先在流中记录并完成一个事件，再测量没有待完成工作的同步调用；原始重复样本保存在
`idle_sync.json`。计时不带 hook/profiler，不包含 Python 调用开销，使用主机单调墙钟，
因此不能把它称为纯 CPU 服务，也不能用于估计设备等待或被唤醒的额外延迟。
这是显式独立测量入口，尚未接入组内自动补库。输出的 `operator_observations` 可连同显式部署声明，
通过现有 `cost-db import --evidence` 的 `cpu_primitive` 类型入库，再离线训练。
已有测量可用该采集模块的 `operation_observations` 转换，不必重复运行设备。
正式读回成本已接入空闲同步与读回尾部的组合；采集命令成功仍不表示整包或整组所需成本已齐备。

~~~bash
scripts/run.sh modeling -- bash -lc \
  'cmake -S src/modeling/trace_graph -B build/modeling/trace_graph-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DTRACE_GRAPH_DEBUG=OFF && cmake --build build/modeling/trace_graph-release --target trace_graph -j2'
~~~

详细诊断和历史静态回放需要 TRACE_GRAPH_DEBUG=ON 的 validation 构建。
历史静态 patch、边界/落图检查和 target observed carrier 只在诊断构建中编译；
Release 与 validation 的正式预测仍共用执行式模型。相关检查命令见仓库 AGENTS.md。
