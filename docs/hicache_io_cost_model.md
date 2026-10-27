# HiCache 数值模型

> 2026-10-27：本文保留已归档实现的成本原理与限制，不是新实现的设计或精度承诺。
> 旧源码位于 `deprecated/`，迁移前运行基线见[仓库说明](../README.md)。

本文回答：每类成本表示什么、工作变量从哪里来、系数怎样估计、预测时怎样使用。
命令和开发位置见[建模说明](modeling_development.md)，当前成绩与数据资产见[工作进展](work_progress.md)，
验收口径见[验证说明](validation/hicache_validation.md)。历史聚合模型只在文末说明，不混入当前预测。

## 1. 成本在三阶段中的位置

~~~text
source trace → 原始 DAG 与工作观测
原始 DAG + target 配置 + workload → 目标操作、依赖和条件分支
目标操作 + 未变工作的源成本 + 独立成本包 → 同一时钟下的执行与完整 HTTP
~~~

结构层决定做什么，成本层回答需要多久。时间相关分支由执行器到达完成、发布或 deadline 后再触发，
不能因为没有样本就删除合法操作，也不能用回归系数决定缓存策略。

实现上，C++ `cost/operator_model`、`host_operation` 和 `io_service` 负责独立数值估计，
`source_migration` 负责明确未变工作的源成本。HiCache runtime 将结果绑定到工作和执行时钟，
不在数值库内创建节点或推进缓存状态。分配准备的源样本推算和静态 phase 成本位于诊断侧，
不作为正式预测的兜底。当前验证范围与限制见[工作进展](work_progress.md)。

HiCache I/O/control、Prefill、Decode、CPU residual gap 和采集开销分别管理。
完整 HTTP 没有剔除这些成本；未变工作的源成本和部分残余等待仍保留，不能说已经建模了所有等待。
操作 service、节点耗时总量和 HTTP 关键路径是不同量，不能直接相加当作 E2E。

正式窗口是计时范围，不是缓存或异步队列的重置点。准备请求已接入连续执行，并通过构建与实际代表验证：
最后一次已声明 idle barrier 之后的准备请求，与正式请求共用目标执行时钟；最终 HTTP 只取正式请求的起止差。
准备请求的未变成本仍可迁移，变化工作查询独立模型；同源 CPU 修正只覆盖实际测量区间，未测区间保留源成本而非记零。
结构报告标明准备请求数量，不能把准备耗时加入正式 HTTP，或在计时开始时提前结清仍有影响的写入。

| 数据 | 当前作用 | 不能承担的作用 |
| --- | --- | --- |
| 当前 base profile | 原始图、请求、工作观测，以及明确对应且未变工作的迁移成本 | 为变化操作在线拟合参数，或补 target 真值 |
| 独立 runtime 测量 | 同部署的逐操作工作量与各计时口径 | 当前 base/评分采集的隐式副本、逐格残差补偿 |
| 独立物理测量 | KV 几何、DMA/存储基础响应和测量条件 | 完整推理中的争用、CPU 控制或计算成本 |

预测不打开数据库或训练 trace，只消费已选成本包。默认不补采；
显式开启时，先检查已有独立数据库，再安排全组共享实验。未知结构、缺模型声明或缺工作变量不能靠重复采集解决。

独立性能采集与数据库维护是一条长期主线，模型包是它交给预测的结果，不是用户必须为每个 target 制作的输入。
`prepare-costs` 组织显式独立采样、同源校正、事实入库与离线训练，不含 target，也不按 target 调整采样或参数。
首次使用可以从已有库离线准备适用模型；新平台缺数据时仍须独立测量，空库不代表模型已经可用。
base 的结构观测、未变成本迁移及采集开销修正，与数据库的变化操作训练保持分工。
库内操作测量、部署和来源各有唯一归属，控制报告不重复保存测量行；
I/O 证据保留实际服务数据及测量条件，而非整份诊断。存储转换不改拟合方式，具体维护合同见
[性能库的事实归属](modeling_development.md#性能库的事实归属)。
物理采集不再提前拟合：导入将明确声明的存储工作点、DMA 设备计时和预取阶段测量存入库，
成本层在离线训练时构造物理响应。历史报告中的系数不作为测量，也不反推伪样本。
存储工作点保留采样时的 percentile 选择；预取按页宽/批次聚合阶段时间，DMA 按每调用字节规模聚合设备时间。
这些计时口径和估计方法各有用途，不为统一代码强行合并。物理缺口只在实际需要该物理依据时阻止相应响应，
已有独立 runtime 测量能辨识 DMA 时，不因未用的物理 DMA 曲线缺失而拒绝。

## 2. 变量、系数与模型

### 模型身份与工作变量

一个独立操作模型由 `operation`、`implementation`、`timing_scope` 选定，并限于声明的部署。
操作名回答执行什么，实现名区分真实后端/布局，口径回答测量的是哪段时间。
配置名、workload 名、cell ID、target E2E 及逐格误差都不是变量。

`OperationWork` 保存结构侧推导的工作，不携带数值参数、数据库路径或 target 观测。
典型变量是 token/page/byte 数、M/N/K、元素数、参与者数和必要的执行条件。
同名 kernel 或相同元素数不足以证明 dtype、stride、布局及执行路径相同。

`features` 进入公式，`coverage_features` 只限定测过的工作条件，不增加系数。
常数也必须说明适用域，不能因为没有变量系数就适用于任意大小。
只在底层工作等价时用 `observation_operations` 显式共享观测；不同实现、口径和部署不因此合并，
原始名字与来源也不改写。

### 形式和训练方式

~~~text
constant:          t_us = a
linear:            t_us = a + Σ(b_i × x_i)
piecewise_linear:  在已测单变量锚点之间插值
~~~

截距是否拟合由 `fit_intercept` 声明。变量须有机制依据，相关变量不能一股脑加入造成秩亏。
例如 M×N×K 是 MAC 数，一个乘加记一次 MAC，不是 FLOPs；
输入 M×K、权重 N×K、输出 M×N 是逻辑张量大小，不是实际显存流量。

训练先在每次独立采集、相同拟合坐标内取中位数，再估计参数：
常数取这些聚合响应的中位数，线性按实际聚合点最小二乘，分段模型按各锚点聚合。
重复层调用不会仅凭调用数压过另一采集；线性模型也不宣称每种采集无条件等权。
范围变量不改变拟合点权重，只增加适用域的坐标。

一个 `operation/implementation/timing_scope` 只声明一个模型。
相同拟合规则可用明确的操作和口径列表一起声明，输入处展开后仍逐项选样本与估计系数。
共享声明不代表共享测量或同一参数；训练与补采需求匹配共用这个展开结果。
通配实现只训练所请求口径下已有观测的实现，其他实现的缺失口径另列，不等于它们获得覆盖。
缺操作、缺口径、缺变量、工作点不足或秩亏分别报告；不自动换公式、删变量或补零。

局部回归的系数可以为负，不逐项解释成物理带宽或服务。
总响应为负、非有限或不能表示为仿真时间时报告成本缺口，不截断为零。
`training_fit` 是聚合点上的训练内诊断，含误差、参数数和剩余自由度，不是留出精度或置信区间。

### 预测与迁移

结构模块先确定目标工作。若对应操作的部署、实现、shape 和执行条件确实未变，可以迁移源成本；
phase 的候选按源节点记录未变条件，已被结构模块替换的区域移除，剩余变化工作必须拒绝。
没有“迁移失败便用整阶段比例缩放”的兜底。

变化或新增操作通过 `OperatorCostProvider` 查询已训练响应。
同一个成本包的响应、部署和来源在预测输入处共同准入。提供者按已声明变量建立一次工作点，
范围判定、响应求值和未见组合报告共用该工作点，不逐层重新解释原始字典。
`cost/host_operation` 组织一个已确定程序所需的各口径，全部解析成功后才绑定数值；
缺口汇总，不返回填零的半个成本计划。实际操作数、等待关系和资源角色不由提供者修改。

计算、分配、释放、load/write 提交和写确认由生成器声明实际工作、成本需求及数值槽位，共用估计与绑定。
成本层不按页数或策略另建操作清单，CPU-only worker 使用完整任务口径，不虚构设备或启动/返回成本。
完全相同的重复拷贝或记录可以共用一次响应求值，但每个真实操作仍各自绑定耗时与依赖。
同步槽使用独立空闲 API 服务，调用残余单独查询，设备等待由依赖承担。
查询计数表示响应求值次数，不等于 DAG 操作次数；整理查询批次可能改变它，不应据此推断工作量变化。

`allow_extrapolation` 明确允许所有声明坐标外推，`extrapolation_features` 只放开列出的坐标；
其余范围仍检查，分段线性禁止外推。变量各自处于范围内不代表组合已测过。
允许外推和未见组合随报告保存，查询成功不是可靠外推的证明。

## 3. 时间口径不能互相替代

| 口径 | 计什么 | 不计什么或主要限制 |
| --- | --- | --- |
| `cpu_service`、`thread_cpu_service` | 对应调用线程的服务工作，满足各自采样/校正合同 | 不把设备等待或整个父包络当成 CPU |
| `residual_interval` | 已归属主机片段的残余区间 | 不是整个 residual gap 模型，也不是纯 CPU |
| `dispatch_interval` | 提交到 worker 可执行的跨线程间隔 | 不把源队列等待照搬给 target；仍受调度条件影响 |
| `worker_launch_service`、`worker_launch_residual_interval` | 同一 FIFO 任务中到实际设备启动点的两类时间 | 不按总时长比例拆分 |
| `worker_return_service`、`worker_return_residual_interval` | 该任务启动后的剩余工作 | 与设备可重叠，但仍占用 worker 队列 |
| `worker_service`、`worker_residual_interval` | 没有设备启动的完整任务 | 不虚构 device/launch 槽 |
| `device_service` | 可独立归属的原始设备工作 | 不含设备调度 gap 或前驱等待，不用图归一化值训练 |
| `collective_completion_interval` | 全 rank 输入就绪到整组最后完成 | 不是纯通信带宽，等同行的时间由依赖承担 |
| `control_interval` 与查询包络 | 明确控制片段的墙钟 | 仍可能有残余等待与观测扰动 |
| `blocking_return_interval` | 提交与设备完成以后，调用返回前的区间 | 可能含唤醒/调度，不是纯 CPU；当前统一包尚无此模型 |
| `post_sync_readback_interval` | 主机同步返回到同步复制/调用尾部结束 | 不包含同步 API 自身或此前设备等待 |
| `idle_stream_sync_interval` | 已空闲流上的 runtime 同步服务 | 不测争用、阻塞唤醒或有限超时 |

未校正观测保持 `profiled_*` 口径，不能训练相应正常口径。
CPU 校正、service 和 residual 不互相吞并；缺分段不能用外层墙钟替代。
零测量必须由完整观测证明，不是缺任务或缺 trace 时的默认值。

张量程序的各族共用分段提取规则，设备去重在同一次导出内延续。
冷准备只排除被污染的 caller 口径，不同时丢弃独立可测的 worker/设备成本；
外层准备与返回只有在所有消费边界完整对应后才准入，不能将遗漏操作收费为 CPU 空白。

### 异步、直接启动与读回

异步调用先提交，再由 worker 启动设备；设备可在 worker 返回前开始。
同一调用有多个明确 native 子启动时，每个启动独立计费，不再叠加父设备总成本。
只有证实完整任务和实际启动点，才拆 worker 的启动/返回段；一段不可测不应丢弃其他可测片段。
直接提交没有 worker/dispatch，不为齐字段虚构这些角色。

设备 record/wait 使用共享原语 `device.event.record/wait`，不同调用者只共享该设备服务，
CPU、worker、派发和返回仍各自建模。wait 样本须有唯一且已完成的记录前驱；
实际等待由依赖表达，不把等待墙钟当成事件指令成本。每个原始设备事件只进入共享族一次。

同步标量/上下文读回是“准备 → 等计算流 → 调用线程同步复制 → 返回”，不生成 worker。
明确的调用、同步和 memcpy 证明结构存在；主机/设备时钟不自洽时，跨时钟返回样本仍不可用。
当前估计采用：

~~~text
readback_return = idle_stream_sync + post_sync_readback_interval
~~~

前序设备等待由依赖承担，不再加同一尾段 CPU/residual 或设备服务。
空闲原语测量使用实际被 hook 拦截的 runtime 接口，不等同于外层 ACL 名称；
只覆盖当前普通/无限等待路径，不保证有限超时、阻塞唤醒或拷贝引擎争用。
分项取整与设备观测的亚微秒精度分开，不用修改时间轴或 target 残差来制造可测样本。

## 4. 各操作族为什么这样建模

下面是规则与成本的分工，不是要求每个实现细节各配一条独立理论。

| 操作族 | 工作量与响应逻辑 | 主要限制 |
| --- | --- | --- |
| Linear、归一化、门控激活 | 结构生成实际 M/N/K、rows×width、dtype/layout；设备用有依据的工作量响应，固定主机路径用分段常数 | 小矩阵、分块切换或新布局不保证服从同一直线 |
| Prefill logits 选行 | 长度处理、末行选择和输出投影分开；选行只读一行，不按全部输入行数放大传输 | 普通单请求末 token 路径，当前宽度固定 |
| eager 输入准备 | 同源/目标 dtype 的 buffer 分组复制，token 轴随 Prefill 工作改变；入口、组间推进、末尾整理分开 | 单请求、已知注册布局，不支持任意回调 |
| Q/K reshape | 布局先决定 view 或 clone；单行 view 只有主机片段，不能借多行复制成本 | 需真实 stride/存储关系，形状相同不是布局相同 |
| 页表索引与除法 | 非连续整理、行索引、页号转换/除法分开；页大小不约束它之前的 token 索引成本 | Slice/AsStrided 选择来自后端规则，不能按样本齐全程度选分支 |
| Embedding | mask 的 bool→int64 和乘法、查表、清零、TP 归约分开 | 逻辑元素数未解释索引局部性，量化/新路径须另支持 |
| K/V 写入 | 连续更新与有行间隙视图分开；非连续 V 为 Slice→Scatter，各段按元素数 | 不把 Decode 融合输出布局套给 Prefill 的 QKV split |
| 贪心采样 | ArgMax 按词表宽度，索引 cast 只按输出行数；共用任务但独立启动/设备成本 | 当前 Ascend 910B 连续单行 float32 路径 |
| 回载准备与索引 | 准备按 requested tokens，页号乘法按 pages，arange 按 page tokens，广播/clone 按实际 token；非空前缀拼接另分输入段数 | 节点遍历/祖先锁、未测多输入和尺寸组合仍有近似 |
| allocator 准备 | 实际特化及冷/热缓存决定准备工作，成本来自独立准备测量 | 不是把 source 编译重叠当变量，首次初始化和跨 rank 竞争未充分解释 |
| 固定 control/worker | 按已确认路径分 CPU、residual、worker、dispatch；使用独立典型响应 | 不能用设备元素数强行缩放主机，固定路径常数也有波动 |

页表整理及连续 K/V 的某些底层启动确实等价，设备和启动口径可显式共享两阶段/角色观测；
外层 CPU、派发和返回不随之合并。未知 dtype、特殊 RoPE/mrope、graph 捕获和新调度规则的结构支持
须单独建立，不能用同名 kernel 或新成本样本补缺失语义。完整范围见建模说明的结构规则表。

### TP 通信

普通 all-reduce/all-gather 先由参与组、真实调用位置和载荷确定工作。
每个 rank 保留输入依赖，全部输入就绪后才执行逻辑完成服务，结果消费者重接到该完成点。
逻辑张量字节数不是链路传输量；这只是调用级共同就绪近似，不覆盖全部协议分片和错峰重叠。

同一后端、dtype、hidden width 和参与组的 Prefill/Decode all-reduce，使用同一独立数据池和载荷响应：

~~~text
到齐后的完成余量_us = 固定开销_us + 字节系数_us_per_byte × logical_tensor_bytes
~~~

变量是实际逻辑张量字节数；两个系数由独立采集按“采集、载荷”聚合的中位数拟合，不使用当前 base 或 target 耗时。
Decode 本身只有固定小载荷，不能单独辨识字节系数；可共享的是同一个 `NpuCommunicator.all_reduce` 实现下的多载荷测量，
不是因为两阶段的名字相近。阶段 ownership、caller 和 worker 成本继续分开，只有这项设备完成响应共用声明与测量。
固定开销包含逻辑通信的典型启动/协议余量，字节系数也不能直接当成独立链路带宽。

`arrival_span_us` 是最晚与最早输入就绪差，来自目标执行的就绪点，不来自 target trace。
参与者等待已经由依赖表达；独立留出发现，对到达差继续拟合一条斜率并不稳定，当前不再让它进入数值公式。
它仍保留为已测运行条件：超出历史范围会显式标为外推，其他几何、实现与部署限制不放宽。
这是调用级近似，不能证明任意错峰、内部协议重叠、争用或超时都准确。独立留出的整体服务误差有所增加，
只在少量较大错峰点上更稳；完整 HTTP 的独立对照见[工作进展](work_progress.md)，不把公式简化或顺利执行当成精度提升。

输入时刻按执行器的整数微秒坐标比较，完成区间仍保留原纳秒差的小数精度。
只有同机时钟、完整参与者与真实前驱足以证明就绪时才产出样本，每组一次，不逐 rank 重复计权。

固定小载荷 logits all-gather 使用共享常数，允许仅到达差坐标外推，其余几何仍受限；
观测中局部斜率正负不稳定，不继续拟合以换取 target 分数。域外没有误差保证。

通信 caller 按每次 enqueue 的准备/提交/返回分段，worker 的归约及 record/wait/destroy 各自计费。
设备事件的真实连接移交，不用整调用均值分摊；CPU 通信的 dispatch/到齐后余量仍含运行波动。
原始 HCCL 持续时间、构图器最短非零归一化值和独立完成服务不能混用。
源阶段归属与结构验证规则见建模说明；通信内部全量对应仍未完成。

### HiCache 控制与页式释放

预取查询按采样前、采样到 MIN、MIN 后应用和全组到齐后的包络余量分别训练。
查询有候选才需要这些工作，即使最终命中为空也不能省掉查询；没有候选不要求无用模型。
本地检查按实际分支查询 `prefetch.check.<role>`，仅对应未变的本地提前返回可迁移 base。
MAX/MIN、广播和 scheduler 按实际动作生成，重试次数来自执行状态，不来自校准记录。
这些口径仍是有限域的控制/墙钟近似，不是纯 CPU 或纯带宽，也未完整参数化查询长度和队列负载。

加载准备的路径节点数和请求大小也可以只作为实测适用范围，而不是强行拟合线性斜率。
小路径下的 CPU 增量若无法从固定开销和测量波动中辨识，使用范围内的典型响应，不据此推广到未测深度。
多节点加载还包含首次和后续索引复制，独立采样须导出整条加载链，不能只补准备入口的测量。

加载失败且设备前缀也为空时，后续拼接的两个索引都为空。该路径不提交 `aclnnCat` 设备工作，
使用 `load.admission.prefix_concat / torch_npu.cat.empty / control_interval`，与非空拼接分别维护。
这段局部墙钟包含空拼接、真实 `Req.set_extend_input_len` 调用及前缀 bookkeeping，不拆成没有测量依据的纯 CPU 和等待。
`calibrate-hicache eviction-cpu --operation empty_prefix_return` 在部署 CPU/NPU 范围内测量真实空 NPU 索引，
排除设备初始化与暖机，不运行推理、不传输 KV；当前只覆盖默认关闭 logprobs 的一维连续 int64 索引。
已有非空拼接响应不因补充空分支而重新拟合，也不能把非空样本的范围直接放宽到零。

小容量设备缓存的 Prefill 接纳先比较请求预算与目标当前可用量：预算包含未命中的输入、声明的生成预留及一页对齐余量；
可用量包含空闲/待合并页与可淘汰缓存，但排除本请求需要保护的设备前缀。生成预留来自 workload 的原始输出上限，
不是实际 EOS 长度，并按源部署声明的 SGLang 预留截断规则处理。旧固定长度强制回放可用其完整输出预算；
自然结束记录缺上限时报告源观测缺口，不用实际长度代替。
异步写入仍锁页时，请求继续执行非阻塞调度检查，确认完成后重试，不强制解锁或改变容量。
请求只锁住已经接受的设备前缀；主机引用由预取、加载和存储写入各自持有并在对应完成或失败边界释放。
加载失败后，未加载的主机命中可以被后续清理回收，不能在进入 Prefill 或提交未完成请求时再次以请求名义保护。
这条归属规则同样适用于准备请求；否则模型可能把合法的重新计算误判为容量不足。
失败接纳前已经发生的前缀匹配仍更新 radix 拆分和访问状态，不能推迟到写入发布之后。
新增重试中的这一段 CPU 工作暂用独立非计算调度区间作粗粒度近似，报告其实际触发次数和局限；
它不是单独校准的接纳成本，也不证明任意队列、并发或分块 Prefill 已获支持。

写回确认必须区分阻塞与非阻塞：
每批发布后即可提交 H2S，容量释放仍等阻塞检查结束；事件销毁异步执行，不成为主机返回前置。
非空入口、逐批确认、循环推进、首次/后续事件释放和返回分别计费，空分支另列。
整批数量已通过重复步骤表达；首次事件释放的固定前缀可显式允许批量上下文外推，但不放宽其他操作范围。
实际事件合并不等于逐逻辑操作销毁，提取合同不匹配的整批区间不能硬塞给单步成本。
`write.acknowledgement` 若已包含在 poll/尾部中，不重复相加。
调度重试中的写确认与普通确认共用逐批发布和异步事件销毁程序；展开整个 MIN 后尾部时，不再叠加固定调度尾部。
加载使用持久事件池，不套用写回的销毁规则。确认样本只在无未分离设备等待、归属完整时接纳。

释放将两次同步与准备分开：页号/去重随释放页数近似，拼接才读取原缓冲向量。
不遍历缓冲区的片段不能误用 `release_buffer_pages` 限制；实际拼接的 caller、worker 和设备仍记录它。
当前小向量常数允许显式外推该长度，限于当前一维两输入、小规模路径；不代表任意拼接带宽。
缓冲区零页是合法状态，测过单变量范围不等于测过“一页释放、零页缓冲”的联合工作。
同步服务仍采用空闲原语近似，唤醒/争用单列；释放准备的历史训练误差明显，不能称精确模型。

分配失败只做页数计算和容量比较，不执行成功分配的索引和 tensor 工作；
未排序失败路径用独立线程 CPU 常数，不按 requested tokens 当带宽。排序/合并队列不能直接套用。

写回的主机分配失败是另一条合法分支：尝试分配、清理可淘汰的主机节点、再次分配；仍放不下时返回零，
设备淘汰继续检查后续候选。它不提交 DMA，也不能使用成功写入的成本。目标状态记录备份大小、主机候选数和
实际释放页数，DAG 生成对应的 CPU 操作。独立原语通过实际 SGLang 方法测量这段线程 CPU，
以主机候选数和释放的 token 数近似清理工作；请求大小和页大小保留为适用范围。原语不采目标 workload，
不包含推理争用或调度等待；平坦主机叶子测量也不证明深层祖先清理的精度。缺少适用模型时仍报告成本缺口。

该原语复用 `calibrate-hicache eviction-cpu`，显式选择 `--operation backup_rejected`，并声明
`--cpu-sets`、`--heap-sizes`、`--page-sizes`、`--batch` 和 `--repeats`。默认操作仍是锁定候选循环；
CPU 原语结果须经独立数据库导入与离线训练后才能用于预测，不在 predictor 中临时拟合。

加载在淘汰后仍分配失败时，还会解锁设备前缀、释放主机引用、写警告日志并返回；
首轮分配失败后又成功的观测不能代替这个返回分支。该段使用 `load.index.failure_tail` 的
`control_interval`：从实际 `dec_lock_ref` 入口测到 `load_back` 返回，作为一个包含 CPU 与本地日志等待的墙钟区间，
不伪造 CPU/等待的拆分。采样前的分配与淘汰受控失败，但不进入计时，也不声称测到了它们的成本。
原语入口为同一命令的 `--operation load_return`，范围由 `--page-sizes`、`--request-pages` 和
`--prefix-depths` 声明。当前返回描述只有请求大小与页大小，使用常数响应；采过有限前缀深度不等于
已支持任意树深或日志争用。该控制区间也不是 HTTP 残余 gap 的校准。

### 淘汰时跳过锁定候选的 CPU 成本

~~~text
cost_us = a × pop_count + b × sum_log2_heap
~~~

变量是弹出次数，以及每次弹出前堆大小 log2 之和。预测单次跳过查询 `(1, log2(H))`，
H 来自目标缓存状态。系数由框架原始循环的独立批量线程 CPU 测量估计：
同一候选集合的“只建堆”基线与“遍历并跳过全部锁定节点”相减，重复测量中位数入库。
无额外截距，零操作对应零工作；整批推导单次依赖可加性近似，不伪装成逐次实测。
当前证据为每个实际绑核范围的 1/8/64 候选，不保证带负载与空闲原语完全相同。
工作不存在与缺成本分别处理，不能因为没有传输就忽略 CPU 工作或填零。

## 5. HiCache I/O service 公式

时间为微秒，B 为 byte 数，P 为 page 数，G 为每 token、每 rank 的 KV bytes。
工作量从实际执行规则得到，最终可见页数不能代替已执行量。
服务估计同时用于状态推进和 DAG，不另维护 planning bandwidth。

### Prefetch

~~~text
page_bytes = target_page_size × G
physical = P × (before_copy_per_page + page_bytes × before_copy_per_byte)
         + copied_pages × (copy_publish_per_page + page_bytes × copy_publish_per_byte)
         + service_calls × return_per_operation + P × return_per_page
predicted = physical × runtime_scale(page_bytes)
~~~

实际读取、复制、完整批次发布分别保存，取消也可能已做 I/O。
六个阶段系数来自独立物理测量，service_calls 来自存储 batches。
只接纳分阶段证据，不用旧总带宽形式兜底；单页宽只能识别该页宽响应，不能证明固定项和字节项可独立辨识。

运行倍率只接纳工作量匹配的独立轻量测量，不用带 profiler 的函数墙钟放大正常服务。
没有这类依据时使用已有分阶段物理公式、倍率为一；这不是证明推理争用为零。
轻量测量也保留计时和资源波动，不能称完全无观测开销。

### Load 与 D2H

同页宽有两个不同的每调用字节规模时：

~~~text
predicted = operations × setup_per_operation + B × 1e6 / bandwidth
~~~

每份采集按调用数归一化、同规模取中位数，再由两端规模确定直线。
负 setup、非正带宽或单一规模不算辨识成功，不裁剪系数强行接纳。

独立运行不足时，使用适用物理曲线和有依据的倍率：

~~~text
physical = B × 1e6 / physical_bandwidth(page_bytes)
         + operations × setup_per_operation(page_bytes)
predicted = physical × runtime_scale(page_bytes)
~~~

Load 与 D2H 各有曲线，不共享 target 修正项。没有运行倍率依据仍明确保留基础物理近似，
不拿当前 base 或 target 残差补系数。

### H2S existing 与 new

~~~text
existing = Σ_batches(
    existing_pages × page_bytes × 1e6
    / physical_existing_bandwidth(page_bytes, batch_pages)
) × existing_runtime_scale(page_bytes)

new = Σ_new_batches(
    setup_per_new_operation(page_bytes)
    + new_pages × page_bytes × 1e6 / new_bandwidth(page_bytes)
)
~~~

existing 受 batch 深度影响；new 每个新操作支付一次 setup。
系数来自独立运行的不同调用规模或适用物理测量，混合新旧页必须分别有依据。
不能用最终可见量抹掉已执行写入，也不把它们合成一个逐配置倍率。

端点间按 page bytes 的对数坐标插值，域外使用最近端点并明确外推限制。
当前 page128 Load、较小页宽 existing H2S 和预取倍率均有端点沿用，
new H2S 的推理争用也未单独验证。实际锚点、来源及限制见工作进展；
独立操作查询无域外请求，不代表载荷 I/O 无外推。

## 6. 独立测量的提取与开销修正

### 共享 CPU 操作校准的采集开销

同一独立 workload 做完整与轻量回放，核对配置、forced tokens、请求/rank 和实际计算工作，
再生成绑定到完整采集的 CPU 校正：

~~~bash
scripts/model.sh prepare-cpu-service \
  --profile-manifest <full-manifest> --light-manifest <light-manifest> \
  --tp-size <actual-tp> --correct-recorder --output-dir <cpu-output>

scripts/model.sh export-hicache-operation-costs \
  --profile-manifest <full-manifest> --cpu-service-cost <cpu-output>/cpu_service.json \
  --page-size <measured-page-size> --output-dir <operation-output>
~~~

默认导出 load index、load submission 和 layer wait；可重复 `--operation` 选择等待、查询、写回或普通释放。
导出器使用正式 `hicache_calibration_export` 目标，多操作共用一次 trace 读取，各自在图副本上提取。
计时分解由 C++ 观测层完成，导出的是操作身份、工作变量、计时口径与实测微秒数；
Python 不再读取局部 DAG 的节点、边或内核名来重新解释测量。写入、释放和回载不导出供预测使用的图模板。
主机、worker、派发、设备及 residual 共用测量输出合同，未测口径仍缺失，不记为零。
只在成功后发布最终文件，失败不留下半成品；旧完整文件可保留，但命令仍失败。
`--diagnostics full` 保留提取上下文和不可用样本，不再生成局部图模板或另一套 layer-wait 聚合计时。
诊断不作为建模前置。具体参数和组内声明见建模说明与公开 `--help`。

这些文件是测量证据，须显式入库和离线训练，不交给目标执行器当结构模板。
普通释放不能借备份释放代替，当前选中的 source 策略决定导出口径，不由 target 指定。
默认提取正式窗口，只有 CPU 配对覆盖全部受控请求时才显式纳入准备请求；
不包含服务器启动，也不改变 base 预测或 target 评分窗口。

CPU 校正只扣有配对依据的区间，节点、间隔及 worker 读取同一校正表示，生成节点不再扣一次。
内部事件序列、同步次数或层工作不同不能直接配对。区间开销分摊是近似，不是逐指令归因；
未覆盖区间、调度残余和设备成本不因此清零，配对文件存在也不表示全部观测扰动已剥离。
探针写日志的区间若无法在源 DAG 中完整、唯一地归属，保留该次写出，不扣减其开销，也不补造源节点。
这只适用于观测器自身的可选校正；真实 CPU 操作的归属不完整仍是错误。

等待预取的空选择循环比较同 rank、同调用序列的单轮线程 CPU 中位数差，
不按循环序号配对，也不相减两次总时长。只接纳单请求、预取已发起未完成、没有另行子工作或同步的路径。
扣减后为负或不能匹配的轮次保持原值。源轮次差异保留，实际应用量不是 E2E 改善量。

可选 `HOOK_EMISSION_TIMING=1` 的原始 hook 写出测量只扣未与其他修正重叠的可测区间，
保留未测、亚微秒及重叠部分；可以覆盖 worker，不依赖 target。
历史同源对照只产生微秒级变化，未证明它是主要 E2E 误差；采样覆盖的改善仍待新实采，不能补造旧数据。

### 来源、去重与范围

数据库按原始采集身份和操作位置去重，来源别名及 full/light 依赖另外保存。
工作变量按数值比较，JSON 的整数与等值小数写法不会制造新样本或重导入冲突；不使用近似容差合并不同测量。
留出任一同源别名会排除该采集及依赖它的测量，但共享轻量参考不把两个真实采集合成同一个来源。
不使用 SHA、内部版本号或冻结副本。修正提取规则须显式替换派生观测并重训，旧成本包不自动改变。

独立测量选部署、排除整轮 base/评分，再按声明拟合；同配置历史数据可用，但必须说明配置重合，
不能因此声称未见配置泛化。来源包含 I/O 选择考察的数据，实际系数贡献与仅被考察的观测分别报告。
新的补采严格串行、全组共享，失败和重试也计入累计预算，具体机制在建模说明第 6 节。

## 7. 怎么解释成本报告

| 报告 | 说明什么 | 不说明什么 |
| --- | --- | --- |
| `training_fit`、原始波动与合并工作组 | 训练点拟合、采集变化和省略变量 | 留出误差、置信区间或泛化保证 |
| `feature_ranges`、已测组合与允许外推 | 声明坐标的证据范围 | 未记录几何、所有组合或 I/O 全域覆盖 |
| `independent_operator_queries` | 按操作/实现/口径汇总查询、外推与未见组合 | 节点数、执行服务总量或 HTTP |
| `executed_cost_sources` | 实际执行有效节点的绑定来源、耗时、gap 和 ready delay | 完整来源归属或关键路径贡献 |
| I/O 参数与 `io_training.measurement_support` | 实际系数来源、被考察数据、页宽及调用规模 | 默认倍率为一时的运行时覆盖 |

一次查询可供多个节点复用，查询耗时和不等于执行总量。
来源为 `unclassified` 不表示零成本或无模型，只表示归属统计尚未接入；
其中可包含存储、控制、保留片段和逻辑边界。统计含后台工作，gap/ready delay 也不自动来自节点的成本模型。

`ready`、`EXECUTED`、结构比较完成和精度 PASS 是四件事。
完整 HTTP 对照正常运行正式窗口，不剔除 gap、未归属成本或 Prefill/Decode；
base 配对只校正自身采集，不从 target 真值中扣误差。实际覆盖及均值/单次波动限制见工作进展。

## 8. 保留的历史静态诊断

以下说明相容静态输入，不是现行训练接口，不恢复旧 base 优先拟合与邻近模板选择规则。
历史成绩由验收文档记录，不作为此次精简后代码的运行证据。

静态控制标量保留 Prefetch terminal commit、必要 state check 和 Load admission；
零载荷仍可有 control，不能因 oracle 数值为零删除依赖节点。
service 实际执行、完整 batch 发布和前台等待分开：
wait_complete 依赖完整服务，best_effort 不等，未完成 timeout 只依赖配置 deadline 门；
后台未完成 I/O 仍占资源，不把完整 service 全压到前台。

历史阶段聚合输入：

~~~text
Prefill common/collective = measured curve(new_tokens)
Prefill attention = a + b × new_tokens
                  + c × new_tokens × (context_tokens + new_tokens/2)

Decode effective_pages = ceil(context_tokens / min(target_page_size, base_kernel_page_tokens))
Decode attention/iteration = a + b × context_tokens + c × effective_pages
Decode collective/iteration = observed median
~~~

历史 Prefill 锚点取中位数、向前单调夹取并线性插值，注意力系数使用非负最小二乘。
Decode 次数沿用请求语义；明确没有迭代时参数为 null，不是零成本。
这些旧模型不为现行变化操作兜底，也不能吸收 HiCache I/O/control 或 residual gap。
`attention_token_pairs = n × (context + n/2)` 是二次工作代理，不是精确因果配对数；
现行 Decode 的零起始第 i 轮上下文为 `prompt+i+1`，页数不是 kernel tile 数。

显式 oracle-cost replay 在独立评分后替换成本，仅诊断绑定与敏感性。
先核对相同成本注入恒等性，不通过 target 成本改变结构或参数；静态重建 gap 缺少源坐标时不能叠加源 CPU 校正。
它不是预测成绩、正常 HTTP 精度或完整 target 时序同构证明。入口与当前可回放范围见建模和验收文档。
