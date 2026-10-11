# Modeling 开发与使用

本项目从一个 base 的真实采集构建 source DAG，根据 target 配置推演缓存行为、工作量和耗时。
用户的正式路径是：

~~~text
base profile + 一组 target 配置
  → prepare-hicache：使用独立成本包，显式开启后才共享补采，生成目标图并预测
  → evaluate：有 target 实测时独立评分
~~~

预测不需要 target trace。参数原理见 [HiCache 数值模型](hicache_io_cost_model.md)，
验收口径见 [验证说明](validation/hicache_validation.md)，当前结果及数据资产见 [工作进展](work_progress.md)。

10-10 已清理不再使用的历史实验资产。本页旧证据路径仅描述当时验证，不保证仍可运行；
现存采集、校准、静态回放输入及最新稳定对照以工作进展的[数据资产](work_progress.md#数据资产)为准。

本轮三阶段重构已交付，完成设备流嵌套插入修正后的 60/60 格预测，使用同一份独立成本包；
完整 HTTP、结构比较及源回放已报告。完整结果以本轮报告为准，不拼接旧格；剩余近似与未知范围明确保留。
执行完成不等于成本覆盖充分或精度达标。最新结果和资产以[工作进展](work_progress.md)为准，
实施过程保留在[计划](tmp/three_stage_modeling_refactor_plan_20261002.md)和[日志](tmp/three_stage_modeling_refactor_log_20261002.md)，不在本页重复逐轮状态。

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
`CostMigration` 表达对应源操作及迁移依据，不把没有源节点的新操作伪装成迁移。
成本提供者只读取工作描述并返回各计时口径的数值、来源和覆盖范围；执行层填入已生成的槽位。
操作增删、资源位置和等待关系仍由结构侧负责，经过正式 mutation 接口及组件归属检查。

`structure/tensor_program` 是 Attention、metadata、普通计算与 allocator 共用的操作程序。
它记录子操作、前驱、执行方式和后端子启动；`runtime/tensor_program` 绑定资源与成本。
设备消费者等待设备完成，纯 CPU 消费者等待主机返回；异步设备依赖不提前阻塞 CPU 提交。
Attention 的两次逐层缓存检查也只在 K/V 主机提交之后发生：未启用回载时直接返回，启用时向设备流
提交 load event wait。它们都不要求 CPU 先等 K/V 写入完成；设备侧先后关系由 stream 和事件依赖保留。
阻塞读回可以先准备参数，到复制入口才等待设备，不能把整个调用都放到等待之后。
声明了多个子启动时分别计费，不能再叠加父操作的设备总时长。

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
HTTP 结束后的已准入 forward 尾部按因果关系保留，规则见第 7 节。
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

加载索引、提交、释放和控制段分别查询 CPU、worker、dispatch、设备及 residual 口径。
同步前准备与等待后的同步服务分开，无法从真实等待中分离的成本不入库；已可测的其他片段可独立保留。
模型缺失时返回明确缺口，不用整段墙钟或零值补齐。各类公式及近似见成本模型文档。

### 数据与报告的边界

base 仅提供对应且未变化的成本，变化或新增操作使用独立数据库离线训练的成本包。
预测不打开数据库、不自动导入 base、不读取 target；共享补采默认关闭。
同一部署共用的成本包必须提前留出整轮 base 和实际评分采集，完整/轻量配对任一侧被留出时，
整份测量不得参与训练。别名与来源用原始采集身份表达，不用摘要或内部版本号充当有效性证明。

独立测量可用 `--include-setup-requests` 扩展到全部有时间戳的受控请求，但配对校正必须覆盖同一范围。
它不改变预测或评分窗口，也不授权将窗口外未经校正的区间标为正常 CPU 成本。
部署声明包含框架、模型/TP、dtype/量化、graph/编译与资源环境；未知或冲突条件不能假定兼容。

实验显式开启 `--experiment-reports`，独立 `evaluate` 读取结构 trace 和正常 HTTP 测量，产生三阶段报告。
目标结构提取使用评分命令的 `--model-run-jobs` 上限并行；默认仍为一，各目标输出独立。
可显式提供少量代表 target 的结构 profile；只比较这些输入对应的预测，其他格标为未请求，
报告分别列出结构实测数、已比较格数与未评价格数。完整 HTTP 评分仍要求所有选中格及正常真值齐备，
不能把代表格检查称为全矩阵结构验证。
这一步只在 modeling 环境构图，不启动推理或设备采集，不能据此放宽 profiling 串行要求。
源回放对照同次 profile；完整性能对照预选的正常 HTTP 样本；结构比较使用实际展开图，不先填 target 成本。
结构报告分开列出对应覆盖、工作量、资源分组和可达性差异，不合成万能相似度，也不把未观测项算作通过。
HiCache effect 包括“不需要执行”的决策；Decode 机会由目标侧分配调用、prompt 身份和后续 forward 独立关联，
不是复制预测结果。缺关联单列 target_observation_blockers，不能当作没有工作。

原生算子和重建调用分区比较，未对应的工作、逻辑等待及其他合成节点分别计数。
比较可穿过中间节点，但这不证明中间 CPU/control、物理资源身份或所有底层操作已经对应。
同族重复调用重排、未知标签、内部连接及完整原生结构仍有覆盖限制，计数或连接特征一致不等于全图同构。
默认保留紧凑摘要；显式 diagnostics full 才保留详细问题清单。重新运行同一目录时清理旧报告，
防止关掉报告后误读旧文件；dry-run 不执行这种清理，原始采集不受影响。
可达性差异始终完整计数；详细报告的 missing/extra 关系各保留至多 64 个示例，
并注明示例是否截断。这只是限制报告大小，不减少比较范围，也不把差异当作通过。

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

### 代码分工

同一份信息尽量只有一个所有者；观察到的源执行与预测出的目标执行不是两份需要互相同步的状态。

| 信息 | 所有者 | 不负责什么 |
| --- | --- | --- |
| 原始事实、源节点和观测区间 | source DAG、fact、source_dag_index | 不决定 target 采用哪个分支 |
| 源 I/O 操作、工作量及其归属 | io_operation_ledger、attribution | 不把源耗时当成目标完成时刻 |
| 目标缓存状态、操作数量与生命周期 | model 状态机与 runtime | 不按有没有源模板决定合法分支是否存在 |
| 源 host 调用的成本与依赖片段 | runtime/write_host | 不负责目标图落图 |
| 独立控制成本的读取、转换和资源绑定 | runtime/control_calibration、write_resources | 不读取 target trace 或决定目标工作量 |
| 目标提交、等待与传输计划 | runtime/load_execution 等操作生成器 | 不在生成时重新解析校准文件 |
| 共享资源顺序、操作展开和依赖连接 | runtime/write_expansion、FutureDag | 不重新推导缓存策略 |
| 成本需求与共享实验的对应 | Python control_acquisition、物理采样器 | 不看 target 分数，也不维护另一份组状态 |
| 准备、尝试记账、恢复整组预测 | Python prepare | 不实现各实验的测量细节 |

源样本与独立测量仍有不同的读取方式，但得到的 host 操作计划通过同一个落图入口执行。
资源顺序由操作计划统一给出：先是执行的 stream，再是等待的 stream；同一资源重复出现时不能去重，
显式 event completion 则另外绑定。main、worker、设备服务与 residual gap 也不能合并为一个成本。

当前全流程重构已完成统一验收：源片段替换结束后，再统一观察保留下来的 worker 队列，
加载、层等待、写完成确认与写入共用这份资源依据。各组件不在删除中间片段后分别认定最终队列。
加载和写入的 stream 插入端点也在此时按同一份最终源图刷新。加载提交、分配入口与空加载分支
共用入口规划，先通过完整 mutation 应用改边及成本替换，再登记执行回调。
非活跃层等待也先规划整组插入再应用；allocator preparation 的源坐标在创建时提供，不再事后修补。
候选分支的资源不足仍在目标实际需要时报告；已确定执行的调用则立即要求可用资源。
本轮进展与统一检查安排见 [重构计划](tmp/end_to_end_refactor_plan_20260927.md)。

### 输入来源

profiling 与 modeling 只通过 `profile_manifest.json` 交接，不扫描目录猜输入。
运行期间不要修改已选 profile。

一个 base 组可以包含多个 workload，但每个 workload 只选一份 base profile；
这些 profiles 必须属于同一个 base HiCache 配置，模型、TP、后端和资源环境相容。
配置名、workload 名只是身份，不作为拟合变量，也不能代替实际配置与环境核对。

环境比较区分运行资源与采集控制：`HOOK_EMISSION_TIMING` 和
`SGLANG_NPU_PROFILER_SERIAL_EXPORT`、`SGLANG_HICACHE_IO_TIMING` 不作为资源匹配维度，原采集配置仍保留这些开关。
这不放宽模型、TP、设备或线程资源匹配，也不表示观测开销已全部消除；CPU 校正仍须绑定实际源采集。

target 文件只含 `{name?, hicache}`，示例见
[hicache_target_example.json](../configs/modeling/hicache_target_example.json)。
target 不得携带成本、观测或评分字段。页大小及策略在输入边界校验。
当前调度器加载遵循 HiRadixCache 的 10-token 最小 host 命中规则：更短的命中不发出分配或 H2D，
但提前返回和已有 prefix 拼接仍须计费。这条源码分支及新 probe 元数据尚待统一运行验证。
当前 schedule_policy 未传 `mem_quota`；其他调用方的 quota 限制不属于已经实现的执行路径。
省略预取策略时，输入解析统一补为 `timeout`，不继承 base 的策略；成本需求检查与执行使用同一个值。

完整 HTTP 评分需要 workload 报告声明正式窗口。只有 bench-serving 总时长、没有 trace 起止坐标时，
不能据此裁剪 trace。普通 build-dag 可保留完整 trace；需要正式窗口的构模或评分会报告缺失。

## 3. 选择命令

所有命令从仓库根目录运行：

| 需求 | 命令 |
| --- | --- |
| 一个 base 预测一组 target | `scripts/model.sh prepare-hicache --group <group.json>` |
| 通用 source DAG 构建、仿真 | `scripts/model.sh build-dag` |
| 已完成预测的独立评分 | `scripts/model.sh evaluate` |
| 导入独立测量并离线训练 | `scripts/model.sh cost-db import/train` |
| 已有成本包直接预测 | `scripts/model.sh predict` |

`prepare-cpu-service`、`export-hicache-operation-costs` 和 `calibrate-hicache` 是同一流程的可独立排查阶段，
不是用户必须手工串联的另一套流程。具体参数用对应命令的 `--help` 查询。

`prepare-hicache` 在宿主机协调容器；设备采集进入框架容器，构模与预测进入 modeling 容器。
一次批量预测共用一个 modeling 容器，但整个 prepare 不保证只启动一次容器。

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

### 最小起点

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

先检查输入，再执行：

~~~bash
scripts/model.sh prepare-hicache --group <group.json> --dry-run
scripts/model.sh prepare-hicache --group <group.json>
~~~

最小配置使用已有独立成本包，不启动自动成本补采。成本包仍须满足部署条件和目标操作的实际需求，
缺少模型或测量覆盖时明确报告缺口，不能保证任意成本包都能完成预测。
普通预测不恢复物理或控制校准文件；这些输入只在显式共享补采需要使用时才解析与准入。

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
| 缺 base profile | `base_capture_budget` | `profile_suite`、base/workload 选择、`forced_token_bundle` |
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
`metadata.config_id/workload_id` 标明要采集的 base 与 workload。已有显式 token plan 时只回放一次；
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
首次准备会自动从 base 部署读取 KV 几何和存储范围，不要求声明 `physical_capture`；这不启动测量，
也不提供任何测得的成本。宿主机无法读取 base 模型的 config.json 时，报告具体路径及已有校准替代方式。
修改采样次数、页域或采样绑核不会使这份部署信息失效。
实际 I/O 补采仍须声明页域、适用放置和预算；没有页域时不会自动沿用账本中的旧 I/O 测量。
仅补锁定淘汰候选 CPU 原语时声明 `physical_capture.eviction_cpu` 的 heap_sizes、batch、repeats，
不要求 DMA 页域，逻辑 I/O 字节为零。原理及外推限制见[锁定候选成本](hicache_io_cost_model.md#61-淘汰时跳过锁定候选的-cpu-成本)。
另一组已有适用测量时，可在 `physical_capture.eviction_cpu.reuse_ledger` 显式填写其
`physical_capture_ledger.json` 路径。只复用环境、CPU 绑定和采样域相符的成功测量；
来源不相符则报告缺口，不静默换成新采集。旧账本及耗用留在原处，新组引用原始报告，不复制或清零历史用量。

### CPU 采集开销校正

CPU 成本校正比较同一个 base 的完整采集与轻量回放，不拿 target 正常耗时拟合 base。
新采集的 base 轻量回放与共享控制校准的轻量回放使用同一验收规则：manifest/workload 完成、
没有采集错误、forced-token replay 验证通过，且仅含 LD_PRELOAD 通道。命令成功不能替代这些条件；
CPU 配对所需计时文件继续由对应采集路径检查，失败尝试仍计入预算。
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

prepare 按顺序完成：取得 base → 准备 CPU 修正 → 提取观测并选择成本 → 检查和补充共享缺口 → 发布模型 → 组预测。
物理服务、workload 和控制补采共用一个检查循环。一次执行暴露的多类 I/O 缺口按服务串行采样，
每项采样沿用前一项的报告，完成本批后统一重新构模、检查缺口；DMA 双向需求合并为一次实验。
存储采样底层也只执行指定的一种服务，不再保留整包串行采样分支；原始观测在拟合前保存，
拟合失败仍可检查测量结果，采样成功或失败都会关闭本次 worker。
若中途因预算等原因停止，已完成的服务仍保留并进入构模，不把整个批次算作全部完成。
同一次准备不重复尝试同一服务。共享模型更新后整组重新预测，不沿用旧模型下的成功格结果。
已有匹配的物理测量可直接复用，不要求新增采集预算；预算和活动采集检查只约束新采集的启动。
后续服务补采会扩展共享报告；只要测量条件未变、当前报告仍包含原测量，报告路径变化不触发重采。
换成不包含原测量的独立报告时，不能仅凭旧账本记录宣称当前成本已补齐。
物理成本不再只在流程开头检查；后续观测暴露的物理缺口也进入同一条预算受限的补采路径。
成本输入统一检查来源角色及 manifest 唯一性；重复观测报错，真实重复测量使用各次采集自己的 manifest。
数值参数保存在显式离线训练的成本包中，来源和范围随包保存，不再由 base 生成临时模型。
默认准备只整理源输入并消费所选成本包，不读取数据库或自动训练。显式开启自动补库并声明数据库和训练请求后，
若发现可补采的成本缺口，先用已有独立数据库训练一次并重跑整组，避免因为所选成本包较旧而重复采集。
这一步不启动设备、也不消耗采集预算，沿用声明的模型形式与留出名单，并补充排除当前组 base。
仍有缺口时才进入预算内共享采集；新测量入库后更新同一成本包并重跑整组。
已有数据库的首次刷新每次调用最多一次，`database_refresh` 记录结果；它不证明成本覆盖完整，
也不允许根据评分重新拟合。工作描述缺变量或没有模型声明时，仍先报告修正需求，不盲目训练或采集。
I/O 服务可以缺席，成本包只保存已有适用依据的服务，不填零。包的就绪状态不保证每个 target 都能执行；
实际操作缺成本时，失败格 HTTP 为 null，具体需求进入 `prediction.missing_costs`。
组流程按需求安排预算内共享补采、离线训练和整组重新预测，未使用的服务不因缺席而强制采集。
离线训练若只完成部分声明模型，仍发布本次有效的部分成本包并重新执行整组；实际执行暴露下一项缺口，
不会因一个尚未用到的模型缺数据而提前停止。训练进程失败且没有新包时保留原包并停止本次补采流程，
不把旧包冒充训练成果；新预测启动失败时也不沿用上次缺口触发补采。
分配器准备成本与 Prefill/Decode 共用计算补采候选；这只表示安排一次共享尝试，不保证覆盖
首次加载、全部特化、页大小或编译路径。已有实验仍无法覆盖时保留缺口，不反复采相同工作或填零。
I/O 补采直接采用执行器报告的具体服务需求，不要求它同时出现在构模前的缺口摘要中；
采样器映射只在物理采集模块维护，构模前的覆盖报告用于解释已有依据，不再充当第二道补采门槛。
同轮发现的可补采缺口串行合并处理，再统一构模和预测，不为每一种成本单独重跑整组。
预算不足只拒绝当前实验，后续不同需求仍分别检查剩余限额；采集失败等其他原因停止后续采集。
若本轮已获得新数据，先用这些数据构模并预测，再报告剩余缺口和停止原因。
已有释放校准仍不适用时保留其缺口，不重采相同声明，也不阻断其他 target 的独立校准需求。
最终采集用量从账本汇总，包含预测阶段触发的补采及失败尝试。
group_summary.json 的 captures 按顺序记录本次准备中的共享补采结果，并用 operation 标明 phase、I/O 或控制需求；
命令结束时逐项显示状态和停止原因，不再只保留最后一项。它是本次流程摘要，不是另一套预算账本；
跨次运行的失败、重试和已完成测量仍以原采集账本为准。
dry-run 和 calibration-only 不执行目标，也不训练成本模型，因此不能证明服务需求、成本覆盖或完整 E2E 可用。
正式预测前只准备源输入及声明的同源 CPU 校正，不在线拟合 phase，也不保留另一套 I/O 服务补采循环。
缺少部署信息时先解决输入声明；不能靠运行传输采样替代模型结构与资源配置。
已有成本包是明确选择的预测输入，目录中旧的执行报告则不能证明本次预测成功；以本次退出状态和摘要为准。

当前补采能力有明确边界：

- 仅缺计算参数时，使用常驻前缀和不同长度续写；I/O 缺口按实际服务选择独立测量，
  不再通过通用 workload 规划器生成整套缓存压力实验。控制 CPU 缺口仍使用有针对性的配对 workload。
- 预取等待成本在目标实际进入活跃分支时检查；只发生本地提前返回时，不要求完整等待程序，仍必须有本地返回成本。
  已删除构模前的预取程序预扫描及内部 `--observe-prefetch-policy` 参数，需求不再由两套逻辑判断。
  执行报告缺口后，可安排 token 记录、完整回放、轻量回放、CPU 校正及控制导出。三次运行都计费，不将首次采集与强制回放直接相减。
  token 记录阶段关闭 profiler，仅保留请求执行和 HiCache 状态检查；其结果提供回放 token，不作为成本观测。
  完整回放保留原采集配置，轻量回放只用 LD_PRELOAD；两遍回放才用于同源 CPU 校正。
  正式构模不再估计历史静态路径使用的 commit、state-check、load admission 标量，也不报告其缺口或触发补采。
  历史模型参数的读取及静态回放保留；现行执行式预测继续使用操作级 CPU 成本，缺失时不填零。
  这条流程只导出所缺的 prefetch_wait，不要求同时成功导出 write_host；已有写入校准保持不变，写入成本仍可显式导出和声明。
- 锁定候选 CPU 缺口可安排共享原语测量；获得新测量后重建并重试整组一次，不原样重试已有证据。
- 写入、淘汰、局部返回等需求检查仍不完整。执行失败报告的 missing_costs 可能只是遇到的第一个缺口，并非完整需求清单。
- 物理、workload 或 control 实验完成后重新检查成本。如果缺项清单原样保留，返回 experiment_exhausted，
  不再重复生成或复用同一实验尝试补洞；已有采集和累计用量保留，需检查缺失的变量或执行分支。
  公开入口同时打印各阶段状态、预算限项、停止原因及采样器限制；完整记录仍见 group_summary.json。
- 首次没有物理报告时，先从 base 部署读取 KV 几何、批大小和资源放置，写入 platform_inputs.json。
  该文件不含实测系数；预测报告缺项后，显式共享补采按需求选择 DMA、预取、覆盖写或新页写入，
  参数由独立数据离线训练，不从当前 base 辨识。
  读取这些部署信息不要求解析 CPU 采样绑核，也不受采样器暂不支持 membind 的限制；
  只有实际需要独立测量时才检查这些条件，不能把未运行的测量限制变成 base 成本建模的前置要求。
  零采集预算不启动测量进程。I/O 按实际执行需要补齐；存储写入的新旧页成本分别保留和按批次检查，
  纯新页不要求覆盖写成本，纯已有页不要求新页成本，混合批次必须具备两者。
  CPU/phase 操作也会在执行时暴露成本缺口；真实多缺口补采尚未全面验收，不能宣称任意组都已实现最小补采。
  采样器只处理明确请求的缺口：补 DMA 或写入不要求先有预取参数；没有需求时不启动测量。
  只补 Prefetch 阶段时不再要求 DMA 原始报告；从已有物理报告的 measurement_scope 读取设备和 NUMA 放置，
  与当前采样声明核对后只运行预取实验。缺失或不匹配时报告 existing_storage_placement_missing_or_different。
  独立命令统一使用 `--base-report <已有报告>`，通过 `--service prefetch|existing_write|new_write` 选择服务；
  DMA 和存储采样均从该报告读取 KV 几何、TP 设备及 NUMA 放置，不再重复传入模型配置、TP 或设备参数。
  首次可使用 prepare-hicache 生成的 platform_inputs.json（只有部署信息，没有测量系数）；
  存储命令的 `--model-config`、`--tensor-parallel-size`、`--kv-element-bytes` 已删除，
  DMA 命令的 `--devices`、`--numa-nodes`、`--layer-count`、`--kv-heads-per-rank`、`--head-dim`、`--element-bytes` 已删除。
  CPU 绑核、页大小和测量规模仍是采样参数；组入口在启动前核对报告与 base 的几何和设备放置。
  两个参数均为必填；不再保留默认一次采全的 `--service all`、`--runtime-dma-report` 和 `--skip-new-write` 分支。
  需要多项测量时由 prepare-hicache 按实际需求串行安排并合并报告；首次所需的部署信息也由该入口准备。
  已有完整物理报告仍可作为输入，不需要重新采集或转换资产。
  新页写入补充只运行运行时的 materialize-then-batch-set，不启动 DMA、覆盖写或预取测量；
  覆盖写补充只预填充并测量已有键写入，不运行 DMA、预取或新页队列实验，保留已有新页成本参数。
  DMA 曲线缺失时单独调用 `calibrate-hicache runtime-dma --base-report <已有报告>`，不启动存储测量。
  DMA 补采按缺口选择 H2D、D2H 或两者，采样操作和逻辑 I/O 预算使用同一方向集合；
  独立命令可用 `--directions host_to_device` 或 `--directions device_to_host`，不指定时仍测双向。
  合并报告只填缺失曲线，不覆盖已有有效参数。采集前核对 KV 几何、设备及 NUMA 放置。
  两个操作规模来自明确的 payload 或 base 输入规模，页大小域来自 physical_capture 声明，不读取 target 真值。
  成功后保留其他服务参数与来源，按累计物理预算计费。组准备重新检查成本；同一缺口不变时停止，不重复测量。
  预取补采的工作量只由声明的页大小、批大小和重复次数决定，不读取 token plan 或准备 DMA/新页写入规模。
  已完成原语按采样规模、几何、设备/CPU/NUMA 放置和环境复用，不把生成命令的拼写当成有效性证明；
  平台测量的适用性不依赖 base token 文件路径，调整 CLI 不会单独触发重采。
  调整 CPU 淘汰实验的样本规模或预算不会作废已有 I/O 报告；I/O 页大小域、设备和 CPU 放置仍参与自动复用判断。

补采使用 base 环境和输入 token，不读取 target trace 或误差。普通模板 workload 报告中的实际 token、
显式 token 模板或 base 自身回放计划可以作为输入；旧文本采集未记录 token 时需补充自身输入，
不从其他 cell 借用，也不编造词表编号。

先看 group_summary.json 的 status 和缺口：

| 状态 | 含义与下一步 |
| --- | --- |
| predicted | 组内预测已完成；查看成本覆盖，再独立评分 |
| ready_for_model_build | 检查/准备已够，可以执行正式 prepare |
| needs_calibration / needs_control_calibration | 已有待执行计划，常见于 dry-run；查看所需预算 |
| budget_exhausted | 预算不足，不会自动越预算启动 |
| cpu_capture_budget_exhausted | CPU 轻量回放预算不足；查看 cpu_service.limits 与 capture_usage，调整预算后重新 prepare |
| cpu_capture_failed | CPU 回放命令未完成；查看 cpu_service.attempt 的失败原因与 command_log，失败用量仍计入预算 |
| capture_incomplete | 存在未收尾记录；提示包含账本路径、记录状态和 Docker 当前查询结果，先核对再恢复 |
| experiment_exhausted | 所选实验已做但证据仍不足，重复同一实验没有依据 |
| no_suitable_experiment | 当前没有能解决这些缺口的实验 |
| prediction_incomplete / prediction_failed | 查看 prediction.missing_costs、失败格日志及实际执行结果 |

采集失败保留其原状态。base、CPU 回放、物理和共享 workload 补采统一在对应结果的 `attempt` 中
保留本次采集记录；命令行直接显示已有的失败原因、退出码和 `command_log`，不必再翻多个报告找日志。
物理实验被预算拒绝时，限制项也保存在 `attempt.limits`；尚未进入实验的准备限制仍在计划顶层。
遇到未完成采集记录时先核对进程、容器和日志，不因记录旧就删除账本重启。
入口只查询该记录声明的容器，不把账本中的 running 当作活进程证据。容器不存在或查询失败仍不自动重试：
它们不能证明产物有效、清理完成或用量已结算；当前还需要人工确认未收尾记录，不宣称已实现无人工中断恢复。
命令失败、预算超时或中断后，损坏的结果文件作为 artifact_error 附加记录，不覆盖原始停止原因；
只有命令已完成但产物验证失败时，才使用 invalid_capture_output。没有可读用量证据时仍按预留预算计费。
CPU 补采遇到上述停止状态时保留已准备的 CPU 成本文件，不启动后续构模或预测，正式命令返回非零。
省略 light_manifest 时，先查找同一 base 和回放输入下的已完成轻量采集；正式运行和 dry-run 均可复用，
不要求新增采集预算。只有没有可复用结果时才要求预算；dry-run 不创建采集目录或启动回放。
这与损坏的 trace/manifest 不同：无效采集数据及意外异常仍报错，不当成普通预算不足。
已完成阶段可复用；异常保留失败阶段和本次尝试耗时，强制杀进程或写盘失败可能来不及保存终态。
采集墙钟用量统一从登记尝试开始，覆盖容器运行、清理及产物读取和验证；不是仅计推理运行时间。
结果验证失败也会计入本次用量，后续补采使用更新后的累计预算。历史账本保留原记录，不倒算旧采集耗时。
共享控制成本按实际完整/轻量采集对保存，不按可重复使用的实验点名称覆盖。恢复时复用原始采集，
所需操作成本由当前导出实现重新生成，不因文件存在而跳过。导出完成后原子替换对应成本文件；失败保留原文件，
但本次准备仍报错，不将旧文件当作本次成功结果。任一采集改变后使用另一配对目录，旧配对保留；
成功导出后才更新组内的操作引用。重新导出不启动设备采集，也不增加采集账本用量。
旧位置的已声明成本仍可使用；重新进入自动准备时使用采集对目录，可能重做派生计算，但不因此重采设备数据。
采集命令成功后若结果处理抛出异常，共享账本会将本次标为 `invalid_capture_output`，不能作为成功阶段复用；
命令尚未完成时的异常标为 `interrupted`，已有超时、失败或清理不完整状态不被后续异常覆盖。

prepare_wall_seconds 包含本次准备、采集、构模和预测，不能再加一次分项耗时。
各采集账本是累计用量；旧资产首次采集成本可能未知，不能据此宣称完整冷启动成本。

## 7. 独立成本库、预测与结果

部署比较不包含纯观测开关（线程 CPU、scheduler/host/layer/prefetch 计时和日志写入计时），
这些设置仍保留在原采集配置中。CPU 亲和性、设备选择及执行开关继续参与适用性比较；
去掉观测开关并不证明采集开销已消除，CPU 样本仍需独立满足配对校正的准入要求。

贪心选 token 在模型 forward 返回后执行，不在 Torch 的 forward 标记内。
源阶段归属使用同时包含 forward 和采样的、带请求身份的真实 batch 调用；只有唯一对应时，
才把采样包络内实际提交的工作归给该轮，不靠最近时间或设备完成顺序猜测。
最后一个 rank 的 batch 可能在 HTTP 响应后仍有尾部工作：构图保留这个已开始调用的完整边界、
内部描述及提交依赖，标为 causal tail，HTTP 正式窗口不延长。不保留窗口后新开始的普通 batch。
结构替换也按这次完整 batch 处理：逐层等待按所属 forward 的开始时间准入，计算调用保留跨界部分和
已明确归属的尾部上下文，不能只替换窗口内的生产者而留下窗口后的旧消费者。成本仍按实际操作绑定，
保留上下文不等于将它的全部耗时计入 HTTP。

缓存确认同样按完整调用处理。HTTP 结束前已开始的 write/load 检查保留内部 MIN、确认返回和
后台通信工作，即使这些步骤晚于 HTTP 返回；不保留此后新开始的空闲轮询，也不延长计时窗口。
Gloo worker 没有直接的提交关联 ID 时，只接纳完整同步调用内同进程、同操作的唯一 worker，
缺失或重复仍报告错误。窗口裁剪不能把一条原本完整的确认调用变成缺数据或零成本。
加载/写入检查的外层准备也可能跨过正式起点。如果两侧 MIN 的采样入口都在窗口内，且有显式
空闲 barrier 或完整观测证明窗口前只做了空队列准备，可以保留窗口内的确认部分；执行时仍要求
目标相应队列为空，否则报错。源队列计数仅证明被裁前缀的性质，不作为目标确认数量。

历史测量通过显式导入进入成本库。选用的 profile 必须独立于本轮 base 和评分采集；
有同源 CPU 完整/轻量配对时声明校正文件，没有校正的 CPU 观测不作为正常服务耗时：

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

导入和训练均不启动设备采集，也不读取评分结果。重复导入按原始采集与测量位置去重，
不同采集中的相同工作点仍保留为独立测量。物理 I/O 或已导出的控制测量通过 `--evidence` 导入；
不能把测量文档路径当成另一次 runtime profile。

有配对 CPU 校正的 profile 导入同时提取支持布局下的写入提交成本，与独立写入导出共用提取规则。
CPU 服务、残余间隔、worker、派发和设备事件分别保留，传输载荷耗时仍由 I/O 模型处理，不能重复计费。
提取时使用独立工作图，不改变原始阶段观测或把测得的模板交给目标结构生成；未测分项仍保持缺失。
旧的派生入库结果不会自动更新：明确重新提取时使用 `--replace-captures`，随后离线重训成本包。

回载索引的专用导出另从唯一成功的分配调用开始，测量其设备索引与后续逐节点 clone。
同一次回载此前可以有分配失败和驱逐重试；这些工作不在该测量范围内，仍由各自的成本处理。
多次成功分配、归属不清或混入其他设备工作时，样本继续保持不可用，不将整次回载墙钟当作索引服务。

`cost-db inspect --database <cost-database.sqlite> --output <coverage.json>` 显示各操作和计时口径的
样本数、独立采集数、不同工作描述数，以及每个变量的范围和实际出现次数。
变量出现次数少于该组样本数，说明部分测量缺少该变量；只出现一个值的变量会单列。
这些信息用于选择有依据的训练变量，不能用“样本很多”代替“变量充分变化”，也不能据范围覆盖
宣称组合都已测过或系数可辨识。数据库检查不读取 target 或改变模型。

训练参数另附 `training_fit`：按同次采集、同工作点中位数计算未取整响应的 MAE、最大绝对误差和 WAPE，
同时列出参与拟合的工作点数、参数数及剩余自由度。观测总量为零时 WAPE 为 null。
这是训练内诊断，不是留出预测精度或置信区间；两个点拟合两个参数即使残差为零，也不证明泛化。
该报告不改变模型参数、覆盖范围或缺口状态，也不触发自动换公式。

修正观测提取规则后，可在 `cost-db import` 上显式加 `--replace-captures`，重新导入指定采集的
完整观测。它在同一个事务中替换这些 capture 的观测和证据，删除此次提取已不再接纳的旧样本；
中途失败则回滚。原始 trace 不删除，已登记的来源别名仍保留，避免替换后绕过数据隔离。
该选项不适合增量导入某一个控制原语：必须提供该 capture 希望保留的全部派生数据。
数据库只维护测量和来源，训练参数只保存在导出的成本包，不再写一份数据库模型副本。
替换测量后需显式重新训练并导出成本包；已有包不被自动改写，导入摘要会提醒重新训练。
默认导入和自动补库均不隐式执行替换。

训练请求明确 `deployment`、`models` 以及 `excluded_captures`/`excluded_manifests`。
每个模型声明 `operation`、`implementation`、`timing_scope`、`features` 和 `form`；
支持常数、线性或单变量分段线性，是否允许外推由请求明确声明。
`allow_extrapolation=true` 允许所有声明坐标外推；需要只外推某个变量时，保持它为 false，
使用 `extrapolation_features` 列出允许的坐标，其他几何条件仍必须处于实测范围。
超范围查询仍会进入报告，不因此成为已测覆盖；分段线性模型不允许外推。
自动补采在启动设备前使用同一训练声明解析规则；不支持的形式先报输入错误，
缺声明、缺目标变量和缺实际测量分别处理，不靠重新采集修复声明错误。
可选 `coverage_features` 声明只用于适用范围的数值变量，默认空列表；它们与 `features` 都必须存在于
独立样本和目标工作描述中，但只有 `features` 进入回归公式。例如设备计算可用 `features=["elements"]`
拟合元素数，同时用 `coverage_features=["batch_size", "page_size_tokens"]` 记录已测范围；
固定提交开销也可以选择 `form="constant"`、`features=[]`，但保留这些适用范围条件。
是否使用常数仍需由实现机制解释，不能因为少变量就忽略实际工作差异。
可选 `observation_operations` 声明模型可使用的原始观测操作名称，默认只选 `operation` 自身。
只在已经证明后端操作等价时共享，例如普通 logits all-gather 的 Prefill/Decode 测量，
可以共同训练 `collective.all_gather`；同一 dtype、部署、计时范围及几何约束仍然有效。
这不会改写数据库中的原始名称或复制样本；导出模型记录这份选择，原始采集与配对留出规则照常执行。
调用者/worker 不会随设备模型自动合并，也不能借此把不同组件的整体误差相互抵消。
Linear 除 M/N/K 外，还提供 `multiply_accumulate_count=M*N*K`、输入 `M*K`、权重 `N*K`
和输出 `M*N` 的元素数。前者计一次乘加为一个 MAC，不是 FLOPs；后三者是逻辑张量大小，
不是实际显存流量。一个可解释的初始设备模型可以选 MAC 数作为拟合变量，以 M/N/K 与布局作
适用范围坐标；需要表达内存搬运时再按机制选择元素数，不能把所有相关变量一股脑加入造成秩亏。
小矩阵、分块切换和带宽瓶颈不保证服从同一条直线，特征存在不代表模型已经训练或足够准确。

TP 通信不能直接套用上述矩阵模型，也不能将 HCCL 观测持续时间无条件当作纯传输服务。
普通 NPU all-reduce 的真实入口是 `NpuCommunicator.all_reduce`，调用 `dist.all_reduce` 后返回原张量；
现已保留输入几何、参与者和层调用位置。普通路径已按目标工作量描述载荷，并用跨 rank 就绪约束表达等待；
完整协议和独立数据覆盖仍未完成，不能用阶段 token 回归补洞，也不能把源码接入视为已运行验证。
源观测导出和实验结构报告已接入通信描述审查：列出参与组、逻辑张量字节数、请求/轮次归属及实际提交的设备节点，
单列参与者矛盾、归属缺失和重复占有。没有探针记录时标为不可用，不视为通信覆盖通过。
普通 MLP 的归约位于行并行 Linear 的外层 forward，已补上该位置及层后处理的归属上下文；
不把归约包进矩阵服务时间。跨 rank 观测先按组和观测序号提出候选，再核对完整参与者集合、
请求/轮次、层位置、调用点及张量几何；缺 rank、重复 rank 或矛盾描述均保持未解析。
描述对应成立也不证明原图中的跨 rank 到达/完成边正确，报告分别标记这两个层面的证据。
对于描述能对应且设备工作构成明确单流链的调用，报告进一步核对 peer 完成到本地后继的显式 HCCL 边；
缺少边或无法确定完成边界分别列出。该检查只核对当前构图器声明的完成连接，不以缺少直接边断言没有
任何传递依赖，也不把完成连接等同于通信开始前的共同就绪条件。
通信源观测现已整理为有类型的普通调用：保留所属阶段、层、矩阵几何和按参与组排序的 peer 观测，
正式预测准备阶段检查实际 phase 通信节点是否有完整归属。报告中的捕获/初始化观测、直接边诊断不会单独
阻止预测；只有实际阶段节点缺少归属才进入此项输入缺口。
目标准入后，普通 Qwen3 的 attention 输出归约与 MLP 输出归约已生成独立载荷描述：Prefill 使用目标
计算 token 行数，Decode 使用对应轮次已有依据的填充行数，列宽来自同部署的 hidden width。
结构层计算逻辑字节数和参与者数，不按页大小缩放通信，也不读取成本库。分散布局、多请求、未知调用点、
空输入和变化后的 graph 调度仍明确缺少规则。服务需求使用独立的 `collective_completion_interval` 口径，不能填入
未经等待分离的原始 HCCL 持续时间。单通信设备节点的普通路径已接入目标服务替换：每个 rank 保留自己的
输入/提交前驱，所有参与者就绪后才开始各自的逻辑通信服务，原有输出消费者原子重接到新完成点。
成本在目标工作量确定后通过独立模型查询，不沿用构图器归一化过的 HCCL 时间；缺数据正常报成本缺口。
这是通信调用级的共同就绪近似，尚不描述协议分片与错峰到达的重叠；多设备启动/多流路径仍需单独展开。
归约及记录/等待事件的 worker 各自拆为启动与返回两段，设备在启动段结束后即可执行，与返回段重叠；
销毁事件保留一个无设备启动的 CPU 任务。一次原子变换保留各任务的唯一提交、FIFO 资源及结果消费者，
不把多个任务合成一个平均成本。源样本和目标查询共用任务工作描述：归约使用目标载荷，
事件任务只使用 `event_count=1`，不因 token/字节数增加而被强行缩放。
派发暂时沿用 base 经校正的调度残余，不沿用源队列等待，这一近似在输出中单独标明。
普通 queued eager 路径的调用线程按每次 enqueue 拆为准备段，逐段查询 CPU 服务和残余间隔；
每段结束只释放对应任务，后续准备与已提交 worker 可并行。源提取与目标生成共用该调用规则，
用真实输入/输出事件交接核对重复 record 的语义，不再导出或使用整调用 CPU 总量作为替代样本。
CPU 准备描述一个二维张量及参与者数量，归约 worker 保留 dtype 专化，事件任务不携带无关载荷变量。
只有同一进程组曾在本次设备通信结束前返回、且当前调用没有主机设备同步依赖时，才使用这条非阻塞规则；
不根据一个环境变量的当前值猜测进程组初始化时的阻塞选项。其他 debug/capture/事件生命周期路径仍缺规则。
设备 record/wait 节点已替换为独立计费的事件服务；原流顺序和记录到等待的同步边原子移交，
等待时间由依赖表达，不使用构图器固定同步耗时。record 使用原始测量，wait 仅接纳可证明
记录前驱在等待开始前已完成的样本，保留亚微秒精度。事件服务单独统计，不混入归约完成节点数。
事件连接仍由源图对应关系移交，不能称完整通信调用已经脱离源模板独立生成；
未知协议仍需结构规则，而不是补一个事件成本就算支持。上述已支持路径用于本轮统一预测。
结构报告通过显式源节点引用将阶段归属交给新通信完成节点，不再把已替换的通信全部计为 inactive。
这些节点标记为 `logical_collective_completion`，实现名为 `logical_all_reduce_completion`；
不沿用原 HCCL kernel 名称伪造原生协议一致。已有明确归属的通信移交单独的语义比较：
以 rank、请求、轮次、层、角色、调用点和参与组对应，比较 dtype 实现与逻辑载荷，
并从两侧实际图提取完成点间可达关系及资源共享分组。不以进程内组名、观测序号或节点编号跨 trace 配对。
缺少归属的通信仍留在原生比较中；报告单列移交数量。底层协议、事件及 CPU 内部依赖尚未完整比较，
不能据此声称完整通信结构一致；代表格已实际比较，本轮完整矩阵的比较仍在进行。
独立 CPU 校准的原始 rank 0 trace 已确认存在 `Enqueue/Dequeue@HcclAllreduce` worker 路径：
20260923_030441_base_cpu_profiled 中 correlation 396 的 dequeue 在 tid 1186，持续 210.74 微秒，
内部 `Node@launch`（item_id 为 `hcom_allReduce_`）持续 177.44 微秒；主线程同期继续提交普通计算。
这证明不能把整段 worker 时间放到调用线程，也不能将嵌套 launch 再重复计费。
同一采集首个 dequeue 为 435475.81 微秒，不能把这个首调用观测当作稳定提交常数；
这些只是历史 trace 的布局证据，不是新模型的成本参数或运行验收结果。
同一次独立采集的通信调用现已复用张量观测器导出可辨识的 CPU 提交、worker 启动/返回和派发分项；
各 rank 分别记录，整组完成区间仍只记录一次。没有配对 CPU 校正时保留 profiled 口径，不冒充正常 CPU 服务。
设备不能证明为无等待串行链时，仍单独判断 caller 和 worker 的测量资格；只禁用依赖该证明的
设备服务总量及阻塞返回样本。张量、层等待和 prefix-concat 的未校正派发间隔使用
`profiled_dispatch_interval`，不能作为正式 `dispatch_interval` 模型的样本。
同一独立 trace 的末次 HCCL（correlation 58696）之前有 record/wait 任务，之后还有两个 record、
wait 和 destroy 任务（58694–58700）。因此完整通信调用不是单一 worker 任务；
源侧已按真实 enqueue 和完整 FIFO 任务分解提交闭包，结构报告列出每项任务及其设备工作；
只按调用线程的提交顺序排列，不把 worker 的观测执行顺序当作目标调度规则。
通信的 record/wait/all-reduce/destroy 任务分别导出派发及可测 CPU 分段：单启动任务拆启动和返回，
无设备工作的任务使用 `worker_service` / `worker_residual_interval`，不虚构设备服务或启动点。
部分任务、直接提交混入队列、多个设备启动点等仍保留缺口，不把整段 worker 包络当作可用成本。
这些任务的 CPU 样本已独立训练并用于目标查询；源中未出现的未知任务分支仍需生成规则。
调用线程的分段采样和目标绑定已接通，整调用 caller 样本已移除；不能用完整调用的平均时间分摊填入各节点。
执行规则另以安装 wheel 所对应的 [ProcessGroupHCCL 实现](https://github.com/Ascend/pytorch/blob/94f8a8e6b523d7ba553e1b80d5b5248478391526/torch_npu/csrc/distributed/ProcessGroupHCCL.cpp)
核对：输入流向通信流交接，完成后再交还当前流；主机阻塞等待是单独条件，不能把 `dist.all_reduce`
的同步接口一概解释为主机等设备完成。该链接只定位上游规则依据，不是项目内部版本校验。
从通用观测器取得的原始 `device_service` 一律不作为通信样本，避免最短 rank 的 HCCL 恰未被归一化时漏入。
阶段调用替换已能保留来自区域外通信完成点的输入依赖：沿用计算时保持精确消费者，重新生成计算时只允许
将原流首操作的依赖交给同一资源上生成的首操作。内部消费者没有语义对应时仍报缺口，不把等待提前到 CPU
调用入口。删除原节点需要显式移交对应 HCCL 边，其他删除路径不会自动放行；通信服务先替换，再移交消费者。
逻辑张量大小不等于链路传输量；组内观测序号也不能独自证明跨 rank 配对。结构报告不导出成本；
独立成本导出另按原始前驱区间计算每个 rank 的输入就绪点，再测量最晚就绪到整组最后完成的剩余区间。
每组只产生一个样本，并保留 `arrival_span_us`（最晚与最早就绪之差）；预测在所有就绪节点完成后计算同一变量。
原始前驱缺失、合成前驱或前驱完成晚于本地通信开始时不产出样本，不使用归一化节点耗时补齐。
该区间可能包含通信调度和协议开销，不称纯网络传输时间；首轮仅用于已对齐的同机源时钟。
尚需核对实际有效样本、CPU 提交与多启动点规则，并在统一运行时验证新增探针、归属和替换后的依赖。
普通 RoPE 的 float32 表/BF16 输入路径已增加 eager 非融合展开：调用分派前的查表与
`forward_native` 内再次查表分别保留，随后按 Q/K 各自的转换、乘加和输出拼接生成操作。
不能因为第一次查表结果未被使用就删掉真实执行量。该分支要求没有位置表缓存且旋转函数未被编译；
可选融合核不存在或工作量未进入其分派范围时才生成普通操作。分派条件按框架的
`rows * query_width < 65535` 计算，不使用成本决定。目标跨入融合范围时明确报告缺少融合输入布局规则。
镜像内的可选函数实际解包三维 Q/K，不能仅凭模块可导入就认为二维输入也可执行。
与 `npu_mrope` 使用不同实现身份和独立成本。源算子包络仍须证明各步骤的归属：
不同 Torch schema 严格嵌套且提交完全相同设备节点时，保留外层操作，避免 stack 内部 cat 重复归属；
只有时间重叠、相同名称或完全重合边界不足以消除歧义，数量不一致不会被平均分摊消除。
可选融合核的大小阈值分派、缓存表路径及实际 native 内核展开尚未收口；本项只有源码接入，
没有采集、训练或运行证据，新增探针字段也须在正式重采时验证。
当前 `configs/experiments/hicache_manual_workload/profiling_full_dag_replay.json` 的正式配置显式关闭图回放。
这不是“任意 graph 模式已支持”的证据：实际图回放不会逐次进入 Python 算子，缺少逐调用包络时仍需
图捕获与执行关联，不能用新的成本样本替代该结构证据。
现有 source DAG 合并按 HCCL 名称和出现顺序关联 rank，为各 rank 的后续节点添加 peer 完成依赖，
并用组内最小非零持续时间作归一化。这只是减少等待重复计入的启发式：同名调用序列错位仍可能误配，
最短观测也未必等于纯传输成本，不能直接把归一化值送入独立成本数据库。零时长不能覆盖已找到的
正持续时间；该最小值选择已修正，并随本轮 15 份源输入完成静态回放。
整体回放误差不能单独证明这一归一化规则准确，也不能代替通信服务的独立测量。
额外范围变量不增加系数，也不改变同一采集内按拟合点取中位数的权重；缺少这些变量的样本不参与训练。
查询使用所有声明坐标的已测范围，超出时仍遵循 `allow_extrapolation`；分段线性保持禁止外推。
各坐标分别在范围内不代表组合已测量，Python 检查和 C++ 查询仍单列未见组合。
`omitted_observed_features` 指未参与拟合的变量，`unchecked_observed_features` 才是连范围也未声明的变量。
没有声明范围变量的常数模型仍缺少工作量域约束，不将其查询成功视为任意配置泛化的证明。
预取查询分别声明 `before_sample_interval`、`sample_to_min_interval`、`min_to_apply_interval`
和 `all_reduce_envelope_remainder` 四种口径；操作和实现为 `prefetch.query` / `sglang.storage_hit_min`。
测量描述记录 rank 和参与者数，不把请求或配置身份作为特征。同 TP 的首轮可明确选择常数模型，
它估计独立采集的典型区间，不意味着已经建模存储查询长度变化或消除调度、probe 开销。
当前查询结构仅覆盖同部署下单个完整 TP 组的 MIN；多 CP/TP 子组序列仍未实现。
预取检查的本地区间使用 `prefetch.check.<role>` / `sglang.prefetch_check` / `control_interval`，
记录每次实际调用的观测位置与 rank、参与者数。配对导出只记录走过的分支，不将未执行分支补零。
执行时按分支查询独立模型；同一请求中对应的本地提前返回可直接沿用 source，其余区间不从 base 拟合。
这些区间仍含残余等待，常数或有限特征模型的泛化能力需要单独验证。
独立等待校准中的 scheduler 轮次按共享进度通信身份对齐各 rank，导出所有可明确归属的完整间隔，
不再只取首个请求的第一轮。缺失确认边界或夹杂其他缓存工作的间隔记录为不可用，不补造成本；
训练仍先按采集取同工作点中位数，再平衡不同采集，不让轮询次数多的采集占据更大权重。

`implementation="*"` 为在所请求计时口径下已有样本的实现分别训练，不合并不同实现。
例如直接启动路径没有 queued-worker 样本时，不要求为该路径训练虚构的 worker 成本。
同操作的其他已知实现仍在 `wildcard_unmeasured_implementations` 中列出缺失口径及已有口径，
不等于它们已获得成本覆盖；目标执行若实际请求这些成本，仍报告缺口。若要求某个实现必须齐备，
训练声明使用它的明确名称。共享成本包的留出名单应覆盖本轮全部 base、
结构评分和正常 HTTP 评分采集，包括配对测量的轻量侧。

需要 HiCache I/O 时，以 `io_evidence` 选择适用的物理测量来源；I/O 训练同时使用同一部署下未被留出的历史 runtime 观测。
控制观测与其他算子观测统一按部署及留出名单选样训练，不再通过 `control_captures` 选择随包携带的原始文档。
`control_calibrations` 仍可声明需显式入库的采集文件，但预测只读取训练模型；已删除无 C++ 消费方的校准路径转发及文档重建。
缺少可辨识数据时输出 `missing_models`，不自动减少变量、切换公式或补零。
报告区分 `missing_operation`（操作/实现没有样本）、`missing_timing_scope`（操作已有样本但缺所需计时口径）
和 `missing_features`，同时列出 `available_timing_scopes`。通配选择在整个所请求口径均无样本时
仍列为训练缺口；已知但无该口径的具体实现另行列明，不把它们隐藏成只有一个 `*` 的记录。

当前异步操作的 worker 成本须声明四个独立模型：`worker_launch_service`、
`worker_launch_residual_interval`、`worker_return_service`、`worker_return_residual_interval`。
例如同部署下的逐层等待使用 `operation="layer_wait.submit"`、
`implementation="ascend.stream_wait_event"`；这四个口径可以明确采用 `form="constant"`、`features=[]`，
分别估计独立采集中的典型值。还需提交线程的 `cpu_service`、`residual_interval`，以及
`dispatch_interval`、`device_service`；`layer_wait.tail` 另有主线程服务和残余间隔模型。
常数形式只适用于明确固定部署和固定操作，不是所有张量操作的默认选项；随 shape 变化的工作须声明对应变量。
训练输出的 `omitted_observed_features` 列出没有进入模型的观测变量，
`collapsed_distinct_work_groups` 记录同一采集中因省略变量而被合并的不同工作组数。
省略变量可能有合理依据，但应由操作机制解释；覆盖范围只证明所选变量的覆盖，不证明未记录的 shape。
这两项是模型定义审查信息，不自动增加参数，也不以组数非零直接否定模型。
完整任务中确实没有 worker 返回段可提供零测量，缺少完整任务观测则不能训练一个零模型。
自动补采启动前还会核对训练请求是否声明了所需操作、实现和计时口径（实现可以使用 `*`）。
缺少模型定义的需求列在组摘要 `undeclared_operator_models`，不为它启动注定不会产生模型的采集；
若全部剩余需求均属此类，状态为 `cost_model_definitions_required`。同组其他已声明需求仍可共享补采。
目标操作描述缺少模型必需变量，或现有模型算出负数/非有限耗时时，组摘要单列
`cost_corrections_required`，不为这些需求自动重复采集。前者需修正工作描述与模型变量的衔接，
模型尚未训练成功时，也先按显式训练声明检查目标描述是否包含所需变量，不等一次无效补采后才发现。
后者需离线审查模型适用性；其他真正缺测的操作仍可共享补采。运行时缺口还列明缺少的变量、
同操作已有的计时口径或已测变量范围，分别帮助定位接口、测量和覆盖问题。
已完成的独立 CPU 原语测量即使来自复用，也进入当前数据库的导入与训练步骤，
从而允许在上次导入/训练失败后恢复；复用本身不再启动采集。同一次 prepare 仍只尝试该采集族一次。
例如新增 `blocking_return_interval` 必须在离线请求中明确选择模型形式与变量，不能自动拿 CPU 服务模型替代。
成本包的 `training_sources`/`training_source_manifests` 保守记录算子参数及 I/O 模型选择所接触的来源，
即使某次拟合失败，它仍可能影响是否采用独立物理公式，因此不能从隔离名单中删掉。
`io_training.captures` 与分方向 `source_manifests` 记录实际 I/O 参数贡献者；
`considered_runtime_captures` 和 `considered_source_manifests` 另外说明被考察的数据。
两端点估计不把中间观测点当作系数来源，也不把被拒绝的页大小当作已覆盖工作点。
`io_training.measurement_support` 给出实际参与估计的页字节和单次调用字节范围；
没有 runtime 观测时范围为空，不能用独立物理公式的默认倍率伪造运行覆盖。
声明需要的 I/O 成本仍缺失时，包状态是 `missing_data`，不因其他操作模型已训练就报告整体就绪。
`measurement_count` 是可发布操作模型使用的原始测量数，`candidate_measurement_count` 是留出后候选测量数。
两者都不是成本覆盖率，包的就绪状态也不证明所有 target 分支都有成本。

单独预测：

~~~bash
scripts/model.sh predict \
  --source-manifest <base/profile_manifest.json> \
  --target-config <target.json> \
  --cost-model <cost-package.json> \
  --resource-environment <resources.json> \
  --cpu-service-cost <optional-same-source-cpu_service.json> \
  --output-dir <prediction-output>
~~~

没有 CPU 修正时省略该参数。source、target、CPU 文件参数可重复传入。
默认失败后停止启动后续任务，已经启动的任务允许结束；`--continue-on-error` 可继续其余目标。
任务数由明确选择的 source 与 target 组合决定。验证代表格时只传对应输入，不再支持按顺序截断的 `--max-predictions`。

常用结果：

| 文件/字段 | 用途 |
| --- | --- |
| group_summary.json | 准备阶段、预算用量、停止原因 |
| cost-package.json | 离线模型、实际训练来源、缺失模型及部署范围 |
| import_summary.json | 本次显式导入和去重结果 |
| preflight_summary.json | 每份 source 的 trace/事实错误 |
| workflow_summary.json → prediction.cells | 每格输入身份、执行状态、http_e2e_us、成本覆盖和近似项 |
| model_runs/&lt;cell&gt;/run_summary.json | 详细执行结果与 HTTP 时间 |
| model_runs/&lt;cell&gt;/runner_config.json | 单格复现输入 |

group_summary 的 prediction 只保留组级汇总和成本缺口，逐格结果统一读取 prediction_summary 指向的
workflow_summary.json，不在两个文件中重复保存。各阶段不再输出恒定为空的 target_inputs、target_score_inputs
或恒为 false 的 accuracy_verified；数据隔离依靠输入准入和独立执行路径，精度结论来自单独评分。

预测每完成一格就更新 workflow_summary；中途停止时，已报告完成的结果仍可查看，其余格保留未完成和 null 时间。
这不是预测缓存：再次执行仍运行当前代码，不根据旧报告跳过格子。组级状态在整组返回后汇总，运行中查看逐格报告。

预测结束后，终端逐目标显示预测 HTTP 耗时（秒）；没有结果时显示 `not predicted` 和缺口或停止原因，
不把缺失时间记为零。这是预测值，不是 target 实测值；是否使用外推、保留了哪些 base 等待，以及成本覆盖限制，
仍需结合终端指向的 workflow_summary.json 查看，命令成功不代表精度验收通过。

现行逐格身份统一记录在 `prediction.cells`：`model_run_id`、`source_manifest`、`source_config_id`、
`target_config`、`target_hicache`、`workload_id`。执行前先保存未完成的任务，执行结束后更新同一份摘要；
结果目录由 `model_runs/<model_run_id>` 确定，不另存任务索引，也不向 C++ 的 run_summary.json 回写 prediction 字段。
评分先核对组摘要的完成状态，再读取对应 C++ 执行结果，最后才打开 target 数据。
历史结果原有的任务索引与逐格身份仍可读取，静态 oracle-cost 回放不受影响；不迁移或删除历史数据。

失败或未执行格的预测时间为 null，不按零计。cost_coverage=partial 表示估计仍有局限，不等于没有预测值，
也不是已计算出的逐操作成本覆盖率。
损坏的运行摘要直接报错，不把错误类型的内容替换为空对象后伪装成普通未执行结果。
EXECUTED 表示事实及操作已完成执行，不证明结构与 target 一致，更不证明精度达标。

命令成功与预测完成分开报告：顶层 model_run_error_count 统计命令失败，prediction.completed_count
统计 DAG 完整执行。未执行原因（包括 dry-run）见各格 blockers；不再维护 handled、runnable、usable、
skipped 四份顶层计数，避免把命令成功数误解为可用预测数。

完整 HTTP 使用 http_client.e2e_us；simulated_e2e_us 和 full_graph_e2e_us 可包含请求结束后的后台工作，不能混用。
source_observations_status=not_requested 只表示默认未额外导出观测，不表示没有成本依据。

默认 `--diagnostics off` 使用 Release，仅保留紧凑摘要、失败日志和复现输入。
`--diagnostics full` 使用 validation 构建，保留更多明细和成功日志，不改变模型。
当前执行式预测没有完整组件归属和 target 操作映射，不能据此使用历史 oracle 回放。

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
同一配置与 workload 的重复运行按正式窗口耗时取算术平均，结果保留报告路径、样本数和最小/最大值。
评分核对请求顺序、失败请求及已报告的 token 回放失败，不排除 gap、未归属成本或计算阶段。
不加该参数时仍使用单份 target profile，重复身份会报歧义；历史静态 oracle 诊断保持此路径。
所有选中预测完成后才读取 target；输出必须与预测目录分离。
评分不采集、不重预测、不改模型，也不触发 refit。

完整 E2E 对比预测 HTTP 时间与 target 正式 HTTP 窗口，不排除任何成本，
不以 target DAG 回放或预测图后台终点为真值。报告总面板、逐 base、self 和 base-wall 参照；
同时按 workload 和 target 配置汇总，并列出最大 APE 与有符号偏差（预测减实测，正数表示高估）。
加权有符号偏差用于判断整体偏高或偏低，不能代替不允许正负抵消的 WAPE；
缺失格不从验收中删除。具体阈值及当前成绩统一见[验证说明](validation/hicache_validation.md)。

`by_cost_query_domain` 另按独立操作查询分组：查询工作点已测、各变量在范围内但组合未见，或存在域外查询。
同一格同时出现后两种情况时归入域外组；未记录查询与明确没有查询分别列出，不当作已有覆盖。
这只分类该查询报告，不包含 I/O 载荷模型、成本迁移或保留 gap；“各变量在范围内”不等于联合工作点已测，
任何分组都不是全图成本覆盖率或可信度证明。分组只帮助说明结果范围，不改变精度门槛。

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

正式入口由 cli/module_pipeline.cpp 进入 runtime::execute_hicache_window。
结构层提交工作，成本层返回耗时，执行器负责资源绑定与完成事件；具体范围见本文开头的三阶段说明。
静态诊断与前序回放仍有独立计时路径，不作为现行执行式预测的替代入口。

### 状态推进与成本绑定

预取发布、取消及重试由目标执行状态决定，规则侧不重新估算服务结束时间。
host 发布后的存储写入是后台工作，存储返回才使数据可读并进入 ACK 队列；后台返回不直接定义 HTTP 终点。
加载、写回和分页释放先生成操作槽、资源与依赖，再分别绑定载荷与 CPU/worker 成本。

预取检查的 MAX/MIN 等待完整参与组，接收广播按通信组顺序和根 rank 生成入口关系。
组身份来自源部署与观测，不要求源请求曾出现活跃等待；身份缺失是结构输入缺口，不能由成本样本补出。
检查和 scheduler 均查询独立操作模型，不再消费旧 prefetch_wait_calibration 文档。
scheduler 的接收请求、写完成检查、加载完成检查、存储控制处理顺序来自框架规则，
不是从测量中的步骤列表复制。只有执行到重试才查询相关成本。
当前 scheduler 模型仍近似空选择循环，未以动态确认/释放数量解释任意队列负载。

### 源区间、分配与计算操作

source_dag/prefetch 负责预取观测提取；call_regions 按 allocator 内层 probe 及提交关联界定工作，
不沿 stream/FIFO 顺序把后续调用一并纳入。结构报告保留区域入口、出口、依赖类型和缺失观测。
前后页队列必须来自内层调用；外层检查尚未驱逐时的状态不能代替真正分配时的状态。

Attention 与 allocator 共用 runtime/tensor_program。CPU 准备、worker、设备及主机返回分别展开：
Ascend Triton 队列打开时使用 worker，直接提交时不虚构 worker/dispatch 成本。
同步读回使用本文与成本文档中的独立空闲同步近似，不把未分离设备等待的墙钟当作 CPU 服务。
成本观测按分项判断可测性；一段不可用不应丢掉其他可测分项，也不能拼成已证明完整的源迁移成本。
source_dag/submission_cost 负责传输提交及分页释放测量，目标执行器不再承担原始区间测量职责。

extend 与 Decode 都先按目标状态生成分配程序；Decode 保留外层容量检查与请求索引更新。
输入不足一个缓存页时仍生成分配和 Prefill；没有可缓存的整页不等于没有请求工作。
空 radix 插入也保留未完成请求的尾页，并在结束时释放。跨正式窗口起点的插入按实际区间保留，
不能只按开始时间过滤，否则会漏掉目标策略在这段工作中新增的发布或写回位置。
正式窗口只切换测量范围，不清空仍在结束处理的请求及其 KV 所有权；窗口前的执行不重新计费。
早于起点的生命周期里程碑在窗口入口建立状态，窗口内的发布和返回继续沿实际依赖执行。
窗口前已提交、窗口内才执行的设备工作，保留唯一原生提交的元数据来确定流顺序；
不恢复提交线程的窗口外执行，也不以设备执行时间猜提交位置。
仅当长度、页大小、batch、新页数、队列、指针偏移及准备历史明确对应时才迁移未变调用，
未知字段不视为相等。Decode 的 last_loc 来自 int32 请求索引表，不套用 extend 的 int64 假设；
没有新页且无需 dtype 转换时，不生成不存在的设备操作。
冷 Triton 调用的准备/装载与热调用成本分开；准备观测使用区间并集，CPU 与残余分别计量。
历史静态准备聚合仅保留作诊断，不进入正式源对象或替代独立操作模型。

实验报告的 executed_allocator_operations 来自实际执行节点，比较请求/轮次、shape、工作量、
资源分组和完成点可达关系；不是比较尚未执行的计划。缺失和重复身份单列，不按局部节点 ID 或时间邻近配对。
CPU/control、原生内部提交和重复 chunk 身份仍有覆盖限制；执行计数与结构一致性是不同证据。

structure/hicache/attention 保留框架后端选择的条件优先级；framework_dispatch 只说明分支选择，
不代表每个特殊模式都有完整张量程序。当前普通 FIA Prefill 的 TND/BSND、已支持的 Decode 布局
及其成本范围必须分别核对，不能用新增样本替代缺失的结构规则。
不保留测试专用标量循环或通信回放；历史静态 DAG patch 和显式 oracle-cost 诊断继续保留。

TraceGraph 的头文件统一位于 `include/markov/trace_graph/`，与 `src/` 的实现按模块对应；
内部辅助头也遵守这一布局，位置变化不表示它成为公开 API。项目不再维护独立测试文件和测试夹具。
引用统一写为 `markov/trace_graph/...`，不通过相对路径引用 src 内部文件。

trace 读取器与事件参数查询共用 `include/markov/trace_graph/json_scan.hpp` 定位 JSON 片段，保留共享缓冲区和延迟解析，
不为简化实现而构造整份 trace 的 JSON 对象。转义字符串由现有 JSON 库解码，保证按需查询与完整
参数展开对 Unicode 的解释一致；跨采集通道合并参数时保留接收事件的默认 pid/tid。

跨 rank 合图保留各输入的顺序，事件与语义侧表各自重新编号，输入合同在合并后统一去重。
计算阶段标记只选择、排序一次，控制成本回放复用同一份源标记；它仍是独立诊断，不改变完整 HTTP 口径。
普通回放与动态展开共用一个仿真入口；CPU 队列识别统一扫描依赖边，再按各线程的提交关系建立 FIFO，
不为每条线程重新扫描整张图。这些整理已完成重构后的统一验收；范围和限制见工作进展，
不能据此声称后续成本修正或完整 target 结构已经通过验收。

Prefill 与 Decode 的设备、通信和提交 CPU 观测共用节点成本结构及统计逻辑；
Prefill 的普通计算与注意力仍分别保存。诊断归属与输出复用同一份 phase 观测，
不重复解析；对外观测字段、成本公式和历史静态回放接口不变。

维护时必须保留：

- source facts 身份与可执行 DAG 节点身份分开；FutureDag 不回滚已开始工作。
- 写入、容量分配与回载通过 replay 的统一身份索引查找事实；decode 派生事实入表后先重建索引，再绑定执行边界。
- 物理完成、前台可见和引用回收分开；跨 rank 确认只消费本轮入口采样到的前缀。
- 实际读取、拷贝和发布量分开；取消不能抹掉已经发生的 I/O。
- 线程、worker FIFO、stream/event 与资源 lane 表达顺序和重叠；不能用总耗时替代依赖。
- HiCache 只替换明确归属的 CPU 区间；未测量 gap 与 CPU 服务成本分开，嵌套区间不能重复收费。
- Prefill/Decode 工作量来自目标缓存状态，变化操作查询独立成本模型，不按 source 算子份额分摊；缺少生成规则仍报告限制。

gap-excluded 是历史组件组合关键路径，排除 residual gap 和未归属成本，不是完整墙钟仅减 CPU 空白。
component ownership 和 formal window 按执行语义定义，不按配置名或 cell 硬编码。

| 位置 | 职责 |
| --- | --- |
| modeling_workflow/group.py、prepare.py | 组输入、预算和宿主机编排 |
| base_capture.py、cpu_capture.py、capture.py、physical_capture.py | 各类采集及恢复 |
| modeling/source_dag/ | 采集引用、原始观测提取；不依赖上层工作流 |
| modeling/cost/ | 数据库、离线训练、I/O 合同、独立控制测量选择与成本包 |
| fixed_calibration.py、control_acquisition.py | 共享实验选择与补采编排，不实现成本回归 |
| prediction/、execution/ | 预测任务与结果 |
| evaluation/、validations/ | 独立评分和历史诊断 |
| trace_graph/src/source_dag/ | 原始观测、调用归属和测量提取 |
| trace_graph/src/structure/ | 目标操作描述、张量程序及生成规则 |
| trace_graph/src/cost/ | 独立成本查询、绑定依据与覆盖报告 |
| trace_graph/src/modules/hicache/model/、runtime/ | 缓存状态与动态执行、资源及完成事件衔接 |
| trace_graph/src/modules/hicache/patch/、src/simulation/ | 图变换、成本落图与仿真 |

预测执行器只接收本次计划并返回结果，不跨调用保存状态。串行和并行共用有界任务队列；
停止策略在补发任务前检查整批已完成结果，已启动任务仍正常收尾，未启动格显式标记。
工作状态完成但缺少有效 HTTP 正式窗口耗时的运行不计为成功预测，也不能进入独立评分。

完整约束见[项目约束](project_constraints.md)。日志是开发记录，不是当前实现的替代说明。

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
