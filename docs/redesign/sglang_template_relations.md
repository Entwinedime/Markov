# SGLang 模板关系与原生机制参考

本文按需查阅，集中保存当前 SGLang/Ascend 路径的原生分支、参数来源及操作替换条件；从[采集与模板合同](04_collection_templates.md)进入。这里的局部关系不构成完整目标资格，历史样本统计不能作为当前进度；当前状态只见[实施计划](../redesign-workflow/2026-10-08_redesign_execution_plan.md)。

## Serving、资源与通信关系

### 通信工作量与物理分支的已有证据

现有 TP=2 资产的通信身份已独立核对：1,300 个 collective 的 `(rank, connection_id)` 全部唯一，27,412 个物理任务的 `(rank, stream, task, subtask)` 全部唯一；11,958 个通信 detail 均通过明确任务身份关联到唯一物理事件和 collective，无歧义或遗漏。证据在 data/redesign/s3-collective-workloads/collective_identity_audit.json。这里只证明这份资产的关联，不证明所有提交、时钟或源依赖完整。

所有 collective 的逻辑 count 与同批模型/请求参数一致：AllReduce 为 token 行数乘 hidden_size，AllGather 为请求行数乘当前 TP=2 的词表分片大小。来源绑定分别为 embedding、decoder layer、MLP 和 logits，不能将 decoder layer 归属进一步猜成某个内部函数。逐项记录见 collective_parameter_audit.json。模板中的 CollectiveLayout 现按已记录的模型区域绑定逻辑 count：embedding 使用 hidden_size 和 token 输入轴，decoder layer/MLP 使用 BF16 hidden 输入轴，logits 使用同 scope 内唯一且已资格化的 projection 输出词表分片。源 count 必须与关系吻合，未知来源保持未绑定。该关系可计算目标 BatchInput 的逻辑 count，但不改写物理任务或推定目标传输结构成立。

| 已观测通信 | 每 rank 的 detail 数 | 对目标变换的限制 |
| --- | --- | --- |
| AllReduce，count=5,120 / 10,240 / 81,920 | 8 | 小消息样本共享这一任务数量，但不能据有限样本推导全部适用区间 |
| AllReduce，count=1,305,600 | 14 | 与上行算法标签同为 MESH-RING-NHR，物理结构仍不同；算法标签不能独自决定变体 |
| AllGather，count=75,968 / 151,936 | 9 | 现有两个请求数下数量相同，不证明任意大小或 TP 下拓扑不变 |

大 AllReduce 的 BF16 有效载荷为 2,611,200 字节，观测到 1,310,720 与 1,300,480 字节的不等长分块，两 rank 的分块顺序不同；不能把所有 memcpy/reduce 的字节数统一按比例缩放。4 字节控制任务及零字节 notify 与数据搬运分别保留，零载荷不代表没有同步依赖。上述数字是观测证据，不写成经验阈值或硬编码分块公式。

当前安装的 HCCL/HCOMM 均为 9.0.0。已读取官方 v9.0.0 源码：HCCL 提交 ba6375e7992301ef43bfce9b4e79b68043edf891，HCOMM 提交 09a304742994c603879a57291579afd52c3126d9，副本保存在上述证据目录，不增加构建依赖。HCCL 的非 950 设备路径转入 HCOMM 的 HcclAllReduceInner，因此不能直接套用新 HCCL 算子目录的 selector。[HCCL 入口](https://gitcode.com/cann/hccl/blob/ba6375e7992301ef43bfce9b4e79b68043edf891/src/ops/all_reduce/all_reduce_op.cc)

HCOMM 的 MeshTopoSelector 在单算子模式下以 256 KiB 区分 SmallCount 与 OpbaseLoop；进入该 selector 还依赖拓扑、设备数量、inline-reduce、缓冲区条件和前序 AIV 选择等条件。AllReduceMeshDirect::PrepareSlice 先按 rank 数向上均分，再将片段上限按 16 KiB 对齐，最后一段取剩余字节。此源码公式与 258 个大消息 collective 的数据片段大小全部吻合，但只证明候选机制一致，尚不证明实际执行器身份及全部切换前提。[选择逻辑](https://gitcode.com/cann/hcomm/blob/09a304742994c603879a57291579afd52c3126d9/src/algorithm/impl/operator/all_reduce_operator.cc)、[分块实现](https://gitcode.com/cann/hcomm/blob/09a304742994c603879a57291579afd52c3126d9/src/algorithm/base/alg_template/temp_all_reduce/all_reduce_mesh_opbase.cc)

源码审阅记录在 hccl_mechanism_audit.json，安装信息在 installed_versions.txt。下一步确认部署前提、循环容量和逐 rank 任务/完成映射，再复用已有 FFTS 依赖实现有限变换；逻辑载荷、物理任务结构、完成端口和成本单位分别表达。当前保留全图变换门槛，不通过补采更多长度猜阈值，也不将源码中的局部阈值误用为全局选择规则。

#### 通信完成点与内部依赖

复用notify ID时，生产端先分别证明发送侧和接收侧的代次顺序，再逐代配对。不同物理任务使用已由流依赖建立的chain/position；同一复合任务使用FFTS描述符依赖及物理任务extent。每侧任意两代必须有且仅有一个可达方向，不能把task/context编号当作先后次序，也不能用跨rank时间戳配对。记录/等待数量不等或顺序不唯一时保留`native_notify_generations_not_uniquely_matched`缺口，不生成猜测通知边。

现有通信资产已独立核对物理成员覆盖：所有 root 的硬件 connection 成员与导出的任务集合相同。1,300 个 collective 各包含一个 stream/task 组，各自的原生任务依赖子图均有一个入口和一个出口；每组所有物理成员共同连接到同一个 DeviceTaskCompletion 汇合点。证据在 data/redesign/s3-collective-completion/completion_audit.json。该结论不消除其他 CPU/设备时钟或完整源图的缺口。

TemplateCollective 的 entry_boundaries、exit_boundaries 和 internal_dependencies 是已有 NativeTaskDependency 的局部视图；completion 只绑定到输入集合恰好等于全部通信成员结束点的既有 DeviceTaskCompletion。它不使用 root 包络结束时间，不创建服务、不连新边，也不把多任务组缺少单一汇合时解释为零等待。多个 rank 间的 notify 仍由原有跨模板连接表达，不能折叠为一个通信时长。

CPU 提交链路另需从包络入口落到物理任务。已有 flow 仅到 collective annotation，之前的源图中 1,300 个包络入口都无法到达其物理子任务（data/redesign/s3-collective-submission/before_audit.json）。read_communication 现通过同一精确 collective/task 身份增加 collective_submission：包络 begin 连接到没有组内 NativeTaskDependency 前驱的物理任务 begin，其余工作沿原生依赖推进。source_reference 指向源 collective interval；不从包络 end 出发，不添加包络服务成本，也不把 CPU API 返回强接为设备屏障。该修复处理已观测提交的因果缺口，不能替代其他未观测提交或时钟资格。

通信还复用 SourceOperation/TemplateOperation 的 CPU 操作视图。async_npu 的目标若是 collective 包络，提取器通过已核对的 SourceCollective 关联，将实际任务写入 device_spans，包络另存 collective_spans；主线程 host 和异步 worker 窗口仍由原 flow/queue 身份确定。此前 1,300 个通信操作的 device_spans 各只有包络，不能当作完整设备子图。修正不额外创建操作族、不添加 CPU 切分，也不把包络当可计费设备工作；包络仍可作为提交边界引用。

通信操作切分现复用 TemplateDefinition::operation_region：device_spans 与 collective_spans 共同指定异步成员，CPU 仍只按自身 lane 上的同步调用窗口归属；任务汇合仅在全部贡献者位于区域内时纳入。统一输出局部节点/边界、跨界服务、入边、出边及外部连接，不能只替换设备任务而遗漏提交包络。当前资产的 1,300 个操作包含 11,958 个物理任务、1,300 条内部提交关系和 1,300 个任务完成点，保留 6,236 个外部连接引用，未报告跨界 CPU 服务。证据在 data/redesign/s3-communication-regions/region_audit.json；完整目标图与此前一致，没有新增采集。该结果验证切分交接，未批准通信物理结构改写，selector、工作量及目标端口映射仍需独立实现和验收。

#### 逐 rank 工作角色与通信交接合同

现有 1,290 个 AllReduce 的 3,096 个数据任务已按源码候选角色逐项核对（collective_rank_work_audit.json）。大消息每 rank 依次包含本地片段输入拷贝、对端片段规约、本地片段输出拷贝和对端片段输出拷贝；小消息 rank 0 为完整载荷的两次拷贝，rank 1 为完整载荷的规约和拷贝。该顺序用于核对已记录 context 中的数据任务，不由观测时间生成依赖。控制任务与 notify 仍使用已有原生关系。

小消息的 ReduceBcast 与 LocalReduceBcast 在 TP=2 下可能具有相同的数据任务表现，任务数量和载荷一致不能唯一判定执行器。现有 profiler 设备信息及 launch/process 日志没有提供完整选择前提。原采集的已退出容器仍保留，现已读取其创建时环境和容器内安装文件，确认没有 Docker 层 HCCL 环境覆盖、HCCL/HCOMM 均为 9.0.0；证据在 data/redesign/s3-collective-deployment/capture_container_evidence.json。该记录不能替代子进程内修改或 communicator 选项，也不能用当前容器环境倒推历史设置。

进一步从原采集的已退出容器取回两个 worker 的原生 plog，PID 647/648 与原始 rank 0/1 观测对应；两份日志各记录唯一加载的 group_name_3，并记录该组 AllReduce/AllGather 的 HCCS transport 创建。comm_config 的实际加载值均为 bufferSize=209715200、deterministic=0、opExpansionMode=0；环境解析另记录 aivMode=0、aicpuUnfold=0，拓扑记录 rankSize=2、serverNum=1、moduleNum=1，芯片为 910B3，运行使用 FFTS。两 rank 的 symmetricMemoryStride 分别为 0/1，原值保留，不能将整份配置简单视为相同。证据与原文行号见 data/redesign/s3-collective-deployment/runtime_selector_evidence.json，日志位于同目录 native-logs；未启动容器或新增采集。这补充了此前缺少的子进程/communicator 初始化事实，但没有逐操作 executor 选择日志，源 collective 也尚未正式绑定 communicator 配置。

同版本 device_capacity.cc 的 910B 地址条件为模 1 对齐比较，并明确支持 BF16/SUM，因此输入长度或分配地址改变本身不改变该项 inline-reduce 条件。已记录 deterministic=0 排除了 SmallCount 内部的 LocalReduceBcast 分支，进入 SmallCount 后选择 ReduceBcast；仍不能跳过 SmallCount 的外层选择条件。OpbaseLoop 的容量公式为 cclBuffSize / unitSize × rankSize，在上述缓冲区、BF16 和 TP2 条件下为 209715200 个元素。该数值是条件推导，不写成核心常量；正式变换还需绑定工作模式、用户/CCL 缓冲区别名关系、有效容量和目标部署限制。

通信资格要求目标变换结果有确定依据，不机械要求唯一执行器名称。同版本源码中，TP=2 时 LocalReduceBcast 的额外本地规约循环为空、rank1 发送偏移为零；在相同内存布局和 stream/notify 绑定下，其数据操作和算法主体 peer 握手与 ReduceBcast 一致。细项见 data/redesign/s3-collective-deployment/small_algorithm_comparison.json。Prepare/同步已进一步核对：同一小消息 executor 在 OP_BASE 下令 scratchMem=outputMem，两种 Prepare 调用因此绑定相同内存；slaveStreams、notifiesMain/notifiesAux 和 rank 参数来自同一资源响应，LocalReduceBcast 默认 streamNum=0 与另一实现同样遍历全部通知对象。结合 TP2 主体分析，这两个候选在上述条件下的设备数据操作和同步调用一致。仍须核对可达候选集合是否完整、目标是否留在共同分支，以及 CPU 服务边界；设备操作一致不证明 CPU 调用路径或耗时相同。

如果已经由部署和实现条件约束出的全部候选，对声明目标范围给出相同任务、工作量、依赖、资源和端口变换，可按同一有限规则处理；若候选之间还有可影响结果的差异，只记录并解决该差异，不为区分无关名称增加 wrapper、采集或规则。此处是资格判据调整，尚未放开当前目标通信变换。

通信组身份通过厂商数据库获取：capture 生产者为每个 rank 显式发布 database 引用；归一化只读连接 COMMUNICATION_OP 与 STRING_IDS，以 connectionId 和操作名关联 trace，保留 group_name 和原数据库 opId。重复的连接/名称组合报身份歧义，不按时间或“唯一已知组”猜测。SourceCollective::group_name 随源图和 TemplateCollective 交接到公开诊断，源 span 的 source_interval 可追溯数据库记录。该身份用于连接同进程的 communicator 配置，组名自身不证明配置、执行器或目标变换资格；不同运行中的同名组不能直接视为相同部署。

#### 物理通信工作进入执行图

SourceCommunicationTask 提供统一 describe，源 collective 与执行图共享同一参数合同。TemplateNode 按 source span 绑定所属 collective 的物理任务，ExecutionGraph 在现有节点上保留该只读描述；没有为包络新增服务节点。完整图的节点 communication 字段包含实际字节数、操作、传输、端点和原始 task/context/detail 来源，节点数组下标及 work_id/rank 定位目标实例；主报告只引用图文件，不重复输出 communication_parameters 数组。

通信参数的来源解释由生产端负责：SGLangTemplateBindings 从 embedding、decoder、MLP 和 logits 的已观测区域绑定通信量轴与宽度；HcclMeshLayout 在建库时解释 HCOMM 部署条件、传输角色、消息分支及字节关系，生成统一 CommunicationWorkLayout。TemplateCollective 保留通信观测和局部图视图，通用依赖分析提供任务入口、出口与完成点，生产规则消费该结构资格，不用厂商包络时间替代完成。

CommunicationWorkLayout 只保存元素字节数、分片数、对齐字节数、计数适用区间及任务到完整载荷/指定分片的关系；核心不按框架、算法或原生任务名称分支。区间按生产端确定的顺序检查并返回相应缺口；未列入参数关系的任务保留源字节数，控制和通知不随载荷缩放。分片由总字节数均分、对齐后截取剩余载荷；这些参数必须由生产端证明合法，并保证计数范围内的算术可表示。

TemplateBank 已改为消费该通用关系，不再调用 HcclMeshLayout；不复制第二份源任务或通信图，目标任务仍保留原 span、资源和依赖。这项合同当前由既有双 rank、BF16、同分支生产规则生成，不因此扩大框架/TP/算法支持域。模型与 KV 等生产布局仍未全部统一，不将通信一项解耦视为跨框架合同完成。

TemplateInstance 现在持有目标通信参数覆盖，ExecutionGraph 读取实例参数，bank 中原始观测保持不变。当前 AllReduce 规则限定 HCOMM 9.0.0、910B、双 rank 单 server/module、HCCS、非确定性及已观测的 host 展开条件；只处理同一消息分支内的单循环工作量。小消息使用完整载荷，大消息按原生 16 KiB 对齐规则分别计算本 rank 与对端切片；源任务角色和字节数必须与关系吻合。跨 256 KiB 分支、跨循环或未知部署明确拒绝。通知、4 字节控制任务及厂商物理 dtype 保留原值，不能统一按比例缩放。AllGather 已使用同一实例参数入口，按下节的完整载荷关系生成工作量。

全图精确输入门槛仍保留，因此上述参数生成不等于非身份目标已经通过结构验收；诊断 physical_transform_qualified 只表示该 collective 的物理参数规则成功。成本模型应读取实际节点参数，不能同时对 collective 包络计费。

#### AllGather 的有限工作量规则

已用同一版本原生源码和现有资产核对普通 BF16 all_gather_into_tensor：torch_npu 的 _allgather_base 经公共 HcclAllGather 进入 HCOMM OP_BASE；910B、单 mesh、AIV 关闭时选择 AllGatherMeshOpbaseExecutor。该路径没有 AllReduce 的 256 KiB 大小分支，不能复用其切片公式。

TP=2 单循环时，每个 rank 执行输入到暂存区、暂存区到本地输出、对端暂存区到输出三次数据拷贝，每次均为 count × dtype 字节数；源资产两个 count、两个 rank 共 10 个 collective 的 30 次拷贝全部吻合，另外 40 个通知和 20 个控制任务保留独立角色。原生循环上限为 floor((buffer_bytes − 16384) / 128) × 128 / dtype 字节数，输出偏移和暂存区模 16384 偏移另有地址语义，不能解释为 AllReduce 的分块。

两 rank 的原日志已有 AllGather HCCS 链路证据，见 data/redesign/s3-collective-deployment/allgather_entry_contract.json。正式合同现用 communicator.transports 保存 operation、peer、transport，来源事实另保留原日志行号；替换原先只表达 AllReduce 的字段，不提供旧格式回退。TemplateCollective::mesh_work 共用部署、边界和任务角色检查，按操作选用自身链路；AllGather 三次数据拷贝使用完整载荷，检查扣除对齐余量后的单循环容量和原生 SDMA 单任务上限。目标参数由实例持有，不修改源任务。全图精确输入门槛仍保留；参数生成不证明 CPU 路径、地址重绑定或 changed-count 完整目标拓扑已合格，不需要为上述已知关系补采。

#### Communicator 初始化配置的交接

native_capture.communication_logs 显式列出原始 worker 日志及 rank/pid。采集生产者在启动框架前记录默认 Ascend plog 的已有字节位置，结束后仅保存已观测 worker 在本次 attempt 新增的内容，避免 PID 复用带入历史组配置；消费者只读这些显式引用。没有可用日志时字段为空，不能填入本机默认配置。日志保留和后处理的开销计入产物与后处理统计；不为取得配置增加模型运行或改变原生日志级别。

归一化按 HCCL 进程身份和 group_name 解析 comm_config 的 bufferSize/deterministic/opExpansionMode，以及 communicator 拓扑中的 rankSize/serverNum/moduleNum。三项记录必须各唯一；缺失或重复初始化保留配置缺口，不选择最后一条或因数值相同就合并不同生命周期。配置附到已有通信 interval，并保留每项原日志文件和行号；SourceCommunicator 随 SourceCollective 及 TemplateCollective 交接，并保存设备类型、HCOMM 版本、AllReduce 的对端链路及环境 AICPU/AIV 设置。字段为 null 表示缺观测，不表示默认部署或配置为零。

HCOMM CommConfig::SetConfigOpExpansionMode 中 0 表示沿用环境配置，并不表示关闭 AICPU/AIV。归一化保留 HCCL_ENV 日志中的 env_aicpu_unfold、env_aiv_mode 及原文行号；当前规则要求 mode=0 且两个环境值均明确为 0。缺失或冲突不能推定关闭，也不以运行当前容器的环境替代历史采集条件。

这部分是初始化观测，尚不证明 communicator 在整个生命周期内没有运行时修改，也没有覆盖设备型号、物理链路或框架提交路径的全部资格。目标变换需将这些事实和已有调用机制一起判断，不能仅凭 rank_count=2 开启通信缩放。重复初始化的 generation 关联、非默认日志位置与实际新采集的日志保存仍须在相应使用场景验证。

#### 当前主线 AllReduce 的调用与选择依据

原采集容器中的 torch_npu 为 2.10.0，提交 94f8a8e6b523d7ba553e1b80d5b5248478391526；已按该提交核对[官方 ProcessGroupHCCL 源码](https://github.com/Ascend/pytorch/blob/94f8a8e6b523d7ba553e1b80d5b5248478391526/torch_npu/csrc/distributed/ProcessGroupHCCL.cpp)。SGLang 普通 NpuCommunicator.all_reduce 进入原地 allreduce，同一张量作为输入和输出；BF16/SUM 不触发整数转换、预乘或 AVG 后处理。公共 HcclAllReduce 转入非 950 的 HcclAllReduceInner，入口设置 OP_BASE 后调用 AllReduceOutPlace 并保留用户指针。

CCL 输入与输出缓冲区是同一次分配中的两个不同起点，而原地输入输出指针相等，因此 selector 中同时匹配两个 CCL 起点的融合条件不成立。无需为排除该条件补采指针。结合实际 communicator 的 910B3、双 rank 单 server/HCCS、AIV 和确定性关闭条件，剩余 MeshTopoSelector 以逻辑载荷 256 KiB 区分 SmallCount 与 OpbaseLoop，前者选择 ReduceBcast。这是带部署和调用条件的源码推导，须绑定到正式资产后才能用于目标选择。

当前 hidden_size=5120、BF16 的阈值对应最多 25 个 token 行；该行数仅为上述字节条件的推导，不编码成模型特例。max_prefill_tokens=16384 对应 83886080 个元素，小于当前缓冲区推导出的单循环容量 209715200；声明范围内可优先实现单循环规则。大消息按源码对齐切片，小消息按完整载荷处理，控制任务与通知保持各自语义。越过消息分支仍需选择或改写有依据的物理子图，不能直接缩放整个模板。详细来源和剩余实现项见 data/redesign/s3-collective-deployment/allreduce_entry_contract.json；这项分析不代表目标图变换已运行通过。

正式通信交接按以下职责落实，复用现有源图和实例体系，不另建通信仿真后端：

| 对象 | 必须保留 | 不能推定 |
| --- | --- | --- |
| 源 collective | rank、源 interval、owner/scope、原生名称、count、逻辑 dtype、算法标签 | 算法标签等于物理执行器身份 |
| 源物理子任务 | 唯一任务/子任务身份、关联 collective、源 detail、任务种类、字节数、传输及端点信息 | detail 的 dtype 等于逻辑 tensor dtype；零字节代表无依赖 |
| 目标通信实例 | 目标逻辑载荷、通过资格判断的任务角色与工作量、端口/依赖改写 | 全部子任务统一按比例缩放，或两个 rank 共用同一任务列表 |
| 成本绑定 | 已明确的物理服务或局部子图、对应工作量与来源 | collective 墙钟和内部任务重复计费，或将等待当主动服务 |

源图提取现已增加 SourceCollective/SourceCommunicationTask：read_communication 共用原 notify 的精确任务索引，关联数据工作和通知，source_graph.json 的 collectives 保存逻辑 count/dtype/算法及物理任务的 span、detail、stream/task/context、字节数、物理 dtype 和传输端点。owner/scope 继续通过源 span 引用，不复制另一份归属。collective connection 重复时报告歧义，不任选一个名称。此字段交接保留观测，不新增图依赖、服务成本或目标变换；模板消费者现已按 owner_call 读取 SourceCollective，通过源 span 映射到 TemplateCollective 的 envelope_node/task_nodes，并核对 rank；这些索引引用已有模板节点。公开 observed_collectives 保留完整源描述、内部 scope 和局部节点编号。逻辑 count 关系已绑定，目标物理任务工作量及拓扑改写仍待接入，不能把源合同交接当作跨配置能力。

### 原生 API 身份与时间戳资格分开核对

Native API 关联先核对 rank、stream/task、硬件类型、唯一 CPU connection、线程及 scope。rtMemcpyAsync 可以是 ACLNN 操作内部的 DMA；当前除独立 memcpy 与 inplace copy 外，已有原始证据支持 aclnnCat 内部的拷贝。保留真实的外围 API 名称，不将它改写成独立 memcpy，不因名称相似接受不唯一或类型不符的 task，也不依赖时间接近配对。

data/redesign/s3-native-copy-association 复用原始文件重新规范化和提取，补上 4 次 aclnnCat 内部 DMA 关联，既有 6,506 次关联不变；调用、flow 和通信记录不变，interval 仅增加这 4 项 native_api 引用。完整源图及公开预测的物理图保持一致。其余 44 项 task 类型不符和 10 项缺硬件任务记录仍保留；不能把查询得到的旧 task ID 强行关联到新拷贝。

当前 39 条 host wait 时间矛盾涉及 17 次同步，逐项仍有硬件任务到 wait 完成的因果路径。因果路径存在不证明时间戳已经校准；原生与 profiler 两种比较仍矛盾，因此不修正时钟、不移除缺口、不升级完整依赖或性能资格。另两条 owner/scope 均为空的设备同步位于末次 serving 返回之后，暂只记录位置证据，不据此认定它们无关或删除缺口。详细证据见 association_audit.json、dependency_audit.json 和 prediction_audit.json。

### 非模型组件的统一语义绑定

核心 NativeRegionBinding 同时描述模型方法和明确的 serving 组件方法。prepare 输出 native_regions；Python NativeRegions 对模型遍历 named_modules，对组件从当前 ScheduleBatch 的显式 component_path 取得实例。两者共用 NativeRegionWrapper/BlockWrapper 生命周期。模型记录为 model_region，组件记录为 component_region，均是继承父 serving 步骤的嵌套区域，不新增顶层流程动作。C++ SourceNativeRegion、归一化及模板提取保留相同的输入、输出和原生归属。

首批组件绑定是 NPU allocator 的 alloc_extend/alloc_decode，安装在真实 Prepare 执行前。记录 page_size、need_sort、输入布局、调用时 free_pages 布局与完整返回 tensor 布局；extend 另记 extend_num_tokens。outputs 的 index=null 表示整个返回值，整数表示 tuple 槽，空返回保持 null。module_inputs 在每次调用读取布局，避免把可替换的 free_pages 缓存在安装时。

cpu_inputs 明确声明 prefix_lens_cpu/seq_lens_cpu 等已有 CPU tensor，记录其整数序列供后续分支关系核对；不把设备 tensor 搬回 CPU，不增加同步。CPU 长度值不等于已经观测到 prefill 内部由设备求得的 num_new_pages_item，也不证明其他调用者提供显式 num_new_pages 时的语义。实际分支、free/merge 行为、内部节点角色与目标变换资格仍需按覆盖表完成。

这两项绑定先补齐观测入口；未经原生运行验证不能标记采集覆盖成功，更不能据此宣告跨页结构可变换。已有模型区域资产继续按其模型绑定读取，组件事实只来自包含对应记录的采集；不从旧 Prepare 的相似 Add/Sub 名称补造组件身份。

allocator 生命周期现声明六个入口：alloc_extend、alloc_decode、free、free_group_begin、free_group_end、merge_and_sort_free。后四项复用同一 wrapper；继承方法也绑定在实际 NPU allocator 实例上。所有入口记录调用时的 is_not_in_free_group，以及 free_pages/release_pages 布局。对象整数属性和 tensor 布局均在调用时读取；模型固定维度仍使用同一合同，不另建状态观察器。

| 原生机制 | 观测合同与待核对事实 |
| --- | --- |
| 独立释放 | free_index 布局、空输入分支、当前分组标志；非分组时 NPU 实现进行 CPU unique 和设备拼接，不能把它当作零耗时状态操作 |
| decode 分组释放 | 发布流程调用 begin，逐请求 free 只积累索引；end 先恢复非分组标志，再对非空列表 cat 并嵌套调用 free。依靠调用父子关系区分两种 free，不能重复计算实际释放工作 |
| 空释放组 | begin/end 可存在而没有内层 free；没有子调用不等于丢失释放 kernel。当前不记录列表内容，不凭该合同推定其他未观测分支 |
| 合并待释放页 | merge_and_sort_free 在 release_pages 非空时拼接并排序；入口布局提供分支条件，但不提供页 ID 或调用后状态 |

六个入口的参数已按官方实现及继承链静态核对；真实观测结果见下文，完整目标变换和依赖资格仍待验证。当前只声明入口状态，不伪装为调用后快照；原生 free 的设备索引内容不由 wrapper 回传 CPU。调用后页身份由目标控制器的合法状态规则表达，并需另有生命周期证据。该观测覆盖表用于一次性审阅必要合同，不能把新增绑定数计作已取得的模板数。

2026-10-10 已按上述覆盖表完成一次真实采集：输入长度 127/128/16、输出长度 4/2/2，三请求同时到达。两个 rank 共取得 32 条组件记录：alloc_extend 4、alloc_decode 6、free_group_begin/end 各 6、free 10。decode 新增页分别为 [0,1]、[1]、[0,0]，同时取得空组及非空组的嵌套 free；32 条区域均经公共提取链路归属 Prepare/Publish。need_sort=false 下未触发 merge，保留此边界，不为取得该记录改变配置。

Prepare 返回额外记录 request_pool_slots，直接读取原生 request 的 req_pool_idx。两个 rank 均观察到请求 a 使用槽 2、b 使用槽 1，b 发布完成后 c 复用槽 1。预热改变了空闲槽顺序，目标规范槽号使用一致双射核对，不能要求原生和目标编号相等。给定节点耗时的公开预测完成 5 批、20 个动作；每批 context、新增页数、页数量、分配前空闲页数及槽复用关系与原生观测一致。设备页 ID 未读取，不能把页数量一致宣称为物理页身份逐项验证。

该次原生运行 179.822 秒、后处理 38.073 秒、采集目录 580093861 字节，分别在 300 秒、120 秒、1.5 GiB 上限内。一次采集后停止；未追加分支采集。源图无环，但依赖覆盖仍为 partial，存在 39 条 host wait 时钟顺序矛盾及既有归属/成本缺口；模板跨配置变换和未见请求验收尚未完成。证据与明确范围见 data/redesign/s3-allocator-branch-evidence。短输入没有消除初始化和大图存储成本，不能据此宣称整体轻量建库已合格。

### 输入解析与CPU上传的显式区域

生产绑定sglang.inputs.resolve将原生resolve_forward_inputs声明为嵌套component_region，继承父Prefill/Decode。入口通过公共module_inputs记录prefill_input_ids_cpu、mix_running_indices布局；SGLangInputResolutionWrapper复用NativeRegionWrapper/BlockWrapper，记录CPU staging tensor的进程内地址、is_pinned观测及numel×element_size，退出记录input_ids布局和目标地址。不存在Prefill输入时input_copy为null，不把Decode的FutureMap读取说成H2D上传。wrapper只保留元数据，不延长staging tensor生命周期，不读取设备内容或增加同步。

输入解析与已有输出观测共用SGLangRegionHook，旧专属OutputHook已替换。原生rtMemcpyAsync日志新增copy_addresses，记录实际源/目的指针；其他API为null。地址只用于同进程、同次调用的关联，不能作为跨运行模板身份或成本特征。只有区域、队列归属、地址及字节一致才可把tensor内存属性关联到实际复制；不能仅凭框架pin策略或相同shape推定。mixed输入的最终concat地址不自动等于上传目标，未资格化分支保持限制。

s3-input-upload-observation已完成一次既有代表请求采集：两个rank共30个区域进入规范化事实和公共模板提取，10次Prefill上传的进程内地址/字节与唯一rtMemcpyAsync及归一化scope对应，源tensor均观测为is_pinned=false；30次原有输出区域及output_work记录保持。源图仍needs_data，时钟/其他关联与成本缺口未关闭。这些证据只授予该输入上传观测链路；目标CPU复制字节变换已接入下述有限关系，API完成与成本资格仍待核对，不从旧事实补造区域。

输入上传工作复用已有CPU服务节点：以区域内唯一地址/字节匹配的API为来源，在统一导出时间坐标中按驱动起止切分，字节只绑定同rank、线程和scope中与copy_interval精确一致的服务。外层API包络不再承担复制工作绑定；多次驱动复制或存在其他复制重叠时保持缺口，不能重复挂载全量字节。完整冷Prefill的参数关系为各请求input_tokens之和乘实测元素字节，生产端同时核对staging布局、实际复制字节和源请求；核心只消费通用参数关系。SourceTransfer在CPU分段后绑定，模板通过InputTokens及transfer_overrides交付目标字节；长度候选要求每个已观测输入上传恰有一个参数关系。范围限于已资格化的完整冷Prefill、同请求数、连续INT64 staging。描述工作量不授予前缀等待或纯CPU成本资格。

**同步复制的来源工作与参数轴已接入，完整目标资格仍按独立结构验收。** `bind_transfers`现将已归属的同步driver复制绑定到同rank/线程、起止与copy_interval精确一致的唯一CPU服务，保留方向、字节及API来源；不增加DMA，也不把外层准备/等待包络计作复制。输入上传仍须通过原有地址、scope与字节资格检查。已有事实在s3-synchronous-copy-work重提取后，134次同步复制全部绑定，工作描述由42增至166，其余图字段及原有描述完全一致。记录源字节不等于目标参数关系：生产端现为新增124项生成显式transfer_layout，120项为请求数轴、4项为过滤保留人数轴；消费者仍拒绝未绑定工作，不新增固定字节兜底。

原生机制提供三类参数来源：`ScheduleBatch.prepare_for_extend`及`alloc_for_extend`构造请求级长度/索引张量；`ForwardBatch.init_new`将`extend_seq_lens`和`extend_prefix_lens`两个INT32列表转到设备；`ScheduleBatch.filter_batch`在实际部分保留分支上传INT64的`keep_indices`。后两者的大小分别由当前请求数和保留人数确定，但仍须将每次复制与其真实张量/分支关联。Prepare内还包含`write_cache_indices`的UINT64前缀指针向量，以及`SamplingBatchInfo.from_schedule_batch`的temperature/top_p/top_k/min_p请求级数组；不能将这些工作误归为长度张量。当前来源134次同步复制均可经唯一Dequeue、async_task_queue及包含它的aten::copy_关联；源/目标张量一维形状和类型相同，元素数乘类型字节与实际驱动字节全部一致，见source_copy_tensor_provenance.json。生产绑定复用该身份链，参数轴按上述原生构造语义声明，不按第几次复制或相同字节数赋予身份；未知参数来源保持缺口。原固定资产的未绑定数量及观测分组见[独立对照审查](../redesign-workflow/2026-10-10_architecture_review.md#独立原生对照事件依赖保留cpu执行缓存资格仍缺)。

s3-input-upload-binding复用原采集完成重提取与给定100 ns下的开发验证：双配置完整24/28阶段，4/6次上传的目标字节全部等于目标输入token总量×8，且全部不同于来源字节；证据为source_binding_verification.json、target_binding_verification.json。该结果针对此前外层包络绑定，只验证上传工作参数，不作为当前驱动分段的完整运行证据。

**内部等待的结构边界**：现有执行核按依赖取最大完成时间，已能表达CPU准备与设备完成共同约束后续工作。但不能把全部前缀压成“CPU服务→驱动入口处的等待”：若等待前CPU工作为A、等待后至驱动入口的CPU工作为B、前序设备完成为D，简化模型给出max(A+B,D)，按顺序分段则为max(A,D)+B，二者一般不同。A/B是给定服务耗时符号，不是由日志拟合的常数。同步成功退出后仍进入MemCopySync再进入驱动，不能默认B为零或用线程CPU总量分配A/B。目标依赖须经SourceStreamWait/TemplateStreamWait重绑资源前沿，单独保存来源设备边不能覆盖跨模板重组。

内部同步的观测切口已在s3-copy-runtime-level确认：安装版torch_npu的Level2比Level1增加ACL_PROF_RUNTIME_API和ACL_PROF_AICPU；同一四场景中新增47条runtime记录，两次同步驱动H2D分别有唯一同线程、父调用内的Runtime@StreamSynchronize，结束均早于驱动开始，两个异步对照没有内部同步。正式归一化仍得到2同步/2异步和4次CPU关联。提取层现按已关联复制API的同rank/thread及完整包含关系选择唯一内部同步，复用HostWait和流前沿，再由公共CPU分段保留同步前后的服务；缺失或多义时保留前缀缺口。显式同步内部的Runtime观察不重复生成等待。该实现已编译，完整模型来源提取已运行，134个内部等待全部保留在serving模板中；目标展开的开发核验见下文。SGLang原生默认Level1；自有observer通过原生hook在_start_profile的ContextVar作用域内把_ExperimentalConfig设为Level2，退出恢复作用域，其他profile构造不受影响。未改变官方框架文件，s3-runtime-wait-capture完整采集/提取已完成：134次同步H2D均有等待及流资源，无CPU服务覆盖等待区间，10次输入transfer匹配驱动边界及字节，详见source_wait_verification.json。s4-runtime-wait-expansion用固定新资产在两配置展开24/28阶段，56/82个复制等待均保留并完成资源绑定；4/6次上传字节和等待→复制→kernel路径通过。统一服务100 ns与CPU100 ns/设备10000 ns两条件均通过，后者分别12/18次等待为正（最大17900 ns）。这只验证给定耗时下的结构与等待反馈，140项来源设备/等待时钟矛盾及独立泛化资格保留。同步包络自身的CPU开销与成本资格仍不由边界观测解决。

同版本运行时分支取证见s3-input-upload-runtime-diagnostic：未锁页4/2040字节在rtMemcpyAsync同线程调用窗口内依次出现pageable判定、StreamSynchronize进入/成功退出、MemCopySync；锁页对照走异步任务构造。此证据说明已观测路径的内部流等待不能当作纯CPU服务。Level1 profiler只暴露外层Dequeue和AscendCL区间，没有内部同步切点；现有CPU transfer仅交付工作量，等待前沿与同步复制服务仍须分开建模，不能以外层API总墙钟拟合成本。调试日志仅用于机制取证，不成为正式采集的常开要求。历史copy_clock_verification.json记录一个同步结束点晚于父区间5520 ns；审查确认来自时间坐标混用。按导出器原始元数据统一后，四API与两个驱动子区间均在父内，见export_clock_domain_audit.json、copy_clock_verification_current.json。此项不解决设备计数器频率或内部等待，也不授予当前驱动分段的完整目标运行资格。

原生采集现为rtMemcpyAsync保存driver_copies，记录同线程、动态嵌套的drvMemcpy源/目的地址、字节、单调时钟起止、线程CPU时间和返回值。通过RAII限定父调用生命周期，窗口外及没有父复制的驱动调用不记录；不增加同步操作、数据回读或常开debug日志。驱动头文件声明该接口同步，因此结束点可作为此驱动复制返回完成的观测；入口仅证明进入复制前的运行时等待已结束，不能把父API入口至此的全部包络称为纯等待。s3-driver-copy-boundary四格诊断验证两次未锁页调用各有一个匹配子记录，锁页对照为空；采集引用显式包含CANN host/start_info。归一化采用安装导出器的collectionTimeBegin×1000−clockMonotonicRaw，把native单调时间统一到trace_interval，驱动子调用形成copy_interval；原始wall_time_ns仅作溯源，不能混作profiler坐标。归一化已校验子调用嵌套区间，并将唯一成功且地址/字节一致的驱动复制标为copy_execution=synchronous_driver；具有合格硬件成员的复制标为asynchronous_device，其余保留未解析。前者不使用可能残留的last-task身份制造DMA或误报类型冲突。四格复用归一化得到2/2分类及4次CPU关联，无任务身份缺口，见normalization_verification.json。提取层已消费配对时钟转换后的copy_interval，在公共CPU分段中加入驱动起止切点；输入上传字节仅绑定与驱动区间精确一致的服务。若区间不在已关联profiler父调用内，报告driver_copy_parent_clock_disagrees，不裁剪/平移。复制前缀仍保留并报告synchronous_copy_preparation_and_wait_not_separated，HostWait分离未完成；旧观测不能补造driver_copies/copy_execution。

同步复制的目标参数由生产端`SGLangCopyLayout`解释：沿driver服务的Dequeue → async_task_queue → Enqueue → 唯一aten::copy_取得一维源/目标张量，要求形状、类型及计算字节一致。冷Prefill准备中的请求数组和model_forward直接子调用的INT32元数据使用请求数轴；filter_batch部分保留分支的INT64索引使用已资格化过滤角色的保留人数轴。前者按目标批人数求值，整批结构资格另行检查；后者明确区分transition与running update，不使用随后选中批次的人数。输入token上传继续使用原有地址核对和token轴。无唯一证据时不绑定参数，不按复制序号猜字段。

核心仅接收元素大小及通用extent；过滤空/全保留分支不能通过把字节改为零沿用原复制。生产端读取原始事实一次后释放，正式预测仍只读取编译资产。这一绑定不授予未见批大小、prefix cache或任意框架执行分支资格；公开predict的双配置开发验证完整24/28阶段，72/102项复制的逐serving调用方向、字节和次数与已有原生观测一致；除transfer描述外，目标图与原固定资产展开全部字段相同。证据见s3-synchronous-copy-work的transfer_parameter_bindings、parameter_expansion_verification及base/budget_parameter_copy_comparison；成本和完整独立结构资格仍未授予。

### 贪心采样与 logits 输出的参数关系

原生 Sampler.forward 的普通 greedy 路径对 next_token_logits 的最后一维执行 argmax。当前 model_forward 采集范围包含该工作；其唯一直接 logits 子区域已通过 logits_selection 的 pruned_states 证明每请求一行，不能用 prefill 总 token 数作为采样行数。

模板提取先完成已有 logits 布局绑定，再按原生父调用查找唯一已绑定的 FLOAT logits 输出。ModelKernelLayout.bind_sampling 从该输出取得 B 与观测词表宽度，核对 ArgMax 的完整 [B,V]、标量 axis 描述及 [B] 输出，以及随后的 INT32→INT64 Cast。仅请求轴可变，词表轴保持当前固定模型和配置的来源值；缺少唯一来源或操作数不符时不绑定，不猜默认宽度。两个节点沿用源依赖，不增加同步或另一套采样实现。

现有资产下两 rank、五批共增加 20 个布局，覆盖 prefill 的 T≠B 及 decode 的 B=1/2；此前布局、完整物理图、执行摘要和缺口不变，未补采。证据在 data/redesign/s3-greedy-sampling/sampling_audit.json。该关系未证明设备内核跨形状分解、随机采样、logprob 或跨 TP 资格，也不生成实际 token 内容；请求长度继续由输入合同给定。全图精确输入门槛保持。

### NPU decode 分配的有限参数关系

PagedKvLayout 直接消费 alloc_decode 区域的原生类型、CPU seq_lens、页大小和空闲页布局。源新增页数按原生条件 seq_len % page_size == 1 计算，目标普通 decode 则由 context_tokens % page_size == 0 得到；每请求必须新增一个 token。零新增页与非零新增页是不同结构分支，当前只允许同分支参数替换，不通过更改 shape 跨分支复用节点集合。排序、分组状态异常或需要合并/容量不足的路径明确返回规则缺口。

区域内先核对操作、dtype 和完整操作数形状，再把请求向量轴绑定到目标 B。BroadcastTo 的描述槽、Cumsum 的 axis 槽、Index 的两个描述槽保持固定；free_pages 长度来自目标分配前状态，不能因源数值恰好等于 B 或请求池行数而误绑定。未知操作返回 operand_unmapped。allocator 区域优先于通用请求池匹配，防止空闲页 Index 被另一种相似布局解释。

零页路径的最终 INT32 Add、非零页路径的最终 INT64→INT32 Cast 附带目标 kv_allocation，复用 prefill 的逐请求页、prefix 末槽和写入范围描述；内部中间节点不被标作完整分配结果。当前源记录中两种路径分别有 9/18 个直接 kernel，这是观测结果，不作为硬编码节点数量或目标选择条件。

这项关系不放开全图的精确输入门槛，也不证明改变 B 后 kernel launch、节点集合与端口仍相同。分支改写及整个 serving 图的泛化另行资格化，不能由局部布局映射推定支持。

### 采样前处理的原生区域

sglang.sampling 绑定 ModelRunner.sample，复用 NativeRegionWrapper；component_owner=model_runner、component_path=. 表示 owner 本身。记录 logits_output.next_token_logits、forward_batch.seq_lens/positions 的布局，forward_mode、return_logprob、is_all_greedy，以及实际返回 next_token_ids 的布局，不读取设备值。

原生 sample 在 Prefill 中向 sampler 传入 seq_lens−1，在 Decode 中直接传 positions；该调用还包含 logits 预处理与可选后处理。因此该范围是来源身份，不是把整段固定为一个 kernel 或一种成本。生产端通过父 model_forward 的唯一合格 logits 输出绑定 greedy 操作，并检查新区域的 greedy/logprob 条件；其他路径不默认复用。现有粗 scope 记录不能被回填成已观测的采样区域，Prefill Sub 的参数和值关系仍需实际来源验证。生产实现已增加受限 SamplingPositions 关系：要求 Prefill、greedy 且不返回 logprob、真实 sampling 区域内唯一 Sub、INT64 请求向量减标量，以及记录的 CPU seq_lens 与来源请求 context+input 完全一致。满足后编译为公共 LastPositions 工作量，按目标请求生成每请求一个 context+input−1 位置；请求轴变化由 TensorParameterLayout 处理，标量槽保留。该关系复用 PositionWorkload 的紧凑区间表达，不构造逐 token 数组。新采样记录同时声明 CPU seq_lens，旧粗 scope 不能据此补造资格。

公开 prepare、Python 合同加载与官方方法签名检查通过，证据见 data/redesign/s3-sampling-region/contract_audit.json。后续观测须确认区域父子身份、实际输入与输出请求轴、长度偏移的直属关系及预处理子路径；与其他公共元数据合同合并规划一次有界观测，不逐项启动独立补采。结构合格后再决定性能单元粒度。

2026-10-10 已用一次真实短 workload 合并验证 metadata 与 sampling：两类区域各 10 次，父 model_forward、rank、CPU 长度和采样输出请求轴均一致；4 个 Prefill Sub 获得 SamplingPositions 关系，目标末位置分别为两请求 [126,127] 与单请求 [15]（两 rank）。源 region/kernel 身份来自新原生记录，不回填旧资产。证据在 data/redesign/s3-metadata-source-evidence；仍不证明新的请求长度/配置下结构完整，metadata 的序列 Cast 等 42 个节点仍未映射。

### Attention metadata 的共同区域观测

生产合同新增 sglang.npu.attention.metadata，绑定 AscendAttnBackend.init_forward_metadata。该区域属于模型前处理，不新增 serving block。component_owner 明确组件从 batch、model_runner 或 scheduler 取得；原有 allocator 绑定属于 batch，metadata 通过 model_runner.attn_backend 安装。公共 NativeRegionWrapper 支持按声明的嵌套参数路径读取输入，同一路径作为记录键，框架侧不复制 wrapper 生命周期。

当前声明 forward_batch.seq_lens、req_pool_indices、extend_seq_lens 的布局，seq_lens_cpu 的 CPU 值，forward_mode 以及 backend 的 page_size/is_hybrid_swa。布局读取不访问设备值；只有明确 CPU 输入可以转换为列表，设备输入误配为 CPU 时直接报错，不执行设备回拷。生产端保留原页表参数编译器对该区域的处理；新增区域不意味着其中任意 Cast 都已建立语义关系。

公开 prepare 到 Python 合同加载、原生方法签名核对已通过（data/redesign/s3-metadata-region/contract_audit.json）；随后的一次合并观测见 data/redesign/s3-metadata-source-evidence/native_region_audit.json。后续观测必须证明该调用的父 model_forward、batch/rank 身份、输入 B 与长度、直属页表/Cast 的因果归属和新增开销；取得这些关系即结束，不遍历请求长度建库。应先整合其他元数据与 Schedule 观测合同，再决定一次有界采集，不为本绑定单独启动补采循环。当前旧资产仍只有粗粒度来源，不回填未经观测的区域身份。

### NPU decode 分组释放的参数关系

普通 decode 的逐请求 free 只积累索引，free_group_end 对非空列表拼接，再嵌套调用真正的 free。PagedKvLayout.release 依据此父子关系，分别处理 token 索引拼接、去重后页索引的类型转换、页集合归还；空组不补造节点。模板区域仍是包含子区域的视图，TemplateNode.source_scope 保留最内层原生调用身份，组件变换只处理直属节点，防止父级规则再次处理内层工作。

目标 KV 工作量由公共 KvBatchWork 从 BatchInput 与页大小计算，供 extend、decode 和分组释放共用。新增页数是逐请求 ceil((context+input)/page_size) − ceil(context/page_size) 的和；单 token decode 下等价于 context 是否落在页边界。分组释放前的空闲页为 Prepare 前空闲页减去全批新增页，不能只扣完成请求的分配。归还 token 数来自完成成员长度，归还页数及身份来自它们的目标 kv_pages。

该对象只借用当前调用输入，生成分配/释放工作量描述，不保存第二套 allocator 状态或模拟时钟。通用 KvParameterLayout 在容量和分支条件通过后生成描述；原生类型、源操作数和分支依据由生产端在建库时验证，再编译成目标参数轴与适用域。

结构装配先建立统一的局部节点、区域及端口身份，再调用 SGLangTemplateBindings 解释来源区域的原生模块记录。原生绑定识别不放在 TemplateDefinition.read_structure 中，后续模型、通信与 KV 参数编译共用生产端解释后的区域。

SGLangTemplateBindings 在建库时调用 PagedKvLayout.bind，按照最内层区域识别 extend、decode 或 decode 发布中的分组释放，并核对内层 free 的父调用。源完成成员同时从 publication 事实绑定；缺失 publication 与确实没有完成成员分别表达，不能把缺失默认为空组。已识别分配 kernel 缺少区域时保留来源缺口。

模板保存统一 KvParameterLayout 值合同，包含输入/输出槽的工作量轴、新增页上限、是否分配新页、释放成员数、释放前自由页的空/非空条件及结果描述类型。合同不保存原生区域索引或指向生产对象的指针。实例化直接使用 BatchInput 求值，不再搜索原生方法、读取来源完成成员或调用 PagedKvLayout。来源布局错误仍保留具体原生诊断；目标域外条件使用通用 kv_* 缺口，不设置旧错误名兼容分支。

该迁移保持既有适用域：prefill 的新增页上限来自原生 200 页分支；decode 的零/非零分配、释放成员数及自由页分支改变仍需结构变换。实际资产的完整预测 JSON 与物理图和迁移前一致，无新增采集，证据在 data/redesign/s3-kv-parameter-contract/parameter_audit.json。此验证没有覆盖所有域外路径，也不放开全图精确输入门槛；不能将值合同交接当作完整跨配置结构验收。

源完成请求由原生 publication 记录确定，逐请求序列长度为 context+input，冷 KV 下页数为各请求 ceil(length/page_size) 之和。目标从完成请求的页预约取得归还集合；释放前空闲页数由 BatchInput.free_kv_pages 扣除本次 Prepare 为全部请求新增的页数得到。group_end 的 Concat 保留各请求长度槽，内层 free 的 Cast 使用归还页数，最终 Concat 使用归还页数与剩余空闲页数。只有最后一个节点附带 kv_release 描述，不重复计费，也不维护另一份控制状态。

现有资产两 rank 共核对 4 个 Cast、6 个 Concat；4 个最终归还节点的页集合、槽位和计数与目标完成请求一致。首次归还后的空闲页数为 127，最后为 128；前一次归还到后续复用页的 6 个分配结果节点均有实际依赖路径。移除新增 kv_release 描述后，整个物理图与上一轮相同，已有布局、覆盖值、执行摘要和缺口不变。证据在 data/redesign/s3-grouped-release/release_audit.json，未补采。

该规则限定 decode、冷 KV、need_sort=false、已有完成成员结构，尚未支持改变完成成员数量或空/非空自由页分支改写；Prefill 独立释放见后文。CPU unique、其他拷贝工作量、跨形状 launch 与完整依赖仍需资格化；目标页 ID 不是原生设备页 ID 的直接观测。Published 也不代表设备归还完成，复用就绪由真实设备依赖约束，详见[目标 KV 页身份](05_controller_semantics.md#目标-kv-页身份)。

### 完成发布的输出工作边界

输出 token 数变化不能用相同 finished 标志或 CPU 节点数量直接放行。已安装 SGLang 的 _GenerationStreamAccumulator.accept 在输出时维护两份列表：output_ids 为未发送生成 token；decode_ids 首次输出带 prompt 尾部，后续输出追加新生成部分。init_incremental_detokenize 的原生尾部上限为 5，属于框架算法常量而非拟合参数；核心不能持有这个 SGLang 常量或原生对象。SenderWrapper.send_output 在 socket 存在时调用 send_pyobj，token 元素数不等于序列化后的字节数，也不能用元素数线性缩放整段 CPU 耗时。

历史资产的 batch_output 区域只保存 is_generation、return_logprob 及调用范围；Publish 另保存输出周期、非流式状态及最终生成数。新 SGLangOutputWrapper 继承 NativeRegionWrapper，在同一区域的 result.output_work 中保存真实解码上下文上限、输出周期、sender_enabled 和按请求 before/after 状态：生成长度、完成/已输出状态、finished_len、发送 offset、解码 offset、文本字符数及流式/可选载荷标志。只读取 CPU 元数据，不复制 token、不二次序列化、不调用推进状态的方法。另记录 speculative、disaggregation_mode 和 request_time_stats，以排除相应附加路径。通用提取保留区域 result；run-20261011-053447-612772 的 30 条输出记录已逐条核对 raw/source_facts/templates 一致，旧资产不补默认字段。已有真实 output 区域可复用，不应为不同输出长度另建 wrapper 或模板。后续参数合同必须区分已记录事实、由固定模式推导的计数、尚未观测的运行状态：记录/推导依据不成立时保留缺口，不给历史资产补默认 offset、socket 或可选返回字段状态。

实现先表达每请求是否输出、首次/后续输出、结束分支、生成 token 增量与解码上下文工作，再以批次记录数交给共享输出区域。这些是目标工作描述，只有控制器维护生成进度；不得增加第二套请求状态。框架生产端解释原生切片、可选载荷及发送分支，共同模板只消费结构条件和参数。节点/端口保持与成本资格分开：给定耗时可用于结构展开，但不能因此宣称打包、序列化的性能模型成立。

PublicationParameterLayout 表达非空目标请求列表的普通非流式输出；来源人数由完整模板资格维护，不在工作量layout内重复保存。生产端仍以首次输出的 before/after 观测资格化共享区域：无 logprob/可选载荷/推测/分离/额外统计路径，原始发送 offset 为零、解码 offset 未初始化，调用后状态吻合；区域必须单入口/出口、纯 CPU 且只有一个服务节点。后续增量关系依据上述原生实现，而不是声称来源已观测周期输出。

PeriodicOutputPolicy 是输出周期算术的唯一维护者。令 g 为本步前已生成数、I 为来源有效周期，本步完成或 (g+1) 整除 I 时输出，已发送数为 floor(g/I)×I，新增输出为 g+1−已发送数。核心从 processed_sequence−g 得到 prompt 长度，用来源上下文上限计算首次解码尾部；后续只发送新增 token，解码 read offset 保持该尾部长度。PublicationWork 按目标顺序记录 generated_tokens、emits、output_tokens、decode_context_tokens、previous_output_tokens、decode_read_offset_tokens、finishes 和派生 decode_tokens。生成进度仍由控制器唯一维护，周期发送不等于请求完成或 KV 释放。

共享服务保留成员扫描及非空请求批的载荷构造，包括零输出成员；工作量绑定原有 CPU 服务，若该节点被拓扑替换则拒绝。成员完成/继续分支、聚合释放和端口分别通过原有计划资格，不因输出参数可计算而放行。整批请求数变化、流式与可选载荷未获支持；共享工作参数不再保存来源人数，不能由局部参数可计算推断完整拓扑合格。token 数不能推导序列化字节或 CPU 耗时。

此前首次完成输出开发证据见 s4-publication-membership/{six-request,run}/verification.json 与 s4-publication-output/verification.json，覆盖零/一/两个输出成员以及来源 6-token 到目标 2/4-token 变换。固定资产跨周期/跨页开发验证见 s4-periodic-output-page-boundary/verification.json：128输入/131输出完成131批，两个rank分别在进度50/100/131输出50/50/31 token，首次解码尾部5、后续不重复携带；Decode context覆盖128…257，每rank每步64个attention节点的目标长度正确，最终258-token序列释放3页，全图依赖时间有序。没有新增模板或采集；未独立核对每次页分配内部操作，且不等于全容量域、独立原生结构或S5成本验收。

### 释放组的单成员传输

Decode 完成发布中的 free_group_end 先聚合 token 索引，再交给 free 归还物理页。单个完成成员的 aten::cat 在当前 NPU 路径产生 D2D，而多成员走既有 Concat kernel；不能把单成员拷贝大小当作与长度无关。生产端要求完成成员数已知且为一、Cat 设备归属唯一且直接位于释放组、嵌套 free 的输入为连续 INT32 向量，长度与完成成员的 context + input 一致，原始传输方向和字节数同时吻合。ATen TensorList 未给内部形状时，使用这个真实嵌套调用描述，不从字节数反推语义。

统一 TransferParameterLayout 替换原固定请求数表，表达固定请求数下不变的传输与完成序列 token 数传输。核心根据目标完成成员计算字节数，保持方向和来源归属；请求数、完成成员数和 Decode phase 仍受约束。保留节点与插入节点都拥有目标传输描述，执行图消费目标值，诊断分别保留观测值和变换值。此关系只描述工作量，不估计传输耗时，不放宽完成发布的输出策略资格。


### Prefill 页分配的完整参数关系

NPU alloc_extend 的直属区域统一处理新增页数计算、分配 kernel 和返回类型转换，替代此前只匹配 alloc_extend_kernel 的通用变换。来源 CPU prefix/seq 长度必须与所属请求一致；新增页数为逐请求 ceil(seq/page_size) − ceil(prefix/page_size) 之和。目标使用自身请求和页状态求值，ReduceSum 保持标量输出并携带 kv_new_pages，不把标量改成请求向量。

Add/FloorDiv/Sub 的请求轴、ReduceSum 的固定归约描述槽、分配 kernel 的空闲页轴与 token 输出轴分别核对。最终 kv_allocation 标在返回 INT32 索引的 Cast 上，内部 INT64 kernel 不再冒充最终返回值。规则按真实组件身份应用；失去 allocator scope 的分配 kernel 返回缺口，不保留通用猜测路径。

当前限定无需排序、非释放组、空闲页充足且新增页数小于 200 的官方分支；200 来自原生实现，非拟合阈值。显式 num_new_pages 参数路径、naive 分支、跨形状 launch 与完整依赖仍未资格化。CPU 长度推导不是对设备 .item() 返回值的直接观测，也不解决 CPU/设备时钟矛盾。

既有资产下增加 28 个操作数覆盖，四个 ReduceSum 的新增页数分别为各 rank 首批 2 页、后续 prefill 1 页；四份分配结果从内部 kernel 移至最终 Cast，内容相同。移除新标量和这项归属调整后完整物理图不变，原有其他布局、覆盖值、执行摘要及缺口不变。证据在 data/redesign/s3-extend-allocation/extend_audit.json，未新增采集；lower-admission 仍 unsupported，精确输入门槛保持。

### 模型最终归一化的维度来源

Qwen3Model 继承 Qwen2Model 的最终 RMSNorm，位于 model_body scope；embedding 和最终 norm 都由同一 config.hidden_size 构造。当前通过唯一直接 embedding 子区域的真实 hidden_size 及 token 输入布局取得宽度依据，再复用逐层/QKV 已有的 bind_norm 操作数核对和轴实例化。参数子区域按父调用身份与语义索引，logits selection 继续使用同一唯一子区域查找机制，不按相似 kernel 维度反推模型宽度。

最终归一化记作 model_output_norm，保留激活、残差、权重以及归一化值、逆 RMS、残差三个实际输出的角色；权重轴固定，token 轴使用目标 T。子区域缺失或不唯一时不生成这项参数关系；直接 input_embeds 或跨 PP 阶段没有该 embedding 证据时，不由当前规则推定支持。该语义标识不要求另建性能模型或新模板族。

### Attention 页表构造与读取

当前非 graph 的 AscendAttnBackend.init_forward_metadata 从请求表取当前请求的前 L 列，按 page_size 跨步选择页首 token 位置，再整除页大小得到页号。这里 B 为当前请求数、L=max(context_tokens+input_tokens)、C=ceil(L/page_size)，不同于模型 token 总数 T。规则依据官方实现及已有原生 model_forward scope，不能把任意同名 FloorDiv 都当作页表构造。

该流程的 seq_lens.max() 同样属于参数关系：model_forward 中的 INT64 ReduceMax 消费 [B] 长度向量及固定 [1] 的归约描述槽，输出保持 rank-0 INT64。SequenceLengthMaximum 只替换请求轴，目标 sequence_length_max.tokens 由 max(context_tokens+input_tokens) 求得；不把标量改成 [B]，也不复制源最大长度或以 sum(input_tokens) 替代。该值与页表列数、请求表裁剪长度使用同一个目标序列长度定义。标量描述不代表已经解决原生设备到 CPU 的同步和时钟资格。

ModelKernelLayout 的显式轴类型增加 max_sequence_tokens、page_columns、request_sequence_storage，分别表示 L、C、B×L；原有 token/request 轴继续使用同一求值过程。核对完整 dtype/操作数后，Cast 和 FloorDiv 使用 [B,C]，Slice 从 [B,L] 产生 [B,C]，AsStrided 的原始存储长度为 B×L、输出为 [B,C]，视图步长关系为 [L,page_size]。axis/shape/stride 描述 tensor 的槽长度保持原生常量，不能随 B 数值一起改变。

Slice/AsStrided 的存在随视图连续性变化：当前观察到 C=1、B>1 时的 Slice，以及 C>1 时的 AsStrided；C=1、B=1 不需要这些物化节点。关系描述保留适用条件，全图精确输入门槛仍在，不把 shape 关系当作已实现节点增删和依赖重接。

最终 INT64→INT32 Cast 的 page_table 描述来自目标请求槽和 KV 页状态，包含列数及每请求有效页号。短请求在 C 列矩形中的其余值保持 unspecified，不能伪造 padding 页内容；attention 的有效访问由各请求真实序列长度约束。中间 token 位置视图和 INT64 运算不被标作最终页表。

已有资产下核对 38 个布局关系：Slice 2、AsStrided 6、Cast 20、FloorDiv 10；10 个页表输出与 640 个 attention 读取的有效页、列数及目标因果路径一致。移除新增 page_table 值描述后物理图与此前完全相同，执行摘要不变。证据在 data/redesign/s3-page-table-layout，未新增采集或成本拟合。跨形状 kernel launch、物化节点改写及完整依赖资格仍待完成。

### 区域边界包含逻辑完成点

语义区域包含归属明确的工作及其必要逻辑连接。SourceGraph.complete 产生的 completion_join 没有服务 span；区域提取必须按全部完成前驱的共同归属将它纳入区域，不能仅收集 span 起止点。任务内部的 stream_order 因而保留为内部关系；跨区域汇合只属于前驱共有的父区域，不能以其中一个前驱的身份吞入子区域。此规则复用已有完成边，不新增工作、屏障或耗时，也不按时间包络猜归属。

现有资产重提取修正 3,262 个区域的输入/输出切口和内部边计数，源物理图逐字节一致，外层 serving 模板与区域成员保持不变。输出端口数量不要求单调减少：任务完成点连接外部消费者时必须显式保留，即使该区域仍另有到外层完成汇合的输出。证据在 data/redesign/s3-topology-boundaries/region_cut_audit.json。

RoPE 分支审阅还表明：已有 position_gather/tile/slice 位于首层 qkv_preparation，而非独立的 get_cos_sin_with_position 观测区域。T=2 时直属工作有 MatMul、Gather、Tile、两次 Slice 和融合 QKV/RMSNorm/RoPE；T=1 时少两次 Slice。初次边界审阅时，该 scope 下没有 CPU 服务 span，CPU 服务仅按外层 serving 身份提取；后续已按下节显式调用栈补齐控制线程内层归属。故不能直接把整个区域视作两次连续化操作，也不能只删设备 Slice 就声明完成分支变换。下一步须明确相关 CPU 提交、数据输入/输出和共享位置结果的跨区域关系，才可选择替换子区域或改写节点；此次逻辑边界修正不解除精确输入门槛。

### Workspace 容量状态与设备级同步（待实现）

workspace 是按 device/stream 共享的原生缓冲区，与 KV 页池分开。目标结构取决于初始化后的容量、目标操作需求和之前的分配效果，不能按“首次 Decode”或某个人数直接选择扩容分支。当前来源绑定、有限需求计算、allocator CPU 切口以及同配置容量推进已接入；目标分支替换、跨配置初值资格及完整设备同步域尚未完成。当前缺口与整改顺序见[整体审查](../redesign-workflow/2026-10-12_workspace_state_pipeline_review.md)。

**原生机制与观测边界。** 安装 torch_npu 2.10.0 提交 `94f8a8e6b523d7ba553e1b80d5b5248478391526` 的 [WorkspaceAllocator](https://github.com/Ascend/pytorch/blob/94f8a8e6b523d7ba553e1b80d5b5248478391526/torch_npu/csrc/core/npu/NPUWorkspaceAllocator.cpp#L69) 对 cached 请求加32字节，新容量按2MiB对齐；旧分配非空且不足时先同步设备，再释放和重分配。首次分配、复用和清空不因此产生该显式同步。地址可能在扩容前后相同，分支须根据需求与容量判断。这些常量来自原生机制，不是拟合。

op-plugin 提交 `dedc316708372c9a8bfde4527abd2a94b74840f5` 的 [OperationSetup](https://github.com/Ascend/op-plugin/blob/dedc316708372c9a8bfde4527abd2a94b74840f5/op_plugin/utils/custom_functions/atb/AtbCommon.cpp#L199) 从 ATB Setup 取得需求，在非 graph、未显式传 workspace 路径调用共享分配器。ACLNN 普通执行及 executor cache 也使用该分配器；只拦截 ATB 不足以恢复容量链。带 stream 的 unsafe_empty_workspace 使用共享 workspace allocator，不带 stream 的重载使用普通缓存分配器。完整调用范围证据为 `data/redesign/s4-workspace-snapshot/allocator_call_coverage.json`。上游内部符号绑定使 LD_PRELOAD 不能覆盖全部入口，因此源级观察补丁随 runtime 适配器维护，位置及安装约束见 [torch_npu 适配说明](../../src/collection/adapters/runtimes/ascend/torch_npu/README.md)。

**采集合同。** WorkspaceCapture 在 warmup 后、profiler 前读取初值，停止后读取终值；短暂 state-only history 必须恢复原设置。使用 memory._snapshot 中明确的 workspace_snapshot，不按普通 segments 大小猜测。每次记录 device、stream handle、请求字节、前后容量与地址、策略、API 和语义身份；最终容量须与窗口快照一致。MarkovWorkspaceAllocate/Release 使用原生时钟和线程身份，写独立 markov.workspace 事件文件并在停止后合并，避免两个写入器共用 torch.op_range 导致 TLV 损坏。worker 归属复用已有队列 flow。失败、缺窗口、未知安装来源和未资格化 uncached 生命周期保留缺口；历史数据不补造空缓存。隔离四分支、安装加载及 serving 窗口证据分别在 s4-workspace-probe-runtime、s4-workspace-serving；隔离 mm 未触发分配，不能据此宣布 ACLNN 正向覆盖，完整 Dockerfile 重建也尚未验收。

SGLang 环境记录实际 fresh_process、warmup 输入长度、调用共用的 sampling_parameters 和 after_warmup_before_profile 快照位置。统一预热起点及当前固定配方见[输入合同](02_user_contract.md#仿真起点统一预热后开始)。历史采集按各自实际记录判断，不能视为已采用新配方；C++ 生产端现消费该记录核对固定配方，编译资产保存 initialization_gap 并在来源选择时拒绝不合格记录；尚未由此计算或推进目标初始容量。KV=cold 不代表 workspace 为空；目标不能无条件继承来源容量。初始化本身可在预测时延范围外，但其产生的状态必须作为有资格的入口条件。

**来源与模板合同。** WorkspaceAllocation 先按固定原生提交校验分配分支，派生 workspace_allocation 与原始记录分开。AscendWorkspaceBindings 再按 source_interval → marker → 同 scope/lane 的包含 worker → 已由 flow 关联的 host operation 绑定。stream handle 关联快照，物理 stream ID 关联模板资源，不互代。TemplateSource 按资源保存初值；TemplateWorkspaceUse 保存操作/worker、API/marker、局部边界、需求与来源分支。歧义或缺失产生 workspace_binding_gap。编译资产只维护当前合同，变化后重新 compile-templates，无旧格式回退。

固定单token预热的真实来源见 `data/redesign/s4-fixed-warmup/native_workspace_verification.json`：当前pool-four两rank的窗口前容量均190840832，业务中各一次grow至197132288及319次reuse，640个需求规则和两个设备等待资源域已交接，公开预测通过配方资格后报告node_durations_missing。该数值只属于本次配置的观测，不是跨配置常量；未运行目标状态推进，也不是独立留出验收。

**有限目标需求。** 实际 ATB 为9.0.0.B160，提交 `8bf8f4054d644f14d3924a7f0b4d0467ef5cfa52`，宿主 ATB 8.5 不作为依据。[PagedAttention tiling](https://gitcode.com/cann/ascend-transformer-boost/blob/8bf8f4054d644f14d3924a7f0b4d0467ef5cfa52/src/kernels/mixkernels/pagedattention/tiling/paged_attention_tiling.cpp) 计算八段 scratch，[分核规则](https://gitcode.com/cann/ascend-transformer-boost/blob/8bf8f4054d644f14d3924a7f0b4d0467ef5cfa52/src/kernels/mixkernels/pagedattention/tiling/paged_attention_tiling_dependency.cpp) 依赖真实核心数、batch、head、dimension 和 KV 长度，Runner 最终按512字节对齐。当前只编译普通 BN 域：每请求一个 query token，batch × heads ≥ 0.8 × 核心数，最大 KV 长度 < 256 × 核心数，head dimension ≤256；heads=32/batch=16 的 group_size=8 是原生分支。长序列 BNS、量化、MLA、压缩 head 不放行。

[UnpadFlashAttention tiling](https://gitcode.com/cann/ascend-transformer-boost/blob/8bf8f4054d644f14d3924a7f0b4d0467ef5cfa52/src/kernels/mixkernels/unpad_flash_attention/tiling/flash_attention_tiling.cpp) 的非 relay、非 splitm scratch 比例为1/1/6/1，单段为 blockDim ×32768×8×sizeof(float)。当前 prefix-cache encoder 仅支持 query_heads ≥ 核心数：非空请求已足够使用全部核心，不进入独立 encoder splitm 路径。设备上限来自 profiler device_info，不从来源 Block Num 推定。公式分项和原生验证见 `data/redesign/s3-workspace-allocator/native_demand_derivation.json`；MKI 公共源码仅作辅助阅读，未作为安装提交证据。

生产端按安装提交、操作与设备事实建立 WorkspaceDemandLayout，核对来源字节后发布。核心只计算分组、线性/二次缓冲与对齐，保持 float32 中间乘法和 uint32 截断，不解析 ATB 名称。目标实例按 BatchInput 求值；域外返回 workspace_partition_*，无规则保留 null，不能当作零。TemplateInstance 在全部拓扑变换后按实际保留及插入的 marker 绑定 workspace_uses，保存各自来源、使用点与重定位后的边界，再计算目标需求；不按来源列表顺序推进容量。明细 workspace_demands 输出这些实际使用点。已有640个保留使用点回归通过，删除/插入 allocator 路径尚无独立执行验证，见 s4-workspace-growth/workspace_use_verification.json。source_binding、demand_rules 不代表 target_state 合格。

**分支装配与执行边界。** allocator marker 起止必须是 CPU 服务切口，避免替换后留下跨界的旧分配服务；AscendSourceExtractor 已实现，需重新 extract，不能仅重编译旧图。TemplateDefinition 的 operation_region 与 workspace_region 现共用 execution_region/complete_region，allocator 只按自身 worker 窗口拥有 CPU 工作，不把外层 attention kernel 纳入。生产端调用共同 region_ports 检查跨界服务、保留工作端点、外部依赖及设备等待，并要求唯一 worker 资源的入口/出口与 allocator 标记一致。640个既有使用点通过，证据为 s4-workspace-growth/allocator_ports_verification.json；原切分回归见 allocator_region_verification.json。该结果资格化来源区域接口，不证明目标端口替换或分支已执行。替换应继续复用已有子图区域、外部端口和资源依赖表达，不新增第二套执行器。需求、目标分支、容量效果和同步范围须绑定同一目标操作，状态不按 block 重置。ExecutionGraph.schedule 每次追加后重算整个图；历史容量效果不能随重算重复提交，控制器也不保存另一份容量。 当前 WorkspaceState 在执行层独占容量，使用生产端导出的分配padding/alignment，按变换后的节点/边进行拓扑排序，并要求同池相邻使用点之间存在真实依赖路径。SimulationRuntime 在整批副本上推进，图装配成功后提交；不是逐节点重新调度时提交。来源和目标分支不同已进入共同子图替换规划：候选须匹配资源、分配几何、操作及目标需求，allocator 和语义区域共用端口/依赖装配。当前只定位保留的来源区域，插入子图内的再次替换未资格化。grow原先的region_wait_predecessor_not_exposed已由目标动作内提交查询关闭，现有开发序列完成两次grow插入及后续复用，证据 target_wait_binding_verification.json；这不关闭完整替换支持域。此前 state_verification.json 记录首次Decode的分支差异；现有 replacement_wait_frontiers.json 说明 donor grow 等待同rank两条业务流，必须恢复目标当前动作内的提交前沿，不能只导入前批工作、复制donor前驱或删除同步。

目标动作内部等待的下一步依据已由第69次原图核对：同一个kernel同时有关联控制线程和实际提交worker的NativeFlow，后续kernel的控制线程关联可早于allocator，但实际提交晚于它，不能以任意CPU关联判定已提交。沿allocator所在worker的提交边，用依赖可达性选择入口前的提交，再按每条设备资源的依赖关系裁剪完成前沿，两个grow均精确恢复原生等待的两条边；核对见 `s4-fixed-warmup/submission_frontier_verification.json`。该算法不使用观测时间选择前沿，现已进入TemplateInstance::bind_inserted_waits：读取保留和插入节点的实际拓扑及提交边，按同步所在worker和入口前可达关系选择提交，再裁剪完成前沿。TemplateAssembly合并当前动作前沿及既有跨动作/跨批前沿；无法证明提交在同步之前或之后时返回local_wait_submission_order_not_qualified，无法匹配该worker时返回local_wait_submission_worker_not_qualified。拓扑邻接由TemplateTopology::successors共同提供，容量顺序与等待绑定不各自维护一份图。当前只验证同worker提交的原生路径，未知线程或未覆盖活动仍保留缺口，不从两个来源结果推导任意配置资格。

设备等待由 SourceStreamWait/TemplateStreamWait 区分 stream/device：stream 要求原生流身份，device 不伪造单流身份，两者均须绑定有效资源域。设备同步等待同设备、入口前已提交的各流前沿；未知工作和重叠提交不能被默认为已覆盖，也不能将等待收费为 CPU 服务。观测活动与未归属业务必须依证据区分，不能只根据没有 owner 删除节点。

**后台事件清理仍待资格化。** 第73次独立观测中，669个未归属destroy_event队列任务均通过async_task_queue flow连接到唯一的后台enqueue，且全部处于同一提交worker的已归属任务之间。通过同线程完整API序列、包含关系及进程内event handle生命周期，其中411个销毁可追溯到本窗口的模型event record，258个没有窗口内record，不能推定其来自预热或给它们补造owner。证据为s4-independent-structure/native_background_event_audit.json。

当前torch_npu源码中，NPUEvent析构通过LaunchLazyDestroyEventTask向提交队列投递销毁；ProcessGroupHCCL的Watchdog查询完成事件并移除已完成Work，也会触发相关对象生命周期。源码与后台线程观察相符，但现有记录不足以逐次证明query对应的event及watchdog唤醒条件。TemplateAssembly的CPU线程投影只保留被建模端点的顺序，不会恢复被投影掉的清理服务；因此已有计算/端口检查不证明这些后台工作已被模拟。

运行时归一化现通过同rank/thread的Runtime@EventDestroy唯一完整包含关系绑定rtEventDestroy，保留原执行scope，不继承event创建者scope或SDK残留task ID。窗口外、时钟不满足包含或多候选均保持source_interval为空。主base的2618条销毁中2360条绑定、1300条有执行归属，剩余258条在profiler首条销毁之前；另一个base的1040条中1027条满足包含、519条有执行归属，13条时钟不满足条件仍不绑定。窗口内外事实不能简单按完整API序号配对。现有base仍没有完整入口event状态，且记录了尚无窗口内destroy的event record；这不等于这些设备event未完成，也不等于其后台清理可以忽略。

Ascend原生观察器现记录rtEventQueryStatus的event handle、原生API起止、返回码和event_status；成功时保留SDK枚举整数，失败时状态为null，不增加查询、等待或设备提交。CPU范围关联复用同线程同API的完整序列；具体watchdog来源仍需事实，不依线程名猜测。第74次单event SDK观察证据在s4-event-query/verification.json：三次状态均为1，和ACL调用结果一致；新建但未record的event也返回完成，因此状态值不能证明producer存在或替代record/destroy生命周期。pending状态、真实后台轮询及窗口入口快照尚未验证，历史无query记录不能解释为没有轮询。

入口观察现单独保留预热前后的event生命周期历史，并由runtime EventHistory重建入口已record未destroy集合；源时间只判断入口边界，不作为目标时延。第75次SDK观察确认已record并保留的A出现在入口集合，已destroy的B及从未record的C不出现，业务journal不混入窗口前记录。跨入口及重叠调用保持歧义；本次尚非真实serving入口交接，也未证明watchdog队列为空。

**沿用既有modeled边界。** [模板外CPU顺序投影](05_controller_semantics.md#模板外-cpu-观测之间的顺序投影)已明确模板外服务不计入目标成本。后台清理占用同一worker本身不构成必须实现完整watchdog的要求；此前“遗漏服务需要后台目标展开”的表述过宽，应区分已声明排除的服务和仍须保留的跨边界因果。新增event事实用于核对生命周期身份及外部依赖，不能把主线扩张成完整运行时仿真，也不能将模板外耗时拟合进节点。现有投影只覆盖已证明的同线程顺序；若存在跨线程/设备/状态端口的新因果，仍须共同表达或明确具体缺口。未归属数量不作为阻塞S3/S4的门槛，完整HTTP/后台成本继续不授资格。第73次输入已用于事实归一化修正，是开发证据；历史计算顺序结果保留，修改后的完整实现另行独立验收。


现有正式 grow 来源见 `data/redesign/s4-workspace-growth/serving_growth_verification.json`：640个使用点、两 rank 各一次 grow、1280个 CPU 切口无跨界；需求和观察容量保留为证据，不写成目标常量。其两条 device wait 曾因 profiler 开关任务没有 CANN 提交而清空 lanes（device_wait_audit.json）。提取器现仅按明确的 PROFILING_ENABLE/DISABLE task type 将观测控制排除在目标同步域外，原始节点仍保留并记录 scope 原因；正式重提取恢复各 rank 两条业务流，节点、边和其他等待不变，证据为同目录 device_wait_reextract_verification.json。此修复不替代目标容量与分支装配资格。开发来源不能兼作固定规则的独立验收。后续先关闭同步域、初始化资格和状态/分支装配，再进行独立目标结构验收；不按长度或人数重复采集。

### CPU 服务的内层语义归属

控制线程上的 CPU 服务使用原生 wrapper 的显式调用身份、父子关系及同线程 marker 进入/返回边界划分。AscendSourceExtractor 先恢复各线程的调用栈变化，将变化点与原有提交 flow 切点合并；扣除 host wait 后，按这些边界分段，记录最内层 scope_call。owner_call 仍是唯一 serving 所有者，source_interval 与 control_part 保留原来源；父区域只引用子区域工作，不另建父级服务段。调用标记与父栈不一致时拒绝提取，不以附近时间匹配或最近调用兜底。

该调用栈规则仅覆盖存在同步 wrapper 标记的线程。异步工作线程另用自身 CPU 观测中已有的显式 scope 身份：同一服务段的全部覆盖观测一致时保留该身份，含未知身份或不同身份时不推断内层归属；不将主线程 scope 按时间传播给工作线程。设备归属继续依赖原有明确身份和 flow；本修改不引入新的 CPU/NPU 时间修正，也不把 profiler 包络当作测得服务成本。

现有资产的 3,282 个内层区域标记通过嵌套核对。重提取后 CPU 服务段由 211,962 增至 218,526，其中 40,932 段具有内层 scope，覆盖全部 3,282 个区域；首层 QKV 的双 token 分支有 13 个直属 CPU 服务段，单 token 分支有 9 个。按线程、serving owner、control_part 核对，服务区间并集与累计原始时长不变；全部非 CPU 服务 span、kernel、等待、非 interval_extent 依赖、边界和 lane 均不变。证据在 data/redesign/s3-cpu-region-scopes/cpu_scope_audit.json，未新增采集。

额外切分来自已有语义边界，不意味着新增真实工作或模板族。给定耗时的验证输入若按每节点赋值，必须明确采用新分段后的节点集合；不能要求其总时序自然保持不变，更不能把这种差异当作性能收益。S5 仍须按真实成本单元独立测量并消除探针开销。区域内层归属也不等于已经知道每个 CPU 片段对应哪个可删除的设备操作；共享数据端口与分支替换仍需继续完成。

### 工作线程服务沿用已有观测身份

异步提交线程的 CPU 观测已经通过原有显式关联保留 scope_call。extract_worker_cpu 在同一服务段内统计全部覆盖观测的 scope，包括未知值；只有身份一致时传给服务节点。serving owner 的冲突处理、等待扣除和原有分段保持不变。该方式复用观测身份，不根据主线程时间包络传播调用栈，也不根据单个最近的 API 选择所有者。

现有 177,198 个 dispatch 服务段均取得一致身份，其中 176,058 个属于内层区域，其余属于 serving 层。重提取仅修改这些服务 span 的 scope_call，节点数量、原始时长、全部边界/依赖、kernel、等待和外层 serving 模板不变；公开执行图逐字节不变，kernel 布局、共享位置依赖、时序与 gaps 保持。证据在 data/redesign/s3-worker-region-scopes，未采集新数据。

首层 QKV 的 CPU 视图现在同时包含控制线程和提交线程：单 token 区域为 39 段，双 token 的两轮分别为 71 与 61 段。这个差异只说明源服务分段不同，不能直接解释成新语义分支、不同 CPU 成本或缓存行为，更不能逐分段计数增加模板族。跨分支替换前需按实际提交操作和端口关系归并工作；区域拥有这些服务并不等于已经知道哪几段可以随 Slice 一并删除。

### 完整操作边界与有限替换端口

操作级替换不能删除同时包含操作内外工作的 CPU 服务段。read_flows 保留 CPU 调用进入/返回切点；read_intervals 进一步按同线程、同原生语义区域中已有 ATen 观测的嵌套关系，加入最外层操作的进入/返回切点。嵌套 helper 不分别创建新规则；非嵌套重叠在读取边界报错。NativeFlow 仍由提交起点连接设备，不将调用返回变成设备屏障。

CANN 调用只是后端入口，不能自动当作完整语义操作。已观测的八次位置连续化均为 aten::contiguous → clone → copy_ → aclnnInplaceCopy；CANN 窗口之外仍有分配及收尾服务。单 token 的对应 QKV 区域没有这组三个 ATen 操作，因此只删除 CANN 窗口会保留不应存在的工作。这里沿用现有 CPU 观测和嵌套关系，不新增 intrusive wrapper 或逐 helper 的语义规则。

主线程调用与设备工作通过 async_npu 关联；工作线程 launch 通过 HostToDevice 关联，enqueue/dequeue 使用 async_task_queue 身份。同步调用的包含关系还须核对线程、scope、操作名和唯一性；不按最近时间或设备发生顺序配对异步工作。完整集合包含 CPU 操作窗口里的观测边界，以及全部成员都在集合中的任务完成汇合点，不能只拿服务 span 的起止点算切口。

现有八个完整集合的替换端口约束如下。此表定义待实现 mutation 的行为，不表示已经完成跨形状展开。

| 关系 | 删除连续化操作时的处理 |
| --- | --- |
| 主线程顺序 | 保留操作外的 CPU 服务及其端点，在同线程的操作进入/返回之间连接逻辑顺序；不新增零耗时服务节点 |
| 工作线程顺序 | 独立处理 dequeue 的前后顺序，不能和主线程端口交叉连接 |
| 设备 stream 顺序 | 按同一操作的任务前驱、后继重接；连续删除两个操作时一并解析到保留端点，不引用已删除的完成点 |
| 操作内部提交 flow | 随被删除工作移除，不能转换为主线程到后续无关设备工作的屏障 |
| forward 完成汇合 | 删除该工作对汇合的贡献，保留其他真实前驱；不把 CPU 返回补成新的设备完成前驱 |
| 共享 sin/cos 消费 | T>1 → T=1 时转接到 Tile 的视图生产端，并保持各层消费者；反向变换须有合格的物化模板，不能伪造工作或时延 |
| 外部服务的 interval_extent | 保留外部工作本身及其端点，不因为端点和被删除操作共享就删除外部服务 |

不能把所有输入和所有输出作笛卡尔积重连。额外同步、通信或未分类出口必须先明确语义；这些已观察集合的有限规则不自动推广到其他后端或任意操作。

当前 2,650 个线程/语义区域组合中的 16,428 个 ATen 根操作嵌套核对通过。通用外层切点使源 CPU 服务从 253,144 段变为 285,964 段；按线程、owner、scope、control_part、来源 interval 核对，覆盖并集和原始累计时长不变。非 CPU 工作、kernel、等待、所有边界和非 interval_extent 依赖不变。八次完整连续化操作均无跨边界服务；各有两条 CPU 顺序通路、一条 stream 通路及对外层完成汇合的贡献，见 data/redesign/s3-operation-ports/full_operation_audit.json。服务分段数不是模板族数量或成本模型类别，给定每节点耗时的时序变化也不是性能收益。

操作关联现已进入正式产物：AscendSourceExtractor 从 async_npu 取得同 owner/scope 的 CPU 调用与设备工作，在同线程/区域内定位完整 ATen 根操作；没有外层 ATen 时保留原生调用窗口。async_task_queue 的唯一 enqueue/dequeue 身份提供工作线程窗口，同步包含关系只用于确认 enqueue 的唯一宿主。SourceGraph.operations 保存来源 span，TemplateDefinition 将 CPU 窗口映射到自身局部边界，公开诊断 operation_groups 同时保留设备来源身份。该字段是观测关联，不代表操作已具备完整可替换边界；通信观测、设备标记和等待也可能是关联对象，不能一律当作可删除服务。

TemplateDefinition.operation_region 现按需解析操作视图，返回局部节点、边界、跨窗口服务、源内部切边、模板外连接及共享位置数据依赖。CPU 窗口仅在自身线程按观测位置切分，设备成员使用已记录身份；任务完成点只有全部贡献在内且无模板外输入时才纳入，forward 完成保持外部消费。视图不新增或删除执行工作，annotation 的包络不能代替服务所有权。区域合同另列 retained_dependencies：区域外节点的 interval_extent 即使接触或共享局部边界，也不属于可替换的内部/入/出依赖集合。操作删除复用该所有权分类；后续移植必须保留这些依赖及对应外部工作，不能按端点集合一并删除。

位置连续化的公开 replacement_region 诊断已与此前独立集合核对：八个操作的 CPU 服务、边界和全部源切边一致，另显式保留 512 个共享位置消费者。两个操作的工作线程出口位于 source_connections，经过未归属原生边界，由 cpu_thread_projection 绑定到实际目标后继；因此不能只遍历内部 dependencies 实现替换。原子 mutation 必须处理该绑定后的出口，并保留外部服务的共享端点。证据在 data/redesign/s3-operation-regions/operation_region_audit.json；目标物理图、既有诊断与条件化执行结果不变，未新增采集。视图提取完成不等于替换资格已通过；实例联合修改进展见下文与控制语义文档。

核心 plan_position_materialization_removal 已把上述连续化移除规则表达为完整方案：删除的局部节点/依赖、各 CPU 窗口与 stream 的独立顺序连接，以及共享位置依赖应改用的 Tile 节点。该方案仅针对已核对的 decode PositionSlice 在目标单 token 下省去物化的分支，不包含其他目标形状的支持判断。它不修改定义或执行图；调用方须在整批变换资格满足后一次应用，不能将方案存在直接当作可预测。

方案保留所有边界身份和外部服务自己的 interval_extent，故相邻连续化操作可以通过原有 stream 切边连接，工作线程投影出口也保持原绑定。内部提交和被删设备对 forward 完成的贡献移除；CPU 和 stream 分别连接，不新增跨通路屏障。包含额外工作、serving 端口、未知切边或不唯一 stream 切口时返回规则缺口。现有八个操作均生成方案：首轮每个删除 32 个节点/65 条依赖，后续每个删除 26 个节点/51 条依赖，各有两条 CPU 和一条 stream 顺序连接，64 个位置消费者指向同区域 Tile；数量差异使用同一规则。公开诊断位于 replacement_region.single_token_removal，证据在 data/redesign/s3-operation-removal/removal_plan_audit.json。公开执行尚未应用移除方案，物理图、已有诊断和条件结果保持不变。

实例拓扑现已实现上述方案的联合应用：TemplateTopology 先合并各操作删除集合和生产端映射，在副本上修改并检查数据端点，再一次提交。ExecutionGraph 和服务完成前沿读取实例拓扑，不再直接以源定义成员作为可执行集合。四个 decode 模板的诊断副本各移除两个操作：首轮每模板删除 64 节点/130 依赖，后续每模板删除 52 节点/102 依赖，各新增六条 CPU/stream 连接，64 个唯一消费者改读 Tile；包括同一 work 投影连接的局部图无环。正式身份展开的完整物理图和结果不变，证据在 data/redesign/s3-instance-topology。诊断副本没有修改 kernel shape 或模拟目标单 token 请求，不能用它证明整批跨形状执行成立。

操作记录不再以 async_npu 存在为前提：所有已有且有 serving/scope 身份的 ATen 根窗口先形成 SourceOperation，再附加明确的设备与队列关联。因此只有 CPU 观测的分支也可进入相同模板读取路径。device_spans 为空只表示没有已关联的设备工作，不能自动证明异步观测完整，更不能解释为 CPU 零成本。现有资产新增 11,970 条此类操作记录，原 16,632 条关联完全不变，源物理图及 serving 模板不变；调用记录数量不等于模板族数量。

SourceOperation 现保留 CPU profiler 原有的 input_shapes/input_types（Input Dims/Input type），TemplateOperation 同步读取；缺失为 null，原始标量列表槽不解释为张量或目标输出 shape。现有 28,602 个操作中 16,428 个有两项记录，另外 12,174 个保持缺失。字段逐项与 source facts 原始 arguments 一致，其他操作元数据、物理图和 serving 模板不变。旧派生源图按现行合同重提取，不增加旧字段回退。

logits CPU view 的来源诊断现由 logits_view_service 按已有操作绑定资格化：源请求数为一、没有关联设备或工作线程、没有跨窗口服务和模板外连接；完整窗口内只能有一个覆盖进入/返回的 CPU 服务，其他节点只能是注解。外部切口仅接受完整窗口两端的线程顺序和相邻服务区间关系，并要求线程入口、出口均存在。方法返回原始服务节点身份，不产生零耗时 view，也不使用观测墙钟作为目标成本。

十个 logits 根窗口的独立边界核对表明，六个物化分支各有一个设备操作及工作线程，四个单请求分支各有一个完整 CPU 服务；全部无跨窗口服务。四个 view 的服务身份已通过公开 cpu_view_service 交接，供后续子图替换引用。其内部注解点只有在没有外部依赖时才可收拢为完整服务窗口，不能把这一简化推广到含等待或异步提交的操作。证据在 data/redesign/s3-logits-fragment-boundaries/fragment_boundary_audit.json 和 view_service_audit.json；正式目标图与已有结果不变；这些来源事实不授予跨人数整批执行资格。

TemplateTopology.apply_operations将工作删除、依赖重接和子图插入作为一次提交，ExecutionGraph和TemplateAssembly消费实际插入节点，保留标准端口及模板外绑定。历史logits诊断证据见data/redesign/s3-logits-view-replacement/replacement_audit.json；该预览及专属实现已删除，不是正式跨人数支持。

### 索引写入的标量广播与完整操作区域

当前 16 个 aten::index_put_ 操作组中，6 个含 BroadcastTo+IndexPutV2，10 个只有 IndexPutV2。根操作输入记录显示前者的值输入是标量，后者为 [2]；物理 BroadcastTo 的 [1] 输入描述不能替代调用入口的逻辑 rank。

原采集容器的 PyTorch 为 2.10.0+cpu，提交 449b1768410104d3ed79d3bcfe4ba1d65c7f22c0。其 TensorIndexing.h 与[同提交 Python 索引赋值入口](https://github.com/pytorch/pytorch/blob/449b1768410104d3ed79d3bcfe4ba1d65c7f22c0/torch/csrc/autograd/python_variable_indexing.cpp#L584)说明：赋值前去掉值张量开头的单位维，故 [1] 变为标量，而 [B]（B>1）保留向量。原容器 ops_nn 包为 9.0.0；[官方同版本 AicoreCompute](https://gitcode.com/cann/ops-nn/blob/0413e303e6cd1986b592c3e039f2579ac0f10b62/index/index_put_v2/op_api/aclnn_index_put_impl.cpp)根据索引结果形状判断值的 rank/维度是否需要广播。这解释当前六个组的来源，不把 B=1 写成适用于任意索引操作的选择规则。直接调用 index_put_ 与 Python 索引赋值入口也不能混为一谈。

公开 operation_groups 现对索引写入复用 operation_region，交接整个 CPU/worker 窗口、显式设备成员、完成点和入出边。16 个区域均无 crossing_nodes；原始物理图及其余执行结果保持不变。证据在 data/redesign/s3-indexed-write-branch/broadcast_basis.json 与 region_audit.json，同目录保留版本信息及源码。未新增采集，也未改变未映射 kernel 的 52 个计数。

标量广播参数现由 SGLang RequestPoolLayout 解释：建库时按同一 aten::index_put_ 的 device_spans 绑定 BroadcastTo 与写入节点，核对原生根参数的标量 rank、dtype，以及广播输入/输出的完整布局。配对写入仍须通过既有 relay/请求表规则；不按相邻时间配对，也不把任何 [1] tensor 都视为标量。目标仍为单请求时广播工作保持不变；目标请求数变化返回 indexed_write_branch_replacement_required，不能通过扩大 BroadcastTo shape 保留错误分支。节点参数由生产端预先编译，核心交接原始描述及目标参数；框架名称和 rank 解释留在 adapters/frameworks/sglang。

六个已有节点均通过同输入验证，未映射从 52 降至 46；物理图及其他结果不变。证据为 data/redesign/s4-indexed-broadcast-parameters/source_audit.json。该结果只覆盖已观测标量分支，不证明跨请求或跨配置执行结构完整。

索引写入变换沿用公共操作替换接口，将源/目标分支中的 CPU 服务、广播、写入和完成端口一并映射。TemplateOperationRewrite 已增加 insert_subgraph：显式提供 donor 节点、依赖及 donor→recipient 边界映射，保留来源身份并重绑定局部端点；CPU view 分支也复用这一入口。新增依赖统一称为 added_dependencies，可表达旁路和被移植的内部边。外部端口须显式映射到 recipient；内部未映射点按 donor 证据及显式 lane 映射在实例内新建，联合应用多个计划时重定位各自局部编号。新增点的来源仅用于追溯，执行图和资源前沿以目标实例身份区分；原有共享源点仍保持共享。历史logits view局部预览曾迁移限定视图及内部注解，未通过正式批人数资格；该预览实现已删除，当前分支通过统一操作规则规划，仍须通过完整批次资格。索引写入的向量到标量替换已在局部预览中连接目标参数与依赖，完整目标执行及反向分支仍待验收，不能据此宣称支持任意多节点分支移植。区域无跨界服务只是可切分证据，不等于目标拓扑已合格，不能仅凭切口授予目标变换资格。

公共 plan_operation_replacement 负责结构移植，语义分支资格由调用规则提供。限定设备路径保留 recipient 的 CPU host/worker、stream 入口/汇合出口和最终完成端口，仅移除被替换区域的节点及内部依赖；区域外工作关系保持不动。旧中间完成点仍有外部引用时，只有通过“消费者已等待最终工作”的资格检查，才由最终完成点承接其身份。内部点按显式 lane 映射新建；未知额外资源、多个无明确配对依据的 worker、跨区域旋转位置依赖等保持拒绝。来源操作仍须具有已资格化的设备端口；目标允许已资格化的纯CPU view。该分支移植真实CPU区域，以零工作依赖旁路被移除的worker和设备区间，保留外部完成身份。CPU来源转设备目标仍未支持；分支代码不授予整批跨人数资格。

插入工作由 TemplateInsertedNode 持有 donor 定义和自己的目标 kernel 描述，不能复用 recipient 按 source span 索引的覆盖表。保留节点与插入节点共用参数变换入口，组件语义（包括 KV 分配/释放）先于通用请求表匹配；插入节点按 donor 的请求、区域和布局解释，并使用当前目标动作求值。设备节点缺少观测描述时报告 inserted_kernel_observation_missing，找不到已解释关系时报告 inserted_kernel_parameter_relation_missing。正式实例的插入节点经私有参数入口处理，公开节点记录携带 source_call 和 target_kernel。

SGLangTemplateBindings 在建库时从原生模型区域和输入记录识别 logits reshape，写入模板的操作索引；核心消费者据此定位区域，不再动态解析 aten::reshape 名称及 BF16 原生类型。模型/attention 参数绑定和 RoPE 数据依赖同样由该生产类解释。共同来源观测类型现位于contracts/source_observations，原生绑定解析位于production；区域元数据仍作为来源值保留，完整模板语义资格继续按实施计划验收。

当前正式路径已接入索引写入的设备分支选择。跨批人数的logits view替换曾只被局部预览调用，已随绕过人数资格的预览完整删除；来源reshape/view服务识别继续用于观测诊断。SGLang 生产端按根操作参数、dtype、目标存储布局和显式设备成员，将索引写入解释为 relay 向量或请求表写入，并记录标量/向量值分支；模板规划在同阶段、同 rank、同目标存储语义中选择已观测 donor，通过公共区域替换与参数化入口生成目标实例。不同目标存储语义不能互作 donor，未找到合格分支明确返回缺口。

历史局部预览证据保留于data/redesign/s4-indexed-variant-replacement/replacement_audit.json，不作为当前可执行能力。single_request_topology曾直接截断请求并调用底层规划，绕过instantiate_step的人数、状态与完整批次资格；该诊断输出与专属logits替换调用链现已删除。正式路径通过实例选择后才调用私有规划和插入参数方法。RoPE单操作切口诊断仍保留，明确只描述源区域，不代表跨人数目标可执行。

串行设备操作的公共端口解释使用实际依赖：stream 入口/出口同时从局部切边和跨模板连接取得；最终设备完成由 stream 汇合出口的贡献节点确定，区域内全部设备完成必须沿内部依赖先于它。外部 host wait 只能引用该完成端点；中间节点的 forward 完成边只有在相同消费者已等待最终节点时才属于同一完成语义。CPU 入口/出口仍使用原生 host/worker 窗口，区域外工作的 retained_dependencies 保持独立。缺少唯一端口、因果路径或出现其他外部依赖时返回具体缺口，不靠时间相邻或 kernel 序号判断。

该解释由既有单设备操作删除规划复用，并交接索引写入的 device_ports 诊断；它只证明当前源区域的端口结构可解释，不决定某个目标 shape 应选哪个原生实现，也不自动批准移植。

### 批人数变换的整体边界

当前正式实例化先限定来源与目标人数相同，再选择完整同批次、跨rank的阶段组合。TensorParameterLayout中的Requests轴和KvBatchWork可计算目标人数，但这些数值关系不证明请求循环、工作节点数和端口可重组。跨人数实现须按以下职责一起闭合，不能解除人数检查后逐案例补洞：

| 部分 | 已有基础 | 尚需成立的关系 |
| --- | --- | --- |
| 请求级工作 | publication成员区域、目标请求槽及KV身份 | 按目标人数重复/省略完整成员区域，保留CPU顺序、等待和外部引用；实际支持由完整批次资格决定 |
| 批次共享工作 | 准入工作参数、共享输出、聚合释放 | 循环外工作只保留一次，成员计数及前后状态随目标重算；不能复制整个来源批次 |
| 模型计算 | Requests/token轴、已有单/多请求连续性事实 | RoPE/logits等视图与物化分支的节点、消费者及完成端口共同选择；不能只改shape |
| 跨阶段连接 | 同源批次选择、rank cohort、统一执行端口 | 每个阶段取得同一目标成员语义，完整组合资格先于装配；局部替换成功不能代替完整执行 |

当前代码中的人数限制须按职责处理，不能统一删除：instantiate_step是整批入口；BatchParameterLayout只对未移植请求保留原成员身份；TransferParameterLayout按目标请求计算输入token总量和逐请求元素，固定单请求Cat与释放组仍保留来源人数/分支资格；BatchOperationVariant决定Cat、索引写入、logits等结构分支。Schedule的admission_cpu_request_count以及filter/merge调用序列还须与新批成员共同成立。共享PublicationParameterLayout已删除重复人数状态，但不替代这些条件。

已有两配置公开结果中的4份不同Decode Prepare来源（两rank、B=1/2）各有唯一aten::clone操作及单个D2D DMA，字节数分别为8/16，与一请求一个INT64元素一致；见s3-publication-target-members/decode_clone_observations.json。生产端现复用RequestElements表达该复制的元素数量，共同求值按目标成员数乘INT64字节数；singleton Cat和释放组继续保留原人数/分支限制。该清点不证明跨人数执行，参数关系接入后仍须通过完整批次资格；s3-target-transfer-extents/verification.json的两个配置同人数回归通过，完整图与此前一致，未验证跨人数分支。

先用已存在的1/2请求来源建立上述共同表达和有限规则，再验证未参与修改的请求组合；更大人数的支持须有结构关系依据。缺关系归Rule，不能按1到请求池容量枚举模板来代替变换。该工作仍未完成，不影响当前复用同人数模板执行准入2→1。

**请求循环的成员间工作。** 当前SGLangRequestPublication把with scope插入原生for循环体内，未包住for迭代推进。已有30份Publish实例（两rank；Prefill单/双请求8/2份，Decode单/双请求12/8份）中，全部10份双请求实例均在前一成员出口与后一成员入口之间保留恰好一个父Publish CPU服务：owner_call属于Publish，scope_call为空，与成员位于同一CPU lane。20个桥接端点的80条关联边已清点，没有跨模板引用。证据s3-publication-loop-boundary/source_inventory.json；使用明确归属和端点相等，不按时间接近推断，也未新增采集。该工作已被提取器保存，不能把它误判成需要新增wrapper才能获得的缺失观测。

准入工作使用逐候选的 `request_horizon_checks`，顺序为原生候选检查顺序；进入候选预算检查的次数 `request_checks` 由数组长度得到，整批 `horizon_checks` 由它求和，不维护第二份累计状态。`candidate_checks` 仍计入容量检查时止步、尚未进入预算函数的候选扫描，不能与数组长度混同。轻量控制器在进入候选预算检查时建立一个条目，早期拒绝可以为零；当前模板生产端只资格化全部被检查候选均接纳的路径，以原生 `accepted_requests` 对齐成员顺序，并拒绝重复或不连续位置。参数化资格要求每个接纳成员至少有一次检查，不能用整批总数掩盖某个成员的缺失。队列压缩和尾部统计仍属于一次共享工作。该合同提供成员变换所需的工作分配，尚不授予成员重写、拒绝路径或跨人数资格。

**准入循环的已知边界。** 短初始化来源的两个rank均有两个admission_budget成员，分别记录accepted_requests=0/1、candidate_accepted=true、native_result=CONTINUE及horizon_checks=1/2；每个成员为一个CPU注解和一个CPU服务，均已取得单控制线程端口。成员之间不是单一服务：同一父admission_prefix下存在三个连续CPU服务和一个内部注解，内部边界没有非ThreadOrder/IntervalExtent关系。证据为 `s3-source-composition/admission_sequence_contract.json`，基于已有原始图和正式预测端口，未新增采集。

plan_region_sequence现通过cpu_sequence_separator沿同CPU线程的ThreadOrder/IntervalExtent路径取得完整子图，保留全部父级CPU服务、内部注解、边界及依赖，再用现有complete_region/region_ports核对单一入口和出口。跨界服务、非父级服务、等待或额外资源不由该规则接纳；不按片段数加分支，不将多个服务合成固定耗时。插入成员间工作时复用整段子图及其来源，原先单CpuControl假设已删除。准入budget成员已进入生产端admission_regions绑定与正式序列规划：按原生accepted_requests排序，只绑定已接纳、CONTINUE且CPU边界合格的成员；编译时用同一plan_region_sequence资格化完整来源循环。目标人数变化时与Prepare共用成员列表装配，首成员保留初始化路径，新增成员复用已观测的后续插入路径；仅有首成员的来源不授予多人展开资格。循环外队列整理和统计保留在一次共享骨架中，拒绝候选不作为目标batch成员。来源循环资格及同人数回归本身不证明目标人数重写；正式选择器现允许满足成员边界和长度参数条件的跨人数候选，仍逐操作核对变换。horizon工作量属于成员参数，不按比较次数新增语义成员，也不在此实现成本拟合。上述关系闭合后，再与Prepare初始化、共享Cat、模型物化和Publish一起资格化人数变化；源序列合格不等于任意目标人数受支持。


跨人数的共同变换应把有序成员和相邻CPU服务作为一个原子区域计划：保留首入口、末出口、共享输出/聚合释放及外部资源端口；对N个目标成员组织N个成员区域和N−1个成员间服务，各实例绑定独立身份及目标槽位。首轮进入与末轮退出的父CPU工作仍归批次骨架，不能把中间服务复制成前后缀。给定耗时仍按实际服务节点计入；S5后续解释其工作量，不把成员间服务当零成本依赖。

共同循环由TemplateDefinition.plan_region_sequence实现，Prefill/Decode成员分支改写共用这一入口。region_ports资格化整个来源循环及donor成员，insert_subgraph分配独立身份和目标槽位；主CPU保留真实迭代服务，其他线程/设备流分别连接，各资源单独维护顺序出口与设备完成。移植等待在推进本成员资源前沿之前绑定目标前驱，防止自等待。循环首末外部端点、输出和聚合释放留在批次骨架；单成员序列直接复用plan_region_replacement的资源插入资格。多成员目前限定来源循环已暴露的资源及一个设备流，新资源和其他流组合仍须另行资格化。

成员列表按目标请求构造：原槽位存在且完成分支相同时保留，否则选择合格donor；成员数变化也触发整体序列装配，不能按来源列表长度分配目标槽。成员选择只检查同phase/rank/输出策略、完成分支及自身接口，不再先按原槽位构造随后丢弃的独立替换计划；前驱合法性由整个序列统一检查。完整计划经TemplateTopology原子装配；成员资格由生产端与编译资产交接。完整批次资格继续保留；PublicationParameterLayout仅计算非空目标成员的输出工作，不重复存储来源人数。目标成员增减仍须与模型分支、Prepare/Schedule及共享工作一起闭合。

正式证据s4-publication-resource-sequence/verification.json覆盖两个既有配置和双请求均在Prefill结束的开发输入：两个配置的指标、执行摘要和节点角色数量保持；两个rank的完成循环分别有唯一真实CPU桥接、worker成员间顺序，以及第二成员两处等待到第一成员目标设备完成的依赖。两个释放kernel分别使用目标槽及页身份，共享输出一次，全图时间依赖有序。主CPU迭代不强制等待worker，分别核对各线程顺序。此前纯CPU循环证据见s4-publication-sequence；这些是固定人数的正式图验证，不是跨人数或独立原生验收。

**Prefill循环的资源与等待。** 当前来源的10份Prefill Publish实例中，两份双请求实例（两rank）分别在相邻成员之间保留主CPU线程顺序、worker线程顺序及设备流顺序；主CPU中间服务见上述共同循环取证。第二成员还各有两条明确NativeHostWait输入，来自第一成员的设备工作结束。流顺序出口是另一个任务完成汇合点，与该设备结束通过CompletionJoin连接。证据s3-publication-resource-loop/source_inventory.json按原始边、资源域和端点核对，并保留native API引用；两份来源的成员完成状态为继续/完成，不把该等待数量推广到所有分支。

这组来源证据是上述共同多资源循环的机制依据，不把donor的原请求关系带入目标。首成员外部等待由资源入口及执行核的等待绑定处理，后续成员使用目标序列前驱；末成员导出的完成身份继续约束循环外消费者。

### NPU decode 的共享位置结果

Qwen3Attention.forward_prepare_npu 仅在首个本地层调用 get_cos_sin_with_position；get_rope 工厂按模型 RoPE 参数返回共享对象，各层的融合 QKV/RMSNorm/RoPE kernel 随后消费该对象的 position_sin/position_cos。当前固定 Qwen3 模型、NPU、非重叠 decode 路径将这对共享结果作为显式依赖组；prefill 使用 forward_prepare_native，不套用该关系。

SGLangTemplateBindings 在模板生产时从唯一已绑定的位置 Gather/Tile 区域识别数据生产者，核对融合消费者的原生区域类型、head_dim 和层序。T=1 时两项连续视图共同由 Tile 的结果提供；T>1 时要求同区域的两次 Slice 都完成。没有 tensor 身份证据时，不按 kernel 顺序给两个 Slice 分配 sin/cos 名称。报告中的 required_value=sin_cos_pair 和 consumer_input_slots=[1,2] 表示整个消费组，并非宣称每个 Slice 单独生成两项数据。

ExecutionGraph 用 model_data_dependency 显式连接生产节点结束与消费节点开始。该关系依据原生模型代码推导，区别于 source DAG 中的观测边；没有增加工作、服务时延或全局屏障。原始图保留不变。固定输入门槛继续保留；将来改变连续化分支时，需要同步改写生产节点及这组依赖，不能只替换 shape 或删除 Slice 后仍引用旧生产端。融合 kernel 的其他不完整 vendor 操作数记录也不会因此自动补齐。

现有资产中六组 decode 的 640 条依赖在原 source DAG 中均已有因果路径；公开 predict 新增同样的 640 条 model_data_dependency，移除这些显式边后整个目标图与上一轮相同，布局、区域、kernel 覆盖、执行摘要、metrics 和 gaps 保持不变。证据在 data/redesign/s3-rotary-dataflow，未补采；lower-admission 仍 unsupported。

### 已识别的连续性结构分支

Qwen3 NPU 路径使用 RotaryEmbedding.get_cos_sin_with_position：按 positions 取表，reshape/repeat 后 chunk 为 sin/cos，再调用 contiguous。当前原生实现中，chunk 视图的行跨度为 2D；T=1 可直接使用，T>1 则需要物化。本次资产中后者出现两个 Slice，不能只改变 T 轴而沿用全部节点。

现有 ModelKernelLayout 已表达 GatherV3、Tile 和 Slice 的行关系，保留缓存表与索引描述槽；Slice 诊断记录 T>1 条件。这不是已实现的节点增删规则。后续有限变换必须处理分支选择、节点与边、数据端口及工作归属，才能跨该条件复用；目前仍保留精确输入门槛。对应证据来自已有资产，无需为了重新发现该分支追加采集。

LogitsProcessor._gather_attn_tp_logits 的整除分支先建立 [TP,B,Vrank]，经 permute(1,0,2) 后 reshape 为 [B,V]。固定TP>1时，B=1可直接view，B>1需要物化；已有双请求来源包含clone/copy_、_unsafe_view及Transpose，单请求来源保留一个真实CPU view服务。TP和B必须分别解释，不能因为观测中两者都为2而一起变换。

生产端根据同一logits语义区域的projection、Cast和BF16原始输入[B,TP,Vrank]定位唯一reshape根窗口，将操作索引写入模板；不按“第几个reshape”、span或时间邻近选择。来源识别与CPU服务依据见s3-logits-operation-inputs/{input_contract_audit,binding_audit}.json和s3-logits-materialization/cpu_operation_audit.json。局部clone有跨界CPU服务，有限替换应处理完整reshape根窗口，不能直接删除CPU调用或将view当成零耗时操作。

**CPU view与设备数据就绪不是串行链。** 原生调用没有为了view读取设备内容。当前两配置使用的8份模型来源（Prefill/Decode、两rank、单/双请求）已按正式模型布局识别logits AllGather、reshape和Cast，并沿来源实例的明确内部依赖检查：全部AllGather物理任务完成均可达Cast开始，CPU reshape返回也可达Cast开始；4份单请求view存在真实CPU服务、没有物化设备节点，且AllGather任务完成不可达view返回。证据s3-logits-view-dataflow/source_dataflow.json。这里的不可达只描述观测图，原生控制逻辑提供不新增同步的依据，不能用缺边本身证明所有后端都不等待。

因此多请求物化→单请求view的后续变换必须保留两个条件：CPU view返回后的正常提交顺序，以及AllGather设备完成到Cast的数据/stream依赖；不得添加AllGather完成→CPU view的等待。用已资格化的CPU view区域替换物化的host/worker/device区域时，还要维护原CPU队列与设备流前沿的外部端点，不把移除的Transpose完成身份悬空。生产端现将该操作绑定到共同BatchOperationVariant：B=1选择已观测CPU view，B>1选择已识别LogitsTranspose物化。共同操作替换已允许CPU view donor；实际跨人数消费见下文“跨人数候选与完整结构资格”，此机制不授予反向分支或性能资格。重提取资产s3-logits-operation-binding的两个既有配置回归已完成，完整物理图与s4-publication-sequence逐字节相同；诊断使用的8份来源中，4份物化操作端口合格、4份单请求CPU服务身份保持。证据verification.json仅证明来源绑定和既有行为，该历史回归当时未覆盖CPU donor替换分支。

### 模型入口的位置参数

普通 prefill 的 ForwardBatch.compute_position_triton 消费每请求 prefix 长度 P_i 和新增长度 L_i，输出总长 T=sum(L_i) 的位置及 B 个起点。第 i 个请求的起点为 sum(L_j, j<i)，该段位置为 P_i 到 P_i+L_i-1。请求顺序由当前批次决定，不能复用源请求身份或源位置值。

当前已将实际 model_forward scope 中的 compute_position_kernel 绑定为 forward_positions：输入两槽按 B 变化，输出位置按 T、起点按 B 变化，核对原生 dtype 与全部槽形状。该关系复用已有轴机制。目标 kernel 的 position_workload 用每请求 prefix/input 长度、extend_start 和半开位置范围紧凑表达值语义；由目标 BatchInput 生成，不保存源位置张量或为每个 token 新增记录。它描述整数内容，不执行 NPU 张量计算，也不独立证明不同长度的 kernel launch 结构。无 prefix 输入变体、speculative 与多模态位置不能由此推定支持。长度门槛仍保留，位置值按上述目标状态公式生成；后续仍需核对其消费者与完整目标数据流。

普通 decode 的原生 _clamp_position_native 为 clamp(seq_lens-1, min=0)，主线每请求新增一个 token，因此有效请求的最终位置等于 context_tokens。当前入口绑定分别保留 Sub 与 ClipByValueV2，输入/输出请求轴均为 B，标量槽保持观测形式；只在最终 clamp 节点附加 PositionWorkload。该描述中的 extend_start 在 decode 表示紧凑位置序列的逻辑偏移，不代表额外产生一个原生起点 tensor。没有合并两个节点或把减一、clamp 的服务重复计费。

### 页分配的源校验与目标实例化

PagedKvLayout 对 alloc_extend_kernel 先用源调用的请求数和新增 token 总数核对四个输入槽及输出；再从目标 BatchInput 生成前三个 B 向量、第四个空闲页向量和 T 个输出索引的形状。源合同与目标参数必须分开，不能拿目标长度校验源记录，也不能只改空闲页长度而留下源 B/T。

前三个输入分别是 prefix 长度、完整序列长度和 last_loc，第四个是 free_pages；实际输出是物理 token 槽位。形状关系不提供 last_loc/free_pages 的具体身份。原生分配还包含旧半页、新整页、新半页以及需要时合并空闲页的分支；new_pages=sum(ceil((prefix+input)/S)-ceil(prefix/S))，S 为页大小。后续须根据目标分配状态解释这些值及分支，不能沿用源物理地址。当前仅修正布局实例化，完整跨请求门槛仍未解除，alloc_decode_kernel 仍明确不支持。

### 跨迭代 relay 的两个维度

请求池节点参数现在在建库时编译为 RequestPoolParameterLayout。生产端核对 relay、请求表写入/读取、全表 Slice 及标量广播的来源操作数，输出明确输入/输出轴；核心用 TensorParameterLayout 的 Requests、SequenceTokens、RequestPoolRows 关系求值。RequestPoolRows 仅在声明该轴时读取目标容量，缺少容量不能被零或源值代替。

通用请求池参数对象另外记录是否使用目标请求槽、是否仅适用单请求。只有索引访问携带 BatchInput 中的 pool_slot，全表视图不伪造成员；标量广播只保留既有单请求分支，不能靠改变 shape 代替选用其他操作变体。原生来源缺口在建库时保留，实例化返回对应原因。容量支持条件由生产端输出 RequestPoolCapacityDomain，保留最大容量及缩容缺口；执行时不再访问生产端对象。每个索引写入操作携带 IndexedWriteVariant，记录目的存储语义、来源请求数和目标请求数适用区间；核心据此选择同语义、同阶段、同 rank 的 donor，不自行把 B=1 当作所有框架的分支条件。当前 SGLang 生产规则声明标量区间 [1,1]、向量区间 [2,max]，这只约束操作变体，不保证区间内任意目标的完整结构合格。

RequestPoolLayout 只在建库期间持有原生写入指针，绑定结束后销毁；模板中的变体、容量域与节点参数均为独立值。建库装配和统一资产交接见[当前生产边界](development.md#c-目录与职责)，不能据此宣称全部核心已与框架无关。

FutureMap 在非 overlap 路径也通过请求池槽位传递输出 token 与序列长度。RequestPoolLayout 的一维 INT64 relay 以容量加 padding 槽为存储长度，以当前 B 为索引/值向量长度：Index 读取产生 B 个值，IndexPut 写入 B 个值但返回池大小的缓冲区。源 B 用于校验实际操作数，目标 B 用于实例化；二者与池容量分别处理。容量不变而 B 改变时也不能直接跳过 relay。

该关系只描述已观察 relay 操作数。具体请求到槽位的映射、跨批数据依赖、过滤/释放分支，以及二维 req_to_token 表的 B/context 变换仍需独立落实；二维表布局已区分容量、B 与最大序列长度，但具体值和完整拓扑尚未资格化。全图仍保持精确输入门槛，不以一维 relay 布局可变宣称整个 serving 已可泛化。

二维 req_to_token 表的当前布局规则以完整操作数匹配解释五类原生工作：prefill 写表、裁剪到最大序列长度、按请求取行、按请求/位置取末槽和 decode 写表。完整表的列容量保持观测值；视图宽度取 max(context_tokens+input_tokens)，索引/值向量与取行结果使用 B。固定长度的索引描述槽不因 B 恰好为 1/2 而变化。源请求用于验证，目标请求用于实例化，未知表操作保留缺口。

write_req_to_token_pool_triton 的原生签名包含 prefix 指针、长度和 out_cache_loc 等输入，当前 vendor 记录只有四个可见输入 tensor，属于部分操作数观测。布局变换仅处理这些可见槽，不据此确认全部输入与地址；具体 slice/index 内容及跨操作数据流仍需目标状态表达。当前正式输入回归不能替代变更 B/context 的端到端验证。 当前请求表规则即使源/目标维度相同也执行操作数匹配；匹配后仅保存有变化的描述，避免同参数路径绕过源合同。轻量控制器的空闲页数量仍只是容量状态；请求槽位现由 Progress 单独持有并通过 RequestSlice 传递，KV 页身份现由 Progress 的目标页列表维护，空闲数量来自同一队列。last_loc 等消费者仍需从该目标身份推导，不能填入源地址。

已匹配的 relay、请求取行、末槽读取及写表节点通过 request_pool_access.slots 携带目标请求预留槽位，顺序来自 BatchInput。整表 Slice 不按当前请求索引，不附加该字段。该描述从控制器状态生成，随实际 kernel override 传入执行图；即使形状不变也保留。它只表达请求池行身份，不补造物理页号、last_loc、token 值或 vendor 未报告的具体指针参数。

### Attention 的目标页地址

AttentionWorkload 现从每个目标 RequestSlice 取得请求槽位和 KV 页列表。prefix 的末槽为 page[(prefix-1)/S]*S+(prefix-1)%S，prefix=0 时为 -1；完整 live 页列表用于解释该请求的 block table。短请求在矩形 block table 中未使用的 padding 列不伪造为实测有效页。

CacheWrite 按目标 token 区间 [prefix,prefix+input) 逐页生成连续写入范围：batch_token_begin 指向本批展平 K/V 的对应行，slot_begin=page_id*S+页内偏移，token_count 不跨页。范围数随触及页数增长，不按 token 枚举。历史末槽、目标页与写入范围由同一状态派生，不保存源分配地址。

这些值随实际 attention/cache-write kernel 参数输出；描述值语义不代表已经执行原生 NPU 写入，也不证明整条分配器和请求表的数据依赖已资格化。当前常规输入只覆盖单页请求，跨页与释放复用仍需独立结构验证。

分配器与 attention/cache-write 现在共用 KvSequence 的地址推导，避免分别实现末槽和逐页范围。alloc_extend_kernel 的 kv_allocation 描述目标序列长度、既有页数、新增页数、页列表及写入范围，空闲数量为 Prepare 前快照；新增页是页列表中既有页数之后的部分。即使布局未变也保留这项语义输出。

这使得分配输出范围与 cache-write 的目标槽范围可以直接核对。值一致还需结合目标图中的分配完成到写入开始路径检查；它不自动证明源依赖、分配器内部所有分支或跨页拓扑正确。decode 的原生准备使用另一组节点，不能把这项 alloc_extend 关系套到未支持的 alloc_decode_kernel。

### 主线 NPU 分配器的实际分支

主线 model_runner 在非 SWA 的 NPU 分支选择 NPUPagedTokenToKVPoolAllocator，不使用通用 PagedTokenToKVPoolAllocator 的 decode kernel。NPU decode 先以 CPU seq_lens 判断新增页数，再计算设备 need_new_pages/cumsum；新增页为零时返回 last_loc+1，有新增页时另读取 free_pages 并组合旧页/新页位置。因此“没有 alloc_decode_kernel”是实现路径差异，不能当成采集遗漏或补一个同名节点。

早期资产两次 decode 的 prefix 为 [8,16] 和 [9]，页大小 128，新增页均为零；FloorMod、Equal、Cumsum 等观测与该路径一致。后续资产已取得 allocator 独立区域和跨页分支，证据索引见实施计划。父 Prepare 中的 Add/Sub 不因名称相同而成为分配输出。

在当前普通、非 overlap、分页 Decode 范围内，`alloc_for_decode` 的 `seq_lens-1`、`seq_lens+token_per_req`，以及 `prepare_for_decode` 的 `seq_lens`/`orig_seq_lens` 增量操作，都具有每请求一行的长度向量。生产端只对 Prepare 直属 scope 绑定此共同关系，排除内层 allocator；核对完整 `[B],scalar → [B]` 操作数及一致的 INT64/INT32 类型。核心复用 BatchParameterLayout 的 Requests 行轴，目标 B 来自 BatchInput，标量槽、dtype 和其他属性保持观测。

该关系只表达工作量，不按执行顺序为多个 Add 猜测具体张量身份或生成长度值，也不建立新的节点/依赖。形状变化的 launch 与拓扑资格仍单独判断，正式精确匹配门槛保留；不将这条行轴规则当作任意 Decode 配置的支持声明。

NPU prefill 在新增页数小于 200 时调用 sgl_kernel_npu 的 alloc_extend_kernel，否则走 naive 操作序列。当前固定容量只有 128 页，成功准入范围内不会进入后者；以后增加容量时必须重新判断覆盖。当前安装的 kernel 以 bs_upper、page_size、max_num_extend_tokens=next_power_of_2(T) 为编译参数，循环分块为 2048；不能把通用版本的动态循环实现或相同 kernel 名当成不同 T 的实现资格。地址公式成立与编译实例/拓扑/成本适用分别判断。

证据保存在 data/redesign/s3-npu-allocation-branches。当前安装源码用于确认实现机制，不独立证明历史捕获二进制完全相同。此项审阅不启动采集，不改变既有精确输入门槛或将跨页路径标记为已支持。

### Prefill 请求数变化的工作与语义切口

`mem_cache/common.py::alloc_for_extend` 在分页路径中按请求取 prefix 的末槽，空 prefix 则在设备上构造 `[-1]`，随后 `torch.cat(last_loc)`。已有两个 rank 的观测显示，B=2 产生 Concat kernel；B=1 的同名 CPU 操作关联 DMA，而非 CPU-only view。两条 DMA 都通过已有事实中的 native_api/hardware_intervals 关联到成功的 rtMemcpyAsync，拷贝量各为 8 字节。整数 transfer_kind 仍作为原生事实保留，公共方向含义须由生产端按真实 API 定义解释。

该审计推动了普通 DMA 的独立工作描述，当前 SourceTransfer 已交接成功且唯一关联的 API 字节数、方向和来源；公共合同见采集设计。rtMemcpyKind_t 的普通 0–3 方向依据本机 CANN runtime/mem.h，扩展/自动寻址模式不套用。有限插入仍缺目标参数关系时明确拒绝，不能靠补 kernel 或追加采集解决，也不把 API 总量重复分给多个硬件任务。Concat 替换必须同时处理关联 CPU/worker/DMA 工作、依赖与端口，且 per-request 输入构造的次数变化不能仅靠修改设备 shape 表达。

操作切口核对进一步确认：B=2 的 Concat 是所在观测 stream 的首个设备任务，入口只有 native_flow，没有前驱 stream_order；B=1 DMA 则有跨模板 stream_order 入边。公共替换器原先要求恰有一条跨切口 stream 入口，错误拒绝前者。现允许从区域内设备任务中排除已有内部 stream 前驱，识别唯一初始入口；仍要求唯一出口、设备工作到最终完成的因果路径及 CPU/外部依赖资格，不按时间挑首节点。两 rank、两种操作共四个切口均通过；这只证明记录中的操作接口可识别，不证明来源完整性、Concat/DMA 语义替换或整个 Prepare 可变换。证据：data/redesign/s3-prepare-operation-cuts/concat_cuts.json。

生产端现在把该 Cat 和请求池索引写入编译为共同 BatchOperationVariant，由同一候选选择和整组替换实现消费，不新增专用替换器。Cat 必须是冷 Prefill、分页路径中唯一直属且设备成员明确的操作。多请求分支核对每个输入都是一个 INT64、输出为 B 个 INT64；其单设备任务域为2≤B≤32，依据后端分组机制与边界观测；不能仅改变输出 shape 推广到任意 B。单请求分支核对实际 D2D 八字节拷贝，并声明其在一个请求下与 prompt 长度、页数及末槽数值无关。该不变量允许复用原传输描述，未增加成本常数或拟合。

历史局部预览（s3-prefill-concat-replacement，后被完整跨人数执行取代）调用同一规划器，对两 rank 的 B=2→1 Cat 完成整组替换：每组移除 30 个节点/58 条内部边，插入 donor 的 24 个节点/48 条边。逐项核对 donor 节点、边和端口映射，替换后的局部依赖无环；CPU/worker 和 DMA 一起迁移。该历史预览不覆盖 Cat 外的逐请求 CPU 构造、完整目标连接或发布循环，当时完整目标由入口拒绝；当前完整收缩证据见下文“跨人数候选与完整结构资格”。反向插入和未观测操作数数量未验收，不因本规则自动补采。证据：data/redesign/s3-prefill-concat-replacement/verification.json。

原生 alloc_for_extend 的 last_loc 遍历 prefix_tensors，冷分支构造 torch.tensor([-1])，Cat 在列表构造之外。SGLangLastLocations 标记聚合表达式；SGLangLastLocation 复用相同 AST 定位与 BlockWrapper 生命周期，通过局部 helper 标记每个原表达式。enumerate 对应 batch.reqs，记录 request_id/index 和 cold_prefix；不读取设备值。列表插入及迭代推进仍归父区域，不能按算子名字或出现顺序推断成员。安装版 AST 去除插桩后与原表达式相同。

生产端验证父区域和请求身份，将同父、冷 prefix、单 CPU 资源端口的成员按槽位保存为 request_initialization_regions。单 CPU 端口不等于只有 CPU 服务：成员内的 HostWait 必须保留，由共同等待装配处理。plan_region_sequence 接收显式源成员列表，Prepare 与 Publish 复用成员、迭代服务及端口装配；插入节点共享目标 request_slot，只有 Publish 附加发布工作参数。codec 保存成员索引，来源报告输出 source_call。目标重复/省略须与Cat/DMA、模型及共享工作联合资格化；成员初始化切口缺失时，不允许仅改变kernel shape后保留原CPU循环。

事实与验证分开记录：完整多批次资产已有10份聚合区域但无逐成员身份；s3-last-location-members 的一次有界采集取得两rank共4个成员，每个25个span，父前置/迭代/收尾服务边界明确。其 live_verification.json 保存原生证据；s3-request-sequence-contract/initialization/prediction_verification.json 验证当前资产读回、请求槽位、CPU端口、无重复执行及图时序。该机制采集约165.4秒，采集产物约160.7 MB、编译资产约11.7 MB，不代表完整建库已轻量化。历史定位、失败恢复及各轮结果保留于执行日志，不作为当前跨人数验收。

Prefill 发布中，完成请求走 release_kv_cache，未完成请求走 maybe_cache_unfinished_req；ChunkCache 将请求表的 [:req.fill_len] 从 INT32 复制为 INT64 prefix_indices。组件绑定从 req.rid、req.fill_len 和 req.req_pool_idx 记录身份、长度和槽位，复用 NativeRegionWrapper，安装对象为 batch.tree_cache。

SGLangTemplateBindings::bind_regions 统一解析显式 request_id 并将来源槽位交给 TemplateRegion；请求循环的 request_index 必须与身份匹配，嵌套区域继承最近的请求归属并拒绝冲突。bind_prefix_publication 复用该槽位并核对长度（固定资产保持证据：data/redesign/s3-region-request-slots/verification.json；当时新增区域继承未原生验收）；缺区域时，只有单请求且唯一直属 Cast 才能由整个 Publish 确定身份。核心 BatchRequestBinding 只保存请求数量和槽位，RequestSequenceTokens 使用该目标槽位的 context+input；未移植节点的请求数量变化返回 request_sequence_membership_rewrite_not_implemented；显式成员移植使用目标槽位，完整批次重复/省略仍待资格化，不继续使用旧的 SingleSequenceTokens 类型或单请求专用绑定方法。显式 scope 存在时不再匹配外围 Cast，避免嵌套插桩后错误绑定其他工作。

该历史资产中的六个 Cast 均只有外层 Publish scope，因此新区域消费路径还未真实验证；两个单请求 Cast 保持已建立的唯一归属，多请求四个不能按长度或出现顺序绑定。公开 wrapper 合同及节点参数化不等于真实区域、CPU 路径、kernel 分支或完整目标资格。旧资产证据见 data/redesign/s3-single-request-prefix/verification.json，最新固定资产保持检查见执行记录；请求循环边界和结构复用另见下文 Decode 分支。

证据：data/redesign/s3-prefill-structure-mechanisms/mechanism_audit.json。该审计本身未新增采集或改代码；后续 DMA 交接证据单独列于实施计划，形状门槛保留。它确认 kernel 参数覆盖率不能替代 CPU、DMA、控制分支和完整子图变换资格。

### 冷 Prefill 的每成员长度域资格

SGLang 生产端对已观测且各成员发布状态明确的模板声明候选域：每成员 context=0、2≤input_tokens≤page_size。此域保持每成员的多 token RoPE 分支和一列页表；计算节点须有参数关系，普通 DMA 须有明确不变量，通信须有任务/工作量合同。候选资格不代替准入 CPU、发布、KV、通信或完整依赖检查。

固定资产两请求127/128的每 rank Schedule/Prepare/Model/Publish 分别有0/10/1065/3个 kernel，全部有参数映射，见 s3-worker-main-coverage/prefill_length_domain_audit.json。原生 allocator 还以 B 为 grid、next_power_of_2(B) 和 next_power_of_2(sum(input_tokens)) 为编译参数，并受新增页数/空闲页与200页分支限制。开发输入127/65与来源均为两页，总 token 数192/255的向上二次幂同为256；这只说明该子机制一致，不能推导任意长度的内核资格。

2026-10-12 独立后端观测：当前 torch/torch_npu 2.10.0、CANN 9.0.0、910B3 上，预建 INT64、ND、每个形状为[1]的输入，分别执行一次1/2/4输入 Cat。三个用户 scope 均通过 async_npu flow 唯一关联到一个设备任务；1输入为 MEMCPY_ASYNC，2/4输入均为动态 ConcatD、一个 block，输入 shape/dtype/format 列表分别包含2/4项，输出长度对应2/4。安装的 dynamic/concat_d.py 转调 concat_v2_d，并使用动态输入列表；ACLNN 包装接口同样接收 aclTensorList。证据为 data/redesign/s3-concat-arity/verification.json，未导入任何服务耗时。

BatchParameterLayout 现以 BatchInputArity 区分固定输入个数与每请求一个输入。生产端先核对同型 INT64/[1]/ND 列表及 INT64/[B]/ND 输出；核心从来源的单成员描述重建目标输入 shape、dtype、format 列表，输出行数仍由 Requests 关系求值。输入个数与单个 tensor 的行数分别表达，不新增 Cat 专属核心分支，也不逐人数建立模板。该关系写入编译资产，由实际模板实例化消费。

s4-input-arity/verification.json 已通过五个既有组合的完整物理图、执行明细和指标对照（仅归一化资产路径），包括已支持的冷Prefill 2→1；本轮没有运行人数增长。

列表参数关系与操作结构资格分开：Cat 的 BatchOperationVariant 现表达单输入拷贝[1,1]与单ConcatD[2,32]。32来自ACLNN普通拼接的输入分组上限，不是拟合常数或按目标人数指定的特例。超过32个INT64标量时会产生多级设备拼接，现有单节点模板不能消费，保持明确拒绝；不修改核心控制器或增加人数枚举表。

后端依据：公开ops-math的[SplitToConcat](https://github.com/hicann/ops-math/blob/e5f6a2852afc84f9dc54dcd6d0bb0c8225f89cb0/conversion/concat_d/op_api/aclnn_cat.cpp)按32输入分组；[IsSupportConcatDV2](https://github.com/hicann/ops-math/blob/e5f6a2852afc84f9dc54dcd6d0bb0c8225f89cb0/conversion/concat_d/op_api/concat_d.cpp)要求尾轴字节数32对齐，INT64/[1]不满足。当前安装环境的边界观测s3-concat-arity-boundary/verification.json确认：32输入只有一个ConcatD，33输入先拼32项、再拼[32;1]，共两个任务；全部通过scope与async_npu流关联，各一个block。该机制证据与已有1/2/4观测共同限定本规则，未使用节点时长。

规则只覆盖当前固定部署的同型、连续标量列表；其他dtype、空输入、非连续布局、后端变化及多级拼接需各自资格。多批次资产请求池容量为2；s3-representative-pool另以容量4仅采双请求冷Prefill，固定该资产验证2→3/4的成员重复、共享Cat与输出、attention和KV槽位。容量不同需要真实来源，不能改manifest伪造更大池；局部Cat域也不能单独授予完整展开资格。历史二进制一致性、完整serving增长以及CPU成本未由单独观测证明，独立profiler包络不能替代serving内成员循环、队列端口及服务所有权。

### 人数增长与反向物化分别资格化

现有多请求 logits reshape 的变体区间为 B≥2，单请求为 CPU view。因此从双请求主体扩展到更多请求时，不必先实现 CPU view→设备物化；仍须核对参数、完整依赖和所有阶段的资格。共享 last_loc Cat 的列表关系及单节点[2,32]分支已接入；不能跨过32输入的多级拼接边界，也不能跳过其他阶段的结构资格。来源请求池容量与当前批人数也是两个条件，不能通过修改目标人数绕过容量资格。

只有选择单请求主体并跨入多请求分支时才需要反向物化。已有主来源的 Prefill/Decode、两个 rank 共四个 CPU view 及四个设备分支，按 CPU 图的 thread_order/interval_extent 可达关系均找到唯一前置 wait_event 与后续 aten::to；设备链由唯一 completion_join→stream_order 连接。四个 CPU view 对应的 worker 桥各含一个 CPU 服务及三个注解，虽无归属中间操作，也不能删除或计入新增 donor。多请求 donor 的 reshape 有自己的 worker 和设备任务。这些事实来自已有图，不使用时间排序，也不触发采集。

当前 plan_operation_replacement 要求接收方已有设备端口，故仍拒绝反向物化；plan_region_replacement 的新资源锚要求同父语义兄弟覆盖外部资源工作，也不能直接套到模型内部操作。后续若支持反向插入，应共同表达 CPU-only 切口的提交队列与设备流位置，保留原 worker 桥及外部等待/完成消费者，再复用 insert_subgraph；不得新增 logits 专属拼图路径。来源证据：data/redesign/s3-source-composition/view_insertion_contract.json。此项只证明已有观测可用于切口设计，未执行目标插入。

### 跨人数候选与完整结构资格

人数变化不再由入口的来源人数相等判断统一拒绝。跨人数候选必须有完整设备参数关系对应的长度域；冷Prefill的Schedule须有准入成员序列，Prepare须有请求初始化序列，Publish须有发布成员和共享输出参数。随后仍按同一个来源批次联合资格化全部阶段/rank，并执行操作、KV、传输、通信、端口及完整拓扑检查；缺失的成员分支或固定人数操作继续拒绝，不能把通过候选筛选当作支持任意人数。

RequestLengthDomain只保存每成员的长度范围，不重复保存来源人数。选择器仅在人数相同时逐槽比较来源请求；跨人数由目标成员列表、输出合同和来源参数域决定，避免按来源槽读取更短的目标列表。成员源可以保留、替换、重复或省略，但每次插入都使用目标槽位；语义区域的父调用与CPU服务的执行owner可能不同，例如prefill_statistics语义记录在Schedule下而服务由Prepare尾部拥有，共享工作须按实际所有权在整个批次中保留一次。

当前开发证据为 `s4-cardinality-qualification/verification.json`：63输入/1输出的单请求，以及staged序列后续单请求批次，均从双请求来源联合展开四阶段/两rank。单请求图已核对第二成员、准入三服务separator、原Cat设备工作和logits物化工作无残留；首成员服务和共享统计/输出各保留一次，KV分配/释放、位置、attention及输出描述对应一个目标成员。两个既有配置及two-step完整图保持；staged因优先来源现可完成跨人数变换，条件耗时随显式100/200 ns来源成本改变，不是精度结论。该证据未验证人数扩张、全部Decode转换或独立原生正确性。

### 长度候选域与结构规则

RequestLengthDomain只筛选来源候选，不是完整结构等价证明。冷Prefill每成员context=0、input在2到页大小之间；Decode每请求一个输入token，context在1到共享sequence_capacity_tokens减1之间。原先取来源最大context的样本包络已删除；每个候选仍须通过整批、各阶段/rank、CPU工作、页表、KV、发布及依赖改写，容量内不等于一定可展开。AttentionParameterLayout的shape关系本身仍不提供拓扑证明。

本次变更依据当前安装的ATB 9.0.0.B160（commit 8bf8f4054d644f14d3924a7f0b4d0467ef5cfa52）：普通PagedAttentionOpsRunner由固定结构参数建立attention节点，context进入kvSeqLen；GetBestKernel按类型/head维度选择kernel，CreateRunner按平台选择runner。长度影响BN/BNS和split-KV tiling；OpsRunner/MkiNodeImplement调用既有节点，安装libmki的KernelBase::Run/RunWithArgs反汇编进一步确认参数准备后走互斥的单次启动路径，没有按context重复启动。此结论针对已审阅的普通attention路径，不扩展到FIA、MLA、AICPU或任意硬件实现。内部tiling变化影响后续成本域，不能据此逐长度新增语义模板。源码、地址范围和限制见data/redesign/s4-length-domain-gap/atb-source/audit.json。

共同容量由TemplateSource.sequence_capacity_tokens唯一维护。SGLang生产端取模型位置上限、显式context_length（非空时）、请求表列数、逻辑max_total_tokens及已观测RopeWithSinCosCache行数的最小值。核心在整组调用选择/装配前检查context+input，包括精确来源匹配；缺容量报告sequence_capacity_not_observed，超界报告sequence_capacity_exceeded。当前来源这些边界分别为40960、未显式指定、40964、16384及41216，因此共享容量16384、Decode候选context上限16383。物理attention缓存129×128含额外存储，不能替代逻辑KV额度；动态页数、物化与释放仍由既有状态规则处理。几何证据见source_sequence_geometry.json及原native_environment。

已声明候选域拒绝时报告request_length_outside_parameter_domain，未声明参数域的变化保留internal_shape_transform_not_implemented；都不自动触发补采。旧s4-length-domain-gap曾在context=132拒绝，属于历史样本包络诊断。当前s4-decode-capacity-domain用同一原生来源、相同给定耗时完成128输入/7输出的全部7批；Decode context为128至133，每rank每次64个attention节点，目标长度、最终134-token序列的2页释放及全图依赖时间检查通过。两配置既有回归的全部组合字段在规范化资产路径后保持。相关输入属于开发证据，未证明整个候选域通过独立原生验收；跨下一KV页、容量超界及固定规则独立评价仍需覆盖。

### 冷 Prefill 长度候选的公共检查

联合选择保留同一来源批次各步骤/rank 的资格。当前 s4-prefill-transition 中，127/65两请求在基线与预算配置分别完整8/12阶段；此前四阶段后的 transition 缺口已通过 CPU-only 人数参数化修正。已执行部分通过参数、KV、发布及物理时序检查，属于开发证据，不是完整目标或独立原生验收。历史单请求17-token局部证据见 s4-single-request-prefill；1-token与超过一页的冷 Prefill 仍在声明域外。

Prefill 请求区域拥有 publish_prefix 或直接 allocator.free 工作；Decode 成员区域只承担 CPU 工作，释放组位于循环外。生产端据此绑定成员切口，核心使用共同区域替换。插入节点只保存一份目标成员槽位，CPU工作、请求序列 Cast 和直接KV释放共同使用；批级关系不被重排。直接释放按目标请求顺序累计之前释放的页，不能复制 donor 的槽位或页身份。s4-prefill-publication/member_replacement_verification.json 确认两 rank 各保留127并生成65长度的独立 prefix Cast 实例，重复 source_span 不被去重，旧释放 kernel、四个等待及其入边已删除。

区域替换的等待合同是：完成导出保留真实设备结束及完成汇合；等待输入必须有匹配资源的原生 stream-wait 记录，且完整 HostWait 节点属于切口。删除区域时移除其等待输入，保留 donor 等待时按资源找到目标切口唯一的入向 StreamOrder 前驱，再连接插入后的等待完成点。缺少或歧义前驱拒绝。donor 独有完成导出可成为内部点，recipient 的既有完成消费者仍须匹配。TemplateAssembly 的前批资源查询不能替代同批依赖重接。源证据见 s4-prefill-length-domain/{publication_interfaces,publication_crossing_edges}.json；接口修改后的六请求61/83阶段及实际图保持，见 s4-region-wait-imports。等待删除已动态验证，保留等待重接与直接释放重定位仍缺独立动态覆盖。

共享输出的资格与周期参数关系见[输出工作边界](#完成发布的输出工作边界)，保留有原生事实支持的单 CPU 服务；工作量根据非空目标成员计算。原生 output_streamer::to_payload 在请求批非空时即构造载荷，不以完成输出数量非零为条件；不能删除空输出步骤。目标 PublicationWork 描述各成员是否输出及长度，不再保存来源 output_count 限制；成员分支可否替换仍由模板计划检查。CPU真实成本留待S5，给定100 ns不证明成本适用或结构独立资格。

当前完整成员实现的六请求回归 s4-publication-members-main 已完成61/83阶段，阶段记录及指标保持，参数/状态/物理检查无错误，见 verification.json、behavior_comparison.json。下一步是后续 transition、未触发的替换分支和独立原生对照，不按缺口逐长度补采。

### Prefill 完成请求的直接释放

非 overlap 的 Prefill Publish 在请求循环内直接调用 allocator.free，Decode 则先登记各请求，再由 free_group_end 批量归还。生产端根据实际区域与请求归属区分两条路径，不能把直接释放伪装为释放组。直接 free 要求已知该槽位完成、is_not_in_free_group=1、need_sort=0，核对 free_index 长度、页数及 Cast/Concat 操作数，复用共同 KvParameterLayout。

直接释放合同带 release_request_slot；目标只归还该槽位的页，空闲页数等于批次入口空闲页减去本批全部新增页，再加上前序已完成请求归还的页。无槽位选择的 Decode 组释放继续汇总全部完成请求。两者共用 KvBatchWork 与逻辑页身份，不增加原生回调或第二个分配器。释放不再要求“一请求一输入 token”，该要求只约束 Decode 分配。

冷 Prefill 单请求的 2…page_size 候选域也可来自立即完成分支，但每个设备节点仍须有参数关系，目标完成状态和输出策略继续检查。CPU 的 free_index 搬运/去重与发送工作全部保留；节点成本后置，不能把长度变化记为零成本。此域不授予跨页、未知排序、成员增删或输出分支变换。当前固定资产验证见实施计划。

### HCCL 分支替换的跨 rank 边界

已有两种 Prefill 来源均包含 130 组通信（129 组 AllReduce、1 组 AllGather）。用真实 native_notify_signal 边连接任务归属，每个连通分量恰含两个 rank，所有跨边端点都有任务归属；配对不依赖列表顺序或 kernel 名称。AllReduce 小消息每 rank 为 8 个任务、4 个外部通知连接，大消息为 14 个任务、8 个连接。因此跨分支变换必须联合替换两个 rank 的任务及通知边，不能只替换一侧节点或修改 bytes。证据：data/redesign/s4-collective-replacement-boundary/boundary_audit.json。

TemplateCollectiveOperationPorts 已明确记录通信流的共同前驱、任务完成汇合点、外层完成点和通知连接索引。与普通设备操作不同，stream_order 入边从共同前驱散发到所有任务，任务结束再分别连入上层完成点，不能要求唯一的任务入口边。正式诊断中 520 个 Prefill、780 个 Decode 操作均通过该本地接口检查，证据在 data/redesign/s4-collective-operation-ports/verification.json；这不证明通信组替换已实现。

TemplateInstance.connections 由目标实例持有连接记录及配对结果，执行图、资源前沿和诊断统一消费。collective_replacement.cpp 先用通知边构造完整 rank 组并核对归属，再检查生产端的 CommunicationOperation、工作量轴/宽度与数据类型；在同一步骤、阶段和部署的已有来源组中选择目标 count 适用的变体。原生名称与 HCOMM 阈值仍只在生产端解释，不按错误字符串选择分支，不触发补采。

联合改写保留 host/worker 的入口/返回、通信流完成点及外层完成点，替换区域内部工作、流入每个任务的边、每个任务到外层完成的边及两侧通知连接。520 个 Prefill 操作的边界依据见 data/redesign/s4-collective-port-mapping/mapping_audit.json。CPU、设备流、任务完成和通信注解 lane 按物理资源或局部角色映射，不复用 donor 的全局身份。

insert_subgraph 返回 donor→计划边界映射，apply_operations 返回各计划的实际插入偏移；联合规划用两者绑定通知端点，TemplateAssembly 保留已绑定的目标端点。TemplateInsertedNode 持有目标通信任务描述，ExecutionGraph 消费其实际 bytes；不可变 donor 只作来源。完整候选仍在局部 selected 中，任一 rank 或后续步骤失败均整体丢弃，图和资源前沿尚未提交。

正式入口已让 127-token 单请求完成 Prefill。目标实际图包含 129 组每 rank 14 任务的 AllReduce 和 1 组每 rank 9 任务的 AllGather，1,036 条通知边均连接本组两个 rank；AllReduce 的目标分片为 655,360/645,120 字节。相同 donor span 被使用 129 次，但目标任务端点独立、图无环。原 base 和 17-token 输入的完整结果及实际图保持。证据：data/redesign/s4-collective-replacement/verification.json。

当前验证限定既有 TP2 部署内的小消息→大消息变体及显式每服务节点 100 ns 耗时，尚未验证反向替换、任意子通信域或独立原生目标。127-token 与 17-token 请求均停在后续 Decode 形状资格，不能将 Prefill 成功解释为完整请求或性能预测完成。

### Decode 首批复用的结构分支

RequestLengthDomain 显式保存来源请求数与有界长度范围。冷 Prefill 仍只授予单请求域；Decode 保持来源批次人数、每成员 input=1，各 context 位于 1…来源批次最大观测 context，不外推到更长序列。已有 attention 合同按每成员长度及批次最大序列生成工作和页表，长度不是请求身份；不以来源某个槽位的具体长度锁住目标同一槽位。每个计算、DMA 和 collective 都须有参数关系才授予候选域，控制入口、filter/merge、KV 拓扑、发布分支及实际 kernel 求值随后独立检查；域本身不证明完整结构可执行。

预计完成标志只筛选 Publish 的逐请求结束和释放行为。Schedule/Prepare/模型阶段消费当前成员、序列和已资格化控制工作，不要求其来源请求恰在同次迭代结束。完整四阶段仍联合选择同一来源批次，各 rank 和端口检查不放宽；Publish 的完成成员排列尚无重写资格时继续拒绝，不能通过提前阶段的 shape 匹配绕过。

Decode Prepare 的 alloc_for_decode 在写回请求表前执行 seq_lens.clone()，其数值为每请求的位置，拷贝大小只随请求数变化。生产端仅绑定 Prepare 直接 scope 中唯一的 aten::clone，核对 INT64 向量形状、D2D 方向及 requests × sizeof(int64_t) 字节，使用 TransferParameterLayout 的固定请求数关系；嵌套 allocator 拷贝不套用此规则。context 改变仍须通过全部计算参数、KV 分支和依赖资格。两 rank 共 10 次真实拷贝的证据见 s4-decode-prepare-domain/clone_qualification.json。

以下三组是旧资产的历史取证，事实索引为 data/redesign/s4-decode-branch-inventory/branch_inventory.json。20261011-025519 资产有五个 Decode 批次：首批双请求 context=127/128，随后单请求 context=128、129、130、131；两 rank 都如此，见 data/redesign/s4-decode-context-domain/source_inventory.json。当前仍联合选择同一来源批次的各步骤与 rank。

| 来源批次 | context | Schedule 过滤 | 新增页 | Publish 完成标记 |
| --- | --- | --- | --- | --- |
| 2 | 127、128 | 两次 2→2，无实际删减 | 1 | false、true |
| 3 | 128 | 一次 2→1，执行选择 | 1 | false |
| 5 | 129、16 | 1→1、2→2，另有批次合并 | 0 | true、true |

127-token 与 17-token 目标的首次 Decode 都是两次 1→1 过滤、零新增页，发布标记分别为 false 与 true。唯一单请求来源 3 与它们在控制/分配分支上不一致；因此 internal_shape_transform_not_implemented 不能通过扩大长度域解决。

上述旧资产的 10 个 Schedule 实例没有独立过滤语义区域，只有外层 batch_filters 的 before/after 请求身份；不能据此按 Index 名称或位置推断整个过滤 CPU/设备切口。核对记录见 data/redesign/s3-filter-region-contract/source_audit.json。

过滤观测现已复用 BlockWrapper：C++ 声明 sglang.batch.filter 及显式类 hook，Python 在每次调用入口分配 filter_index，关联父 Schedule 的 batch_filters 项，并在原生返回后记录 after；空、全保留、实际删减都使用同一个区域合同，不拷贝设备值或增加同步。类 hook 与组件实例安装互斥，替代原来仅记录请求列表的回调。公开 prepare 到 Python wrapper 的合同交接证据见同目录 contract_audit.json；后续联合实采已取得区域并接入 Prefill transition 替换，Decode 更新分支仍待完整实现。

模板生产端现按区域的 filter_index 绑定 Index 节点，校验其直接父 Schedule 和索引范围，再交接统一 BatchFilterBinding；指定过滤范围内的其他操作不会误用批次合并规则。无独立区域的 Schedule 只有在实际删减调用唯一时才能绑定，多个候选仍返回 batch_filter_scope_ambiguous。记录为全保留或清空却出现 Index 时返回分支矛盾，不能以形状相同覆盖身份。此消费路径已编码；固定资产的完整结果及实际图保持，证据为 data/redesign/s3-filter-scope-binding/verification.json。真实多过滤区域的节点归属及整区域替换仍需原生证据。

原生 ScheduleBatch.filter_batch 在全保留时直接返回，实际删减才执行索引和成员重建。NPUPagedTokenToKVPoolAllocator.alloc_decode 在 num_new_pages==0 时只使用 last_loc+1，否则读取 free_pages 并组合旧页/新页位置，二者是不同任务结构。零新增页区域已有双请求观测，可作为区域复用的候选，但不能据此认定整个单请求 Prepare 或整个批次已合格。完成发布仍需核对单请求归属、循环次数及释放端口。

公共 TemplateSubgraphRegion 现同时接收原生操作窗口和插桩语义区域的成员，complete_region 统一划分内部、进出、保留边、跨模板连接及旋转位置依赖；Prepare 的正式诊断消费该切口。现有 12 个 Prepare 语义切口的依赖分区互斥，六个 Decode allocator 均为一条 stream_order 和两条 thread_order 入边/出边，无跨模板或旋转位置依赖，另有两条区域外 interval_extent 必须保留。新增页来源分别有 494/452 个节点、18 个设备完成汇合，零新增页来源有 218 个节点、9 个汇合。证据：data/redesign/s4-semantic-subgraph-cuts/verification.json；这里是源边界资格，不是目标区域替换已成立。

region_ports 已在共同子图合同上检查资源入口/出口唯一配对、服务节点归属及内部路径可达性，并拒绝尚未映射的跨区域接口。六个 Decode allocator 均得到两个 CPU 线程和一个设备流的端口，设备出口以真实 stream_order 后继确定物理流，不把任务完成 lane 当作物理资源。另六个 Prepare 区域因外部连接或出口关系不在当前合同内而返回缺口，未自动扩大范围。

两条保留 interval_extent 属于 allocator 前后的 Prepare CPU 服务：各只有一端触及 allocator 端口，另一端位于区域外。资格只要求区域内端点可映射，区域外服务和完整区间边仍保留；不能误把区域外端点也要求纳入映射。证据：data/redesign/s4-region-resource-ports/verification.json 与 retained_boundary_audit.json。端口资格仍不等于分配分支替换或目标工作量已通过。

分配区域替换已接入 plan_materializations：生产端仅在该区域所有计算节点都有一致的 KV 分支合同时导出分配类别的 KvRegionBranch，核心用 KvBatchWork 判断目标是否新增页，选择已有同阶段/rank 的相反分支。plan_region_replacement 按资源端口重绑定 CPU/设备入口与出口，复制完整节点和内部依赖；内部任务完成点具有目标局部身份，区域外边及 Prepare CPU 工作保留。插入 kernel 继续使用 donor 已有参数合同，缺工作量关系仍拒绝。

分配和释放现在共用 KvRegionBranch 与 plan_kv_region：前者由目标新增页数选择分支，后者同时按目标完成成员数选择释放组，空/非空相同但成员数改变时也进入共同区域替换；候选donor须覆盖目标完成成员域：空组[0,0]、单成员拷贝[1,1]、已资格化INT32/ND一维ConcatD列表[2,32]。32来自已记录ACLNN普通拼接分组机制，超限多级拼接另行支持；不按人数自动补采。SGLang 生产端把直接属于 Decode Publish 的 free_group_end 及已知发布状态编译为释放类别；核心不识别原生方法名。选择同阶段/rank、同类别的 donor 后统一调用 plan_region_replacement。相同分支内的成员数量、CPU 工作及完整发布状态仍分别资格化，不能只凭布尔分支相同就认定整段可复用。 批次 2 的两个单请求预览已进入共同规划器，随后通过下述完成端口资格实现局部替换；历史缺口定位见 data/redesign/s4-kv-region-branches/verification.json。

公开的单请求拓扑预览已验证两 rank 新增页→零新增页替换：各移除 allocator 的 494 个节点、插入 218 个节点，9 个目标 kernel 均求值成功。目标 context=127、input=1，对已有页的 slot 255 写入一 token，new_page_count=0。原请求表写入的 36→46 节点替换逐项保持，因此组合预览总量为 530→264；不能混为 allocator 规模。边界边保留，组合图无环。证据：data/redesign/s4-decode-allocation-replacement/verification.json。此为诊断副本，实际 base/target 图保持；反向分支及独立原生目标未验收。

已有资产另提供可研究资源撤销的真实区域：两 rank 的 Decode free_group_end，在批次 3 无结束请求时各有 2 个节点、一个 CPU 资源端口；批次 2/5 有结束请求时分别有 118/124 个节点。原生 allocator/base.py 在 free_group 非空时执行 cat 和 free，空组直接返回；不能把嵌套 free 的“登记到组”和“真正归还”按同名方法互换。

Publish 的公共区域诊断已启用。非空释放组内有两次 stream wait，Publish 外围另有一次；空组只有外围那次。变换须撤掉组内 wait 并保留外围 wait。目标等待集合随保留/插入节点重建，TemplateAssembly、ExecutionGraph 和报告共用该集合；插入等待的完成边界随拓扑偏移迁移。初次资源接口审计见 data/redesign/s4-region-resource-inventory/{inventory,publish_ports,release_interfaces,verification}.json；其中的端口拒绝数量是历史结果，当前资格与替换证据如下。

跨模板普通顺序连接已纳入区域端口配对，原连接保持。批次 2 向外导出的 native_host_wait 现已核对为设备流唯一终止工作的完成点，两 rank 均通过一条 completion_join 连接流出口。资源端口保留这两个身份，撤销工作后用 stream_entry→device_done→stream_exit 的纯依赖连接，不生成设备服务。末批次 5 无后继工作时，现从区域真实服务/等待的完成点按内部因果路径推导唯一终止出口；两个 rank 的 free_group_end 均通过主 CPU、worker CPU、设备流三组端口检查。证据为 data/redesign/s4-terminal-region-ports/port_changes.json，除这两处端口诊断外完整结果及实际图保持；没有追加后继请求制造边。批次 2 的两个非空→空预览现均移除完整区域的 118 个节点、插入已有空组的 2 个 CPU 节点；两条内部 wait 节点删除，外围一条保留，区域外边及完成汇合保留，局部图无环。证据为 data/redesign/s4-release-completion-ports/verification.json。实际图保持，尚未执行完整 Publish/Decode 或独立原生目标验证；组外逐请求工作、完整候选资格及后续目标等待仍需处理。

释放组之外的工作已单独核对：批次 2 两 rank 各保留 34 个节点，包括 17 个 cpu_control、16 个 cpu_observation 和 1 个 host_wait，没有设备 kernel。其中仅四个节点属于 free_group_begin/free 区域，其余没有内部区域归属。原生 process_batch_result_decode 在释放组之间逐请求追加 token、更新结束状态并调用 _handle_finish_state_updated_req，后者经 release_kv_cache → ChunkCache.cache_finished_req 登记释放；循环外还有批次输出和统计。因此组外 CPU 工作不能随释放组一起删，也不能按人数缩放或平均拆分。证据：data/redesign/s4-publish-residual-work/{residual_work,audit}.json。

逐请求发布的观测边界覆盖完整循环体，显式关联 request_id、request_index、父 Publish 与嵌套 allocator/prefix 区域；批次共享工作单独保留。SGLangRequestPublication 按实际 AST 的生成循环及变量结构插入 scope，不改官方源码、不按行号匹配。Prefill 使用 enumerate(zip(batch.reqs, next_token_ids))，Decode 使用 enumerate(batch.reqs)，embedding/reward 的另一循环不安装。共同 BlockWrapper 维护正常、continue 与异常退出的记录生命周期。同方法区域组合改写的合同见下文 Schedule 插桩；历史静态证据见 s3-request-publication-contract，真实边界已由 s3-admission-work-capture 联合资产取得。

**输出策略与共享发送。** SGLangPublishWrapper 从 worker 已加载模块记录有效 force_interval_tokens 和 non_streaming，提取为 SourceOutputPolicy，再编译 PeriodicOutputPolicy；缺失仍为 unknown，不填环境默认值。普通非流式请求结束时输出，未结束则在有效周期的整数倍输出。RequestSlice.generated_tokens 是动作前的生成数，周期判断使用本轮追加后的数量，首次/后续增量输出分别表达。具体公式见[重复区域与批次共享工作](05_controller_semantics.md#重复区域与批次共享工作)。

请求不输出 token 不等于批次不发送：to_payload 在 reqs 非空时仍构造 BatchTokenIDOutput，即使 rids 为空，外层也执行 send_output。真正输出时还涉及距上次发送的 token 数、首次/后续 incremental detokenize，以及 prompt 尾部拼接；不能仅按结束状态或输出布尔值复用全部共享工作。

共享区域 sglang.publish.output / batch_output 绑定 SchedulerOutputStreamer.stream_output 类级 hook。该对象是 slots dataclass，使用 SGLangOutputHook 将原生函数绑定到本次对象，再交给继承 NativeRegionWrapper 的 SGLangOutputWrapper；不改实例布局或另建参数解析器。记录 is_generation、return_logprob，请求与进度取父 Publish；整个区域包含输出选择、payload 构造和消息发送。旧实例安装失败已由类级绑定修正，来源和修正经过见 s3-joint-serving-capture。

**Decode 成员重绑定的实现合同。** 最新资产已按 request scope 核对来源批次 4/7/9：未完成成员为 1 个 CPU 服务和 1 个观测节点，完成成员为 7 个 CPU 服务和 6 个观测节点，两者均单入口/单出口；逐请求 free 只登记待释放索引。设备工作集中在循环后的 free_group_end，不能随请求区域重复插入。原生顺序为 free_group_begin → 逐请求循环 → 一次 batch_output → 一次 free_group_end。证据见 data/redesign/s4-publication-membership/region_inventory.json；计数仅证明观测构成，不单独授予替换资格。

成员分支现可复用同一TemplateBank中其他调用的已资格化区域。先尝试原调用，再选择同step/phase/rank的donor；成员donor只要求输出间隔、解码上下文上限和sender一致；donor原批人数不属于单成员的语义资格。recipient的共享输出由PublicationParameterLayout.evaluate按目标成员计算，非空请求批仍构造一次载荷；整批人数资格由TemplateBank统一检查，不能用成员可复用代替整批变换资格。只替换所需完成/继续成员的语义切口，既有region_ports和资源连接检查继续执行；插入节点绑定目标成员槽，来源span仅用于追溯和耗时查询。共享batch_output与聚合释放仍由recipient及原有KV规则处理，不能随成员重复插入。此处没有混合不同模型/配置资产，也没有改变批人数。请求级BatchParameterLayout在循环装配显式提供目标slot时，按该slot计算请求序列长度，不再同时要求donor原批人数相等；未移植节点继续检查来源人数。KV单成员释放沿用已有目标slot和先前释放状态。s4-publication-member-donors/verification.json的两个既有配置及单请求Prefill完成输入均通过，前两者指标/执行摘要保持；实际来源核对未发现跨人数成员donor，因此新增候选及参数分支尚未取得运行覆盖，不算跨人数验收。

s4-publication-cross-call验证在两rank分别从调用792/5933复用继续成员到4596/9737：每rank一个成员服务、一个保留的共享输出服务，目标槽0与生成进度5正确，成员→共享输出→Published可达，复用来源span的不同目标调用使用独立边界，全图依赖时间不逆序。执行由4批推进到5批，随后如实拒绝context=132。既有两配置成功输入的所有组合字段与此前一致。证据verification.json及regression_verification.json；不据此宣称任意输出策略、跨人数或独立原生验收通过。


实现复用现有区域替换、端口和参数合同：

- 生产端按 request_index 编译每个请求的完成分支、区域及范围；目标槽位选择相应已观测分支，按目标顺序替换循环体。槽位映射局限于请求区域，不能重排 Prepare/model 的批次。保留外部服务、CPU 顺序及原等待；逐请求 CPU 区域无设备参数不代表其成本相同。
- 共享 PublicationWork 扩为有序成员工作描述，记录扫描成员、实际输出成员及各自生成/解码上下文数量，仍绑定唯一输出 CPU 服务。已观测 before/after 覆盖零、一、两个首次完成成员；未完成成员在非输出周期保持 offset，完成成员从零 offset 推进。数量、成员顺序、发送 rank 和有效模式都要资格化，不能复制单请求工作或按总 token 缩放时延。
- 聚合释放继续从目标完成成员按目标顺序计算序列、页和 DMA；只装配一次 free_group_end。已有单完成成员DMA与多成员kernel关系可复用，分支域及对外完成端口由释放区域统一检查。多成员列表节点按目标完成序列重建shape/dtype/format，归还页的后续节点按目标总页数求值，不再重复保存来源成员数或为每个来源成员保存参数轴。当前大于两成员的完整Decode运行尚未验证，区域域声明不代替整批资格。FinishedSequenceTokens 只要求 Decode 和完成成员数与来源一致，字节为目标完成序列 token 总量乘元素宽度；未完成请求不进入 free_group，不再要求整个 batch 人数相同。固定字节分支仍保留来源人数限制。此局部关系不单独授予跨人数 Decode 资格。
- 共享输出参数已通过实际交接验证；生产端现为完整普通 Decode 循环编译有序纯 CPU 区域，核心仅在这份资格存在时允许完成槽位不同，并在 plan_materializations 中按目标槽位执行同源分支替换。共享输出人数、聚合释放及全部端口仍须通过；固定资产 100/200 ns 下均已从 12 推进到 16 阶段，来源 [继续,完成] 变为目标 [完成,继续]；两个 rank 的完成区域 13 节点先于继续区域 2 节点，随后进入唯一共享输出，释放 132 字节并归还槽位 1 的页 [1]。证据见 s4-publication-rebind/{100,200}/verification.json。后续过滤索引资格已进一步通过下文 Decode 更新参数关系解决；该组输入属于开发证据，不代替独立并发验收。该历史证据仅覆盖普通非重叠、无可选载荷的首次输出；当前周期关系见[输出工作边界](#完成发布的输出工作边界)。无需为 [完成,继续] 另采排列样本。

**未完成且无 token 输出的 Decode 参数域。** 生产端要求全部请求明确未完成且周期策略不输出，每个请求恰有一个合格的纯 CPU publication 区域；整个 Publish 只能含 CPU 节点和已记录的主机等待。目标请求数、完成状态保持，改变生成进度后仍须落在不输出 token 的分支。所有节点、空批次发送、端点及外部等待保留，核心不复制原生输出器，也不填零耗时。真正输出、完成释放或缺策略仍拒绝这一参数化。

六个真实来源（两个 rank、三个生成进度）各有 30 个 CPU 节点及一次主机等待；以调用身份解析插桩标记到真实 binding 后，31 个节点的操作身份/端点别名和 60 条内部依赖一致。证据 s4-publication-cpu-progress/{source_inventory,normalized_topology,topology_verification}.json。此域只资格化给定耗时下的结构，S5 仍需根据目标 CPU 工作估计耗时；内部结构相同不证明外部端口可删除，也不代替完整目标装配验收。

Schedule 合并不能只按 Concat 数量处理：get_next_batch_to_run 先过滤 last prefill，running 为空时直接接管，非空才 merge_batch，随后 update_running_batch 再过滤。sglang.batch.merge 现已声明独立区域，与过滤共用 SGLangBatchWrapper；记录进入时的 filter_prefix_count、left_requests、right_requests，不新增可变请求列表。空 running 的直接接管不伪造 merge 调用。

原生合并发生在下一批 phase 选择之前：若随后接纳新 Prefill，就不会调用本轮 running filter。轻量控制器已遵循这个顺序；生产端的显式 merge 区域因此不能依赖 Decode 或两次过滤。生产端核对父 Schedule 或其成员转换区域、prefill filter 后的位置、两侧非空及右侧人数与首个过滤结果一致，以区域参数建立 BatchMerge；如果两次过滤已提供合并关系，再核对一致性。当前支持一次合并，空 running 的接管不生成该关系。安装版原生源码证据：data/redesign/s3-merge-phase-contract/source_audit.json。

显式 merge scope 仅使用 BatchLayout::bind_merge，外围节点不再借用该合并关系；filter scope 仍只使用指定过滤规则。公开 prepare 到共同 wrapper 的交接、实际安装方法签名和捕获范围外跳过已核对（data/redesign/s3-merge-region-contract/contract_audit.json）。真实 merge 切口与 filter/接管/merge 的整段序列变换仍未验收，旧资产没有这些 scope，不能将记录三个参数视作完成调度变换。Prepare last_loc 与共享 CPU 边界仍按联合观测准入表推进，不单独补采。

合并前缀的有限行数资格：生产端要求pending过滤全保留、整个transition只含CPU观察/服务与已绑定MergeLeft/Right/Total的Concat，复制、等待、通信和其他计算不满足该资格。目标保持pending人数、其过滤选择和merge右侧人数，左侧须非空且等于进入transition的running人数；后续running过滤仍独立规划。此规则复用当前固定非overlap/non-spec及采样域对可选字段的限定，不能扩展到任意导入来源、可选字段变化或其他合并分支。CPU节点和端口全部保留，目标行数改变不表示CPU耗时已可预测。该资格保存于编译资产的transition_merge_parameters，旧资产需用已有事实重编译，无兼容回退；开发回归已验证2+2→4的14个Concat及随后4→2的14个Index、同rank合并到过滤的因果可达性，证据s4-fixed-assets-acceptance/merge_structure_verification.json；新的独立输入及原生结构对照仍待完成，不认定完整变换验收。

过滤区域的分支资格还需独立于人数变换。安装版 filter_batch 先处理空保留集（清空 reqs、保留不再使用的旧张量），再处理全保留（直接返回），其余路径才构造设备索引、选择批次张量并调用 sampling_info.filter_batch。空输入同样先走清空分支；两个提前返回路径仍包含按输入成员构造 keep_indices 的 CPU 工作，不能当作无工作。部分过滤中的 input_ids、seq_lens_cpu、采样张量、bias 和 penalty 等条件会改变内部工作，必须由已声明范围固定或由观测事实解释，不能只用输入/输出人数资格化整个区域。

filter scope 不包含 update_running_batch 中随后的内存检查、回退判断、比例更新和嵌套 Prepare；过滤后为空会跳过这些工作。admission 与接管也位于过滤区域之外。因此删除 Index 或复制一个全保留区域，均不足以证明整个 Schedule 调用序列可改写。源码证据：data/redesign/s3-filter-branch-boundaries/source_audit.json；未执行原生代码或追加采集。当前固定资产的可选状态核对见下文；后续聚焦外围工作与调用序列的组合资格，不新增逐 kernel wrapper。

已有环境与采样记录可限定当前主线的可选状态，无需逐张量新增插桩：

| 状态 | 当前依据与边界 |
| --- | --- |
| sampling_seed、自定义处理 | 原生有效参数记录 deterministic=False、custom_logit_processor=False、无 speculative_algorithm；seed 张量不创建 |
| greedy、logprob、输出类型 | 10 个采样区域均记录 greedy、无 logprob 和 NPU next_token_ids 张量 |
| input_ids、seq_lens_cpu | 非 overlap/non-spec 原生路径将输出送入 relay 后清空 input_ids；普通准备路径创建并更新 CPU 长度镜像，GPU-only 清空路径属于 spec；这是源码推导，非逐批状态观测 |
| penalty、bias | 当前采集入口仅传固定长度、ignore_eos 和 temperature=0；当前原生默认值使四类 penalty 与 bias 关闭。该结论依赖入口及默认值，不能推广到任意导入来源 |

来源证据集中在 data/redesign/s3-schedule-state-domain/source_audit.json。已有两 rank 的部分过滤各 7 个 Index、合并各 7 个 Concat，与三个批次设备张量及四个采样张量的固定路径一致；这是数量一致性检查，不是每个张量的语义身份或目标拓扑证明。CPU 张量选择/拼接仍有工作，不能因为不产生设备 kernel 而忽略。来源路径准入现由 SGLangBindings::supports_native_path 统一维护：核对声明配置与 effective_server_args 一致，复用 control_limits，确认模型架构及非 spec、非确定性、无自定义 logits/rl_on_policy 路径。AscendSourceExtractor 与模板生产都调用它；生产端不只信任资产的 semantic_resolution 标签，路径范围外报告 native_source_path_not_implemented。真实资产的公开提取与预测保持证据见 data/redesign/s3-native-path-qualification/verification.json，其他原生路径的拒绝尚无独立资产验证。该入口准入不等于完整区域资格：采样入口/default 的来源约束、真实 filter/merge 切口及外围 CPU 序列仍待闭合。

成员转换前缀由 SGLangBatchTransition 复用内联 scope：包住 get_next_batch_to_run 中准入 phase 选择之前的语句，记录运行人数、是否有待处理 Prefill、该批人数和入口满批次状态。覆盖首个过滤、空运行批次接管及非空合并，准入与其后的运行批次过滤留在区域外。生产端核对首个过滤与待处理 Prefill 的人数，允许该过滤和 merge 以转换区域为父；其他嵌套路径不因此获得资格。

同一 get_next_batch_to_run 上先注册内联 hook，再注册 Schedule hook，使 Schedule 观测在外层。内联编译只支持原生 scheduler_nvtx_method 装饰器：从其直接函数体重建，并按原 AST 应用一次原生 profiler 装饰器；遇到额外闭包包装拒绝，不越过其他插件。安装版装饰器、HookRegistry 顺序及 AST 保持的静态证据见 data/redesign/s3-batch-transition-contract/contract_audit.json。真实运行及嵌套 filter/merge 消费仍未验证；这只是 Schedule 的内部区域，不新增 serving 步骤或证明序列改写已完成。

Schedule 的准入入口与最终计算批次还需分别解释。现有 visible_requests 将 waiting_queue、running_batch 和 last_batch 的身份顺序拼接，没有分组边界、last phase 或 batch_is_full；过滤列表只记录实际调用，不能直接证明未执行哪个准入分支。原生 _get_new_batch_prefill_raw 在队列为空/批次已满时提前返回，没有可分配槽位时也提前返回；之后才计算优先级、构造 PrefillAdder 并尝试等待请求。同样返回 Decode，不代表执行了同样的 CPU 工作。证据：data/redesign/s3-schedule-entry-boundary/source_audit.json，核对了安装版源码及现有 10 个 Schedule 实例。

完整序列资格须同时解释准入前的等待/运行/待合并成员、容量条件与满批次状态、实际准入尝试，以及接管/合并和后续过滤。目标状态继续由轻量控制器维护，不能复制源 batch_is_full 或新增并行调度器状态。若使用来源历史推导这些条件，必须说明并验证推导依据；当前扁平列表本身不构成该证据。有限变换先围绕整段准入/成员转换边界建立，不能把增加一个全保留 filter 调用当作完整 Schedule 改写。

准入函数包含嵌套 Prepare，不能把整个方法直接当作单一 Schedule 区域。SGLangAdmissionPrefix 复用 SGLangInlineRegion，只包住 _get_new_batch_prefill_raw 中 new_batch.prepare_for_extend() 之前的语句：提前返回仍关闭 scope，局部变量继续供 Prepare 与后缀使用。入口只记录 waiting_requests、running_requests 和 batch_is_full，不读取设备值或复制请求状态。生产端核对直接父 Schedule、非负人数与布尔标志；当前固定路径若选中 Prefill，入口必须非满且等待人数不少于选中人数。

公开 prepare 的声明交接、安装版源码的 AST 还原与静态编译已通过，证据：data/redesign/s3-admission-prefix-contract/contract_audit.json。尚无真实准入区域记录，入口参数消费分支、CPU 切口和目标序列改写未验证；三个入口参数本身不证明完整准入路径等价。SourceCall::serving_parent 继续提供跳过观察层的阶段归属，但不因此允许任意跨 serving block 区域替换。

准入之后的运行批次更新采用 SGLangDecodeUpdatePrefix，包住 update_running_batch 中 batch.prepare_for_decode() 之前的前缀，包含过滤、空批次提前返回、内存检查及原生回退判断。入口记录 request_count、batch_is_full 和此前过滤次数；生产端核对对应 running filter 的输入人数与序列位置，并允许过滤区域嵌套于这一已声明父区域。Prepare 和函数后缀仍在 scope 外，记录回退相关工作不等于轻量控制器支持回退。

三类 Schedule 前缀共用 SGLangInlineRegion.wrap_prefix，保留 docstring、局部变量、提前返回及原生主体，不分别维护 AST 切分算法。安装版三个函数的静态核对与旧资产公开预测保持证据：data/redesign/s3-decode-update-contract/{contract_audit,verification}.json。真实切口、嵌套过滤消费和目标序列改写仍未验证；新增声明本身不触发采集或放宽支持域。

序列展开的共同合同见[重复区域与批次共享工作](05_controller_semantics.md#重复区域与批次共享工作)。下一步处理 Schedule 过滤和发布/释放的有限变换，将已实现的分配区域规则纳入完整候选资格。当前整批来源选择及跨步骤 CPU 时间投影仍耦合；若改为分别选择步骤，必须先用目标端口和资源前沿证明跨步骤装配，不能仅删除同源批次条件。现有事实缺的是复用资格与部分请求区域身份，不触发自动采集。

## 模型计算关系

### 3.3 主线 Qwen3 模型结构与参数化落点

2026-10-10 首次源码审阅依据：官方 SGLang 子模块提交 `0460f277b770f8b5a404da2e50e7a9573ef8db40`。以下是准备实施的映射，不是已生成模板或已证明 kernel shape 泛化。源码入口为 [Qwen3](../../third_party/sglang/python/sglang/srt/models/qwen3.py)、其继承的 [Qwen2Model/MLP](../../third_party/sglang/python/sglang/srt/models/qwen2.py)、[LayerCommunicator](../../third_party/sglang/python/sglang/srt/layers/communicator.py) 和 [Ascend attention](../../third_party/sglang/python/sglang/srt/hardware_backend/npu/attention/ascend_backend.py)。

参数以模型配置和实际初始化对象为依据：H 为 hidden_size，I 为 intermediate_size，D 为 head_dim，Q/K 为实际 rank 的 query/KV head 数；T 为本次执行 token 总量，B 为请求数，L_i 为各请求已缓存 context，P_i 为各请求本步输入 token 数。非分块 prefill 的 T=ΣP_i，普通 decode 的 T=B；KV 位置/页表由当前目标状态决定，不能复制源地址。Q/K 包含分片或复制条件，不默认以 TP 直接除所有维度。

| 模型语义区域 | 原生位置与已确认关系 | 动态参数/分支及实施要求 |
| --- | --- | --- |
| embedding | Qwen2Model.forward 的 embed_tokens | 输入 T 个 token，模型输出 H 维；TP 相关通信由具体 embedding 实现补充，不凭模块名字插边 |
| 层入口与残差归一化 | Qwen3DecoderLayer.forward → layer_communicator.prepare_attn | residual 是否存在、是否空输入、scatter/fusion 条件影响路径；首层不能无依据套用中间层物理模板 |
| QKV 投影及位置处理 | Qwen3Attention.forward_prepare_native / forward_prepare_npu | qkv_proj 与 Q/K/D 关联；Ascend extend 使用 native 路径，decode 使用 npu 路径，后者有融合 split/QK norm/RoPE，不能只替换 T 就复用同一物理图 |
| attention/KV | Qwen3Attention.attn → RadixAttention → Ascend backend | T、B、L_i、P_i、KV 页与后端元数据；forward_extend/forward_decode 是不同入口，内部实现条件尚需逐项审阅，不宣称统一 shape 规则已覆盖 |
| 输出投影与层中间通信/归一化 | self_attn.o_proj、layer_communicator.prepare_mlp | 保留实际 RowParallelLinear 与 communicator 的通信/残差关系，不预设每层固定通信次数 |
| MLP | Qwen3MLP 实际复用 Qwen2MLP：gate_up_proj → SiluAndMul → down_proj | T 可变，H/I 与分片来自初始化；gate/up 合并为实际投影，不能凭模型逻辑名称虚构两个 kernel |
| 层尾 | wait_cmo_stream 条件及 postprocess_layer | CMO 是否启用需要运行环境证据；同层返回不等于所有设备完成，资源依赖不能省略 |
| 最终 norm 与输出 | Qwen2Model.norm、Qwen3ForCausalLM.logits_processor/lm_head | residual 分支及输出位置选择；LogitsProcessor 可从 prefill hidden states 选择每请求末位置，不能把 lm_head 工作量一律设为所有 T |

这张表明确需要的语义来源和分支，不以层数复制 wrapper。优先把模型模块路径、层实例、phase、参数来源及实际执行分支作为区域事实交给公共提取器；底层 SDK 身份继续负责关联设备工作。尚未覆盖的 attention、communicator、layout/融合分支先查源码和已有环境证据，再制定采集输入，不先运行一个 demo 等缺口出现。

TemplateBank 以同源 batch 保持跨步骤关系，精确长度之外已使用 RequestLengthDomain 与各节点参数关系联合资格。Publish 完成槽位和过滤状态仍属独立控制分支，不能因长度域通过而直接解除。后续模板族归并及跨来源装配须证明参数、端口和依赖等价，不能仅删除实例匹配条件。

### logits 选择分支的观测合同

新增 `sglang.qwen3.logits.selection` 绑定原生 `LogitsProcessor._get_pruned_states`，作为 logits 父区域中的子区域，沿用同一 wrapper 与子图提取。该位置已使用 LogitsMetadata，统一声明 call_parameters 从实际方法参数读取 forward_mode、extend_return_logprob、padded_static_len；不将调用时分支值存成固定模块属性。输入记录 hidden_states 布局，声明的返回槽 0 记录 pruned_states 的 shape/stride/dtype/device。只读取 CPU 标量和张量布局，不读取设备值、不插入同步。

通用 NativeRegionBinding 分别声明模块属性、调用参数路径、输入张量及返回 tuple 的命名槽位；Python 在公共 completed 生命周期记录返回布局，C++ SourceNativeRegion 保存这些事实并交给模板。未声明的参数或返回槽不猜测、不自动遍历；声明后缺少记录由输入边界报错。新选择绑定属于第十项；native/NPU QKV 绑定另声明 Q/K/V 三个返回槽和 q_norm.weight/k_norm.weight 两项模块张量输入布局。module_inputs 与调用参数张量均在调用时读取布局；两者在 inputs 中按声明名称保存。旧资产缺少新 QKV 必需字段或选择区域，不能据此宣称已有这些事实；缺失时不补默认值或兼容分支。

原生代码显示普通非 padding prefill 且不返回 input logprob 时，每个请求选择其末输入位置；decode 等另有路径。后续应以实际分支值、选中布局和请求身份建立请求数/输出位置到节点的关系。当前合同已通过一次有限原生取证：两个 rank 均观测到普通 prefill 的 [24,5120]→[2,5120] 选择，以及 decode 的两行/一行返回，未返回 input logprob且无静态 padding；分支值及返回布局已完整进入模板，普通非 padding prefill 的行关系已接入，整体 logits 变换仍未资格化；不把 B=1 的矩阵形状当成任意目标每请求一行的证明。新增观测需求纳入有边界的建库计划，与其他缺失事实统一审阅后再采集。

已接入的选择关系限定原生 EXTEND、extend_return_logprob=false、padded_static_len=-1，并核对输入 `[T,H]` 与返回 `[B,H]`。长度 Cast、Cumsum 和末索引 Sub 的行轴来自 B；Index 的激活输入来自 T，索引向量及输出来自 B，固定维度描述槽位不变。统一 TokenAxis 以 extent/multiplier 表达两种来源，删除原 token_multiplier 字段，不保留别名。Index 的目标节点附带 logits_selection.last_token_indices，按目标请求 input_tokens 的累加和减一得到，来源明确为 target_request_input_lengths；这些是派生语义，不冒充采集到的索引张量值。decode 不创建不存在的选择 kernel，padding、input logprob、speculative 等分支不套用此关系，lm_head 行关系使用下述父子证据，完整拓扑继续单独处理。

lm_head 的矩阵和最终 BF16→FLOAT 转换从所属 logits 调用的唯一直接 logits_selection 子区域取得行策略。缺少选择子区域或同一父调用出现多个选择子区域时不绑定；仅接受普通 prefill/decode、无 input logprob、无静态 padding，并核对父/子 hidden_states 与真实 pruned_states=[B,H]。矩阵按 `[B,H] × [V_rank,H] → [B,V_rank]`，类型转换按 `[B,V] → [B,V]`，两端行轴都来自请求数 B。权重分片、词表宽度、dtype/format 与通信结构保留原观测，不按 TP 数值重新推断；诊断 row_policy_source 指向唯一直接选择子区域。父节点仍由其原始 scope 归属，不将子区域中的选择节点重新标成父区域工作，也不重复计费。这些关系不证明其他 logits 后处理、量化或配置变化下的拓扑成立。

### 3.4 已接入的模型矩阵与归一化形状关系

[ModelKernelLayout](../../src/markov/include/markov/adapters/frameworks/sglang/models/model_kernel_layout.hpp) 在模板读取时消费最内层模块 scope、模块参数和 kernel 观测，先核对模块输入为 `[T,H]`，T 等于本次 serving 请求的 input_tokens 总和，再按原生角色核对完整输入/输出槽。相同数字或 kernel 名称不足以建立映射，不沿父区域把子模块节点重新标成父模块的工作。

| 角色 | 输入与模型关系 | 输出 |
| --- | --- | --- |
| QKV 投影 | `[T,H]`、`[(Q+2K)D,H]` | `[T,(Q+2K)D]` |
| attention 输出投影 | `[T,QD]`、`[H,QD]` | `[T,H]` |
| MLP gate/up | `[T,H]`、`[2I_rank,H]` | `[T,2I_rank]` |
| MLP SwiGLU | `[T,2I_rank]` | `[T,I_rank]` |
| MLP down | `[T,I_rank]`、`[H,I_rank]` | `[T,H]` |

当前来源的重复MLP核对：同次调用各层完整计算描述一致；按上述模型维度仅替换激活token轴后，全部观测共用一套计算描述。证据与适用边界见[整体审查](../redesign-workflow/2026-10-10_architecture_review.md)，不据此扩大目标长度域或宣布通信、CPU、资源及外部接口等价。

ModelKernelLayout 现在只作为 SGLang 建库阶段的临时绑定器；关联唯一 logits 输出并完成 sampling 绑定后，将每个节点编译为统一 ModelParameterLayout，原生绑定器不保存在消费模板中。统一对象包含语义角色、输入/输出槽与轴、轴的目标量和乘数、页大小，以及序列最大值、页表、位置或 logits 索引的工作量类型。核心按这些明确关系求值，不识别原生算子名或 dtype 来选择语义。

来源诊断由生产端生成并随参数关系保留，仅用于解释，不用于目标控制或规则选择。两个阶段共用 TensorAxisExtent/TensorParameterAxis 值合同，避免另写一套形状计算；编译不会为未观测字段生成关系，也不将参数关系视为跨分支结构资格。模型和 Attention 的形状求值共用 TensorParameterLayout；KV 等仍有生产布局依赖，尚未完成整体统一交接。

H、Q/K、D、I_rank 来自实际模块属性，权重维度约定依据官方 dense linear 的 `F.linear` 路径；两倍 I_rank 表达原生合并的 gate/up。当前矩阵 kernel 观测为 MatMulV2，激活为 SwiGlu。消费者按关系计算激活输入/输出，权重、dtype、format 和原生 kernel 身份继续保留；TP、模型及后端变化不在这项规则的适用范围内。诊断 model_kernel_layouts 保存源区域、角色、token 来源、固定宽度和动态轴，kernel_parameter_coverage 分别统计模型行关系、attention 关系及两者之外的 kernel。

归一化继续使用同一个 ModelKernelLayout，以 token 行槽和行乘数表达多个操作数，不增加独立归一化变换框架：

| 原生角色 | 输入 | 输出 | 可变行数 |
| --- | --- | --- | --- |
| 层内 RMSNorm | `[T,H]`、`[H]` | `[T,H]`、`[T,1]` | T |
| 层内 AddRmsNorm | 两个 `[T,H]`、`[H]` | `[T,H]`、`[T,1]`、`[T,H]` | T |
| native Q norm | `[TQ,D]`、`[D]` | `[TQ,D]`、`[TQ,1]` | T×Q |
| native K norm | `[TK,D]`、`[D]` | `[TK,D]`、`[TK,1]` | T×K |

层内 H 由原生 hidden_states 的宽度取得，Q/K/D 使用实际模块属性；公式依据原生 `_reshape_for_qk_norm` 的二维展开和 `forward_npu` 的 RMSNorm/AddRmsNorm 调用。完整操作数槽匹配后才建立关系。输入 gamma 的 `[H]` 或 `[D]` 保持固定，原生 reciprocal-RMS 和 fused residual 输出的行轴一并更新，不能因 Python 丢弃输出就遗漏它们。诊断 varying_axes 对每个输入/输出动态轴记录 slot、axis、extent 和 multiplier；extent 区分 input_tokens 与 requests，维度为对应目标数量乘以该操作数的乘数。Q/K norm 的乘数来自模块 query_heads/kv_heads，其他层内归一化为 1，不保留旧的单一行乘数字段。

Q/K head 数相等时，当前最内层 qkv_preparation 范围和形状不能区分两种角色，保持未映射。decode 的 NPU fused QKV 走不同路径，不套用 native Q/K norm；最终模型 norm 的来源关系见“模型最终归一化的维度来源”。此处不插入额外归一化节点，不拆分实际融合工作，不证明两条路径可互换。

这是有来源的形状关系，不是整个目标图的泛化许可证：未映射 kernel、attention/KV、通信、CPU 提交、融合或分解变化仍需各自依据。当前实例选择仍要求已观测 token 条件，结构报告继续 pending_source_and_transform_validation；不能仅凭这些关系把该限制删除。shape 可求值与 kernel/子图结构在目标域成立分别验收。

native QKV 的 Slice 复用 ModelKernelLayout 的单输入/输出行轴关系：已报告输入为 `[T,(Q+2K)D]`、两个长度为 2 的切片参数张量，输出为 `[T,QD]` 或 `[T,KD]`。依据 `forward_prepare_native` 的末维 split 和 `_reshape_for_qk_norm` 的布局转换，仅在该语义区域核对槽位后绑定；不同 scope 的 Slice 不套用规则。只改变激活的 token 行数，两个切片参数张量的形状不变。参数值未观测，关系不认定具体 K/V 身份、不恢复偏移值，也不证明新形状下仍产生相同物理 Slice 节点。

native RoPE 复用同一逐操作数 token 轴机制。原生 `RotaryEmbedding.forward_npu` 向 npu_mrope 传入 positions、二维 Q/K 和 cos/sin cache；当前设备记录将 Q/K 输入展平为 `[TQD]`、`[TKD]`，输出保留 `[T,QD]`、`[T,KD]`。绑定限定 qkv_preparation 的 native 方法、RopeWithSinCosCache 和这组完整观测槽位。positions、展平 Q、展平 K 的乘数分别为 1、QD、KD，两个输出为 1，cache 两维保持原样。该关系不推断 positions 数值、cache 内容或新长度下的 kernel/布局选择；其他 RoPE 后端及 mrope 多轴位置路径不自动套用。

缓存写入前的数据准备也复用 ModelKernelLayout：限定 attention_core 区域，`cache_slot_cast` 仅绑定 `[T]` INT64→INT32 的实际 Cast，依据非 FIA `NPUMHATokenToKVPool.set_kv_buffer` 的 loc 转换；`value_slice` 要求 V 输入为 `[T,KD]`、stride 为 `[(Q+2K)D,1]`，并核对实际 Slice 的 `[T,Q+2K,D]`→`[T,K,D]` 槽位。两者只改变输入/输出第零维，切片参数张量形状保持 `[3]`。不把其他 scope 或 dtype 的 Cast 当作槽位转换，不将连续 V 套用 strided materialization，不复制源物理槽位编号或推断未记录的切片参数值。它们没有 attention 计算工作量字段，仍各自是一份物理工作。

embedding 的 token 行关系依据 `VocabParallelEmbedding.forward` 与 `get_masked_input_and_mask`：逐元素词表边界判断、布尔/整数转换和 offset 运算使用显式一元、向量二元或向量/标量槽位模式，避免 T=1 时把标量当作 token 轴。GatherV2 保持观测权重分片二维形状与 axis 参数张量形状不变，只变更 token ID 槽位和 `[T,H]` 输出；MaskedFill 变更 `[T,H]` 与 `[T,1]` 两个输入及输出，填充值槽保持不变。三类角色复用 TokenAxis，不按 token ID 枚举模板。TP 分片改变、量化路径、越界检查及通信的结构资格另行核对；该关系不推断词表边界值或目标通信。logits 的裁剪和 lm_head 行数需要请求/输出位置语义，不能直接套用 embedding 的 T。

NPU decode 的融合 `split_qkv_rmsnorm_rope_kernel` 复用行槽机制，以实际 Q/K/D 核对已报告输入 `[T,(Q+2K)D]`、两个 `[T,1,1,D]`，以及已报告输出 `[T,QD]`、`[T,KD]`。sin/cos 的 token 轴来自原生 `get_cos_sin_with_position` 的布局定义，不能凭 T=1 时的单例维度猜测。融合工作仍是一个物理节点，首层独有的 Gather/Tile 不复制到所有层。

该 vendor 记录不完整：原生函数返回 Q、K、V，读取 Q/K norm 权重，但记录只有两个输出且遗漏这些权重输入。第二输出无法仅凭形状确定为 K 或 V。诊断明确标记 `partial_vendor_record` 和 `one_of_k_or_v`，不伪造第三个观测输出；`partial_operand_layouts` 是 `model_row_layouts` 的子集，不可重复相加。该关系仅覆盖已报告槽位的行轴，完整操作数合同和新形状下的执行结构仍待核对；轮换请求长度不能修复观测合同缺口。

### 3.5 Attention/KV 的长度语义与缓存布局

[AttentionKernelLayout](../../src/markov/include/markov/adapters/frameworks/sglang/models/attention_kernel_layout.hpp) 处理实际主线的 ND cache write、prefill flash 和 decode paged 三种路径，和负责分配器的 PagedKvLayout 分工。绑定依据最内层 radix_attention 身份、实际 Q/K/V 布局、模块 head 参数、原生 page_size 和完整 kernel 操作数；未匹配的 FIA、SWA、MLA、padding 或其他变体不套用这些关系。

AttentionKernelLayout 只在生产端识别原生路径并核对完整操作数，随后编译为统一 AttentionParameterLayout。模型和 Attention 共用 TensorParameterLayout 的轴求值：生产端声明哪些输入/输出槽的哪一维来自 tokens、requests 或 page_columns；消费端不按原生 operation 名选择槽位。缓存写入仅改变输入 0/1/4 的 token 轴，保留缓存输入和输出形状；prefill/decode 则改变 Q 与输出 token 轴、页表 B/C 轴。

统一 attention 对象保留固定的 operation、head/page/cache 几何和来源诊断，目标序列来自当前 BatchInput，携带自己的请求槽和 KV 页身份。没有将原生物理槽编号带入目标，也没有因通用形状计算而宣称新 launch、attention 后端或拓扑分支可用。TemplateDefinition 不再持有 AttentionKernelLayout；KV、请求池与结构规则仍需继续统一。


令 T=ΣP_i、B 为本批请求数、L_i 为既有 prefix/context，P_i 为本步 query 数，S 为 page_size。原生 init_forward_metadata 从每请求的实际 sequence 长度生成页表，因此页表逻辑形状为 `[B,ceil(max(L_i+P_i)/S)]`。分配的 cache tensor `[cache_pages,S,K,D]` 保持当前固定部署的观测布局，不以请求长度缩小，也不将 padding page 计作有效 context。

| 路径 | 本步可变部分 | 保留的部分 |
| --- | --- | --- |
| cache write | K/V 的 `[T,K,D]` 与 slot_indices 的 `[T]` | 两个完整缓存输入/输出的布局 |
| prefill flash | query/output 的 `[T,QD]` 与页表 | cache、预建 causal mask、空可选槽和部署参数 |
| decode paged | query/output 的 `[T,Q,D]` 与页表；当前每请求 P_i=1 | cache、空可选槽和部署参数 |

目标节点的 attention_workload 保存来自本次 BatchInput 的逐请求 query/prefix/KV 长度，以及本地 Q/K/D、S、cache_pages。对于 attention 节点，另报告每 head 的逻辑因果对数 `Σ(P_i×L_i + P_i×(P_i+1)/2)`；这是工作量特征，不是耗时、实际执行指令数或 padded kernel 的操作数估计。cache write 单列角色，不赋予 attention 对数。由此避免仅凭相同 query/cache shape 就将不同 context 当作相同工作量。

原始 source kernel 仍保留观测内容，派生 attention_workload 只随目标副本及实际执行图输出。图实例中没有复用源缓存槽地址；本项也不生成实际页表内容或执行缓存读写，所需模拟状态沿用控制器的唯一请求/KV 状态。TP、缓存容量、后端变化及新长度下的 launch/依赖结构仍须独立资格。当前精确 token 选择门槛继续有效，不能以三个 shape 公式宣称整图泛化完成。

诊断 attention_kernel_layouts 保留源区域与布局关系；kernel_parameter_coverage 取代旧的单一 model_kernel_layout_coverage 字段，不保留别名，分别列出 model_row_layouts、attention_layouts 与未绑定两类关系的 kernel 数。该统计不覆盖所有独立布局规则或 CPU/通信资格。

当前真实核对已取得 512 个 attention/KV 绑定：cache write 256、prefill flash 128、decode paged 128。目标 prefill 为 query=8/prefix=0/KV=8，decode 为 query=1/prefix=8/KV=9；对应 attention 的每 head 逻辑因果对数分别为 36/9。实际图除新增 attention_workload 外保持相同，条件执行摘要不变。模型行关系与 attention 关系合计 2,560 个 kernel，未绑定这两类关系的观测 kernel 为 1,110 个。证据见 data/redesign/s3-attention-parameters/attention_parameter_audit.json；这仍是开发资产交接核对，不是新 context 或未见请求泛化验收。


### 准入策略与内存压力

安装版 schedule_policy.py、schedule_batch.py、scheduler.py 与当前第三方源码逐字节一致，静态证据见 data/redesign/s3-admission-policy-audit/source_audit.json。采集入口明确使用 ignore_eos=True，关闭 tree cache 时 add_one_req 进入 add_one_req_ignore_eos。因此不能以普通 add_one_req 中的 prompt+max_new 判断代表当前主线。

该路径已编译为统一 AdmissionPolicy，核心使用下面的完成次序预算并保留安装版增量插入顺序；旧 reservation_tokens 已删除。Prefill 输入预算仍在接纳后扣除页对齐输入。尚无带新策略字段的真实资产，公式落地不等于原生内存压力下的组批资格。

Decode 的 check_decode_mem 是另一个判断：非投机路径按 kv_committed_len 是否落在页边界统计下一步新页需求，必要时进入回退。不能用准入预约替代此 CPU 分支的资格说明，也不能因过滤结果相同便复制整个更新前缀。

NewTokenRatioTracker 的 init/min/decay 来自原生环境与 schedule_conservativeness；每次非回退 Decode 更新都会衰减。当前 native_environment 只保存 server args，不能反推出历史 worker 的环境覆盖值，也不能把当前容器默认值填成历史事实。Schedule wrapper 现已从 worker 实际 tracker 记录 initial_ratio、minimum_ratio、decay_per_decode、entry_ratio，并从 worker 模块记录 output_estimate_limit_tokens 和 reserve_tokens_per_request。AscendSourceExtractor 校验数值域后，经 SourceAdmissionPolicy 写入模板 source_state.admission_policy；不存在记录时保持缺失。生产端核对所有 Schedule 的策略参数一致性、每个 rank 按 observed_start_ns 选择的首个入口比例，核心正式消费 AdmissionPolicy；缺记录返回 admission_policy_not_observed，策略变化或初值不符返回实现缺口。后续联合采集已取得真实 worker 记录并完成正向消费，见 s3-admission-work-capture 及执行记录。目标只维护自己的 current 状态，不能复制来源每步的 ratio；测量窗口开始前的 warmup 可能已衰减入口值，初始状态资格不能仅靠 initial_ratio 推定。

原生 batch_is_full 是持续状态：人数/内存拒绝后关闭准入，成员减少或接管新的 Prefill 批次才解除，非空合并保留。共同控制器现以 admission_blocked_ 实现这些转移，复用 BatchFilter 的输入/保留人数，Published 不直接清除；KV 拒绝谓词已换为有效策略预算，原状态转移继续复用。来源写点及正式路径检查见 data/redesign/s4-admission-latch/{source_audit,verification}.json。

比例更新还须包含 on_idle：只有 is_fully_idle 成立才 reset，不能把每个空选择都当作 reset。另需保留 add_one_req_ignore_eos 中 req_states 的实际增量插入行为：当新剩余长度大于所有现有值时，原生循环结束后的 i 指向末元素，insert(i, ...) 并非追加到末尾。因此逐候选重新排序不能直接宣称与安装版等价；这项静态发现尚未量化为真实目标差异。

统一交接与生效时点已明确于[控制合同](03_control_interface.md#准入策略交接与状态生效)，已接入核心但未完成真实正向验收。当前 ignore_eos、冷缓存、非分块、非混合路径的预算关系如下；所有量以 token 计，P 为实际 page_size，E 为实际空闲页数乘 P：

- 准入周期开始，当前占用偏移 C=0，总预算偏移 T=Σ min(output_limit−published, estimate_limit)×current_ratio，求和对象为原生尚未过滤的运行成员。
- 候选输入按页取整得到 Q。先要求 Q≤min(E−C, E−T)。ignore_eos 完成检查使用完整剩余输出 output_limit−published，不乘比例，也不截断；这与 T 的估计是两个不同用途。
- 完成次序检查中，第 i 项剩余输出为 L_i、此前预计释放总量为 F_i、余下请求数为 B_i；要求 E−C−Q+F_i−L_i×B_i > reserve×B_i。每项通过后，F 增加该请求的 prompt+published；这里保留原生未页对齐的释放估计，不将它当成真实页归还。
- 接纳后 C 增加 Q+P，T 增加 Q+min(output_limit, estimate_limit)+P，Prefill 输入预算仅扣 Q。先检查内存预算耗尽，再检查输入预算耗尽；只有内存拒绝/耗尽才置准入关闭状态。
- 下一步 Decode 所需真实新页由当前缓存长度是否整除 P 决定。预算估计不证明有可分配页；无法满足且回退未实现时应报告明确控制缺口，不能从空页队列取元素或凭成本补偿继续。

完成次序列表必须保留上述原生增量插入语义，不能假定每次都已排序。安装版 on_idle 的 is_fully_idle 同时核对运行、等待、上一批及当前批等状态；非 overlap 正常循环先执行 on_idle，再设置 last_batch。核心仅在 Idle 的 Selected 完成且无 Waiting/Active 请求时重置比例；空动作不触发 reset。此映射仍需带空闲间隔的真实执行验证。

关闭条件包括固定资产下容量临界、页对齐、不同完成顺序及空闲间隔的结构核对。已保存的静态证据和同输入保持不是独立目标验收；不通过新增 wrapper、逐案例补采或复制原生对象实现这份合同。

### 联合资产下的 Schedule 前缀变换边界

本节只维护当前合同。历史调试、累计开销和阶段状态见执行记录、实施计划，不以旧 complete 代替当前支持域。

**来源与目标工作。** 控制器在 transition 前记录 BatchTransitionState，在准入前记录 AdmissionEntryState；AdmissionWork 由现有决策过程生成候选、请求预算、horizon 检查次数及停止原因。非空 Prefill 的队列整理量和尾部扫描量由入口及接纳数推导，不额外维护请求状态。来源由生产端根据真实记录编译，缺字段保持未知。

| 原生区域 | 记录与位置 | 生产约束 |
| --- | --- | --- |
| batch_transition | get_next_batch_to_run 中进入准入前的完整前缀 | 保留 pending/running、filter/merge、资源端口与状态效果 |
| admission_prefix | 准入方法开始至 Prepare 前 | 原生候选/请求检查、队列整理和退出事实 |
| admission_budget | add_one_req_ignore_eos，每次调用一个区域 | 在原生循环内累计 horizon_checks；拒绝候选不绑定最终 batch 成员 |
| prefill_statistics | Prepare 后的统计赋值及参数求值 | 记录实际等待/运行数和 chunk 状态，不能混入准入前缀 |
| decode_update_prefix | running batch 更新至 decode Prepare 前 | 一次更新过滤、非空匹配结果资格化普通路径；完整区域替换仍需端口与目标容量合格 |

同方法的 inline 区域按 hook 分组，由 SGLangInlineMethod 一次改写原始 AST 并编译；子类负责参数和切口，BlockWrapper 负责记录生命周期。旧逐区域包装编译路径已删除。SourceNativeRegion 保存原生 result 并随模板交接，核心不直接解释框架退出字符串。

**工作编译。** prefix 的请求检查次数必须等于其直接 budget 子区域数；horizon 取这些区域的实测计数之和。队列整理和 Prepare 后统计分别核对真实入口及接纳数。当前只转换 queue_exhausted、request_capacity，以及关闭/空队列可解释的 early_return。NO_TOKEN/OTHER 存在不同原生成因，未确认前不转换为目标内存或输入预算原因。未资格化返回 admission_work_not_qualified。

**完整 transition 替换。** 生产端按真实父 scope 划分 transition 与 decode_update：存在 pending prefill 时，前缀恰含一次 index=0 的过滤；merge 必须属于前缀并与已编译状态相符。Decode 恰有一次后续 update 过滤；数量或归属不符不授予 transition_region。核心 matches_transition 比较入口成员、前缀过滤和 merge。生产端对已资格化普通前缀声明 transition_latch_parameterized：原生前缀仅保留/清除准入标志，不读取该标志选择执行路径；因此来源与目标的入口标志可以不同，目标效果仍由控制器维护，后续 admission 独立检查目标状态与工作量。该合同替代仅在成员减少时允许旧标志不同的限制，不放宽人数、过滤或合并关系。生产端另为无 merge、非索引过滤且同线程纯 CPU 的前缀授予参数域。参数域保持 pending 的存在性，只允许全清空/全保留；输入人数可作为 CPU 循环工作量变化，成员采用及 latch 赋值由目标控制器维护。固定资产的两 rank 各有 pending=1、running=0、全保留的纯 CPU 来源，见 s4-prefill-publication/transition_count_audit.json；带 merge 的候选包含设备节点，不授予此规则。新人数的实际展开见 s4-prefill-transition/transition_verification.json：第二轮 Schedule 的 pending=2、running=0，两次过滤均保留[0,1]且无merge；两rank实际各18个节点、无kernel/transfer。完整请求输出及物理时序通过，给定耗时下基线8/预算12阶段，预算指标保持。transition 在下一阶段选择之前结束，因此可在相同 Schedule/rank 的 Prefill/Decode 来源间选取该区域，经 plan_region_replacement 纳入 plan_materializations；其他区域仍要求相同阶段，整批四阶段同源保护不变。后续过滤与 transition 分开解释，由独立的 Decode 更新区域合同处理；没有区域资格时保留过滤差异拒绝。四例真实区域经正式规划完成删除/插入与等待重建，见 s4-transition-selection；此证据不替代完整目标 assembly。

**完整 Decode 更新。** 原生 update_running_batch 包含过滤、KV 容量检查、回退或比例衰减及 batch_is_full 更新，不能只删除 filter。原生 retract_decode 无条件再次调用 filter_batch，即使保留最后一个请求；因此完整观测中恰一次 update 过滤且过滤后人数等于非空选中批次，可资格化非回退区域。batch_is_full 在该方法中只被赋值、不参与入口分支，目标控制器按自身成员变化维护它，不要求 donor 的入口标志相等。

核心在非回退、每请求一个 query token、目标新增页不超过可用页的域内，按同阶段/rank 和已资格化更新过滤选择完整 donor，复用现有区域替换器。matches_running_filter 接受精确过滤；来源与目标均全部保留且完整更新区域为同线程纯CPU时，允许人数变化，保持原区域及目标CPU工作参数。部分过滤要求来源和目标都属于非空、非全保留分支；输入人数、保留人数和索引值由目标参数计算。区域内每个设备节点必须有 BatchParameterLayout，求值后 batch_filter 必须等于目标选择；未绑定节点仍拒绝，不将人数变化当作精确匹配。同步驱动复制仍是CPU服务；区域替换后也必须应用其已有传输关系，不能只为Dma节点更新字节数。CPU 工作和已有 host wait 保留，未知节点或关系拒绝，不把索引 [0]/[1] 编译成不同结构变体。来源直接满足该关系时保留原区域，正常参数化仍将 retained_indices 写入每个 Index kernel。设备过滤改为按 Transition/RunningUpdate 语义角色绑定：生产端以直接父区域及其记录的过滤位置确认角色，来源索引只用于核对观测；目标按自身 pending Prefill 是否存在解析对应过滤。删除来源总过滤数与绝对位置绑定，不再按前缀过滤数量拒绝已资格化的设备区域。无明确父区域的 Index 不推断角色；空/全保留与部分过滤仍是不同分支。外部依赖、资源和等待资格不放宽。该实现接入规划不等于实际目标替换已验收，公开证据和剩余缺口见实施计划。

固定资产三请求开发序列在 100/200 ns 下分别完整执行 45/36 阶段，两次运行共 42 个实际 Index kernel 均携带目标 input_count=2、retained_indices=[1]；准入 1 两图保持，未追加采集。证据见 data/redesign/s4-filter-membership/{100,200}/verification.json。此证据覆盖当前固定人数部分过滤，不授予跨过滤序列重定位。

新增独立检查 s4-independent-prefill-members 在两配置、两种耗时下均停于上述跨序列限制。实际 donor source_call 414/5540 的唯一过滤为2→1、保留[0]；目标有两次过滤，最后一次为2→1、保留[1]。BatchParameterLayout 在成员索引求值前要求 sequence_size 相等，导致 matches_running_filter 先返回不匹配，公开缺口为 decode_update_variant_not_observed，而非缺少原生部分过滤观测。当前已按生产端区域归属替换绑定，保留部分过滤形状、非回退、完整更新区域及资源端口资格；s4-filter-role-binding 四组合验证结束，仍停于20/24阶段，拒绝转为 region_new_resource_has_unexposed_work：donor 引入的资源在 recipient 区域外已有工作，当前替换器要求显式插入点。56个已执行Index使用正确目标索引，但跨过滤位置的插入数为零，不能声称重定位通过。下一步需从原生提交顺序及相邻区域证明资源切口，不以时间邻近或删除该拒绝连接工作。原独立失败保留，该输入用于修改后属于开发证据。

**已有资源上的区域插入。** 模板选择仍固定最终请求人数。两请求来源31/1170与5157/6296中，资源工作全在transition，可供后插update；但一个请求的目标不能直接选择这些来源。实际单请求来源414/5540的transition只有CPU，全部worker/设备工作在后续update，且每rank有一次HostWait。两种方向均有实际同线程依赖证明transition先于update，见 s4-filter-role-binding/{resource_predecessor_evidence,resource_successor_evidence}.json。不能用“可用donor”的区域布局代替“实际recipient”的布局。

公共region_resource_anchor要求同父、区域不相交、资源全部外部工作由唯一相邻区域覆盖，并沿同CPU lane的ThreadOrder/IntervalExtent证明在先或在后。在先时以相邻资源出口连接插入入口；在后时以插入出口连接相邻资源入口，并将新设备完成连接到后继区域中拥有明确同资源记录的HostWait。跨模板后续工作、等待和导出继续由TemplateAssembly处理；CPU调用先后只确定提交顺序，不直接使CPU入口等待设备完成。联合变换须保留引用端口；找不到唯一邻接、存在不明工作或顺序继续拒绝。已删除只支持前驱的实现。s4-resource-neighbor-insertion四组合完成核对：公开缺口推进到Publish/publication_member_branch_not_qualified，但四阶段按整批接受，失败批次未进入实际图，仍为20/24阶段；不能宣称资源插入路径已动态验证。来源publication_cohort_audit.json确认单请求完成分支已存在于batch15，下一步核对同源四阶段选择、同循环发布替换与多个区域同时引入资源的组合限制，不把公开Publish缺口直接当作缺观测。

**联合区域引入资源。** 已确认包含单请求完成分支的batch15对应Schedule4206/9332：transition和update的已观测端口均只有CPU。目标需要两个区域分别插入merge与部分过滤，共用worker/设备流；当前apply_operations对重复added_resource_ports直接拒绝，不论区域是否有明确先后。证据 s4-resource-neighbor-insertion/joint_resource_contract_audit.json。当前区域计划保留source_region，apply_operations显式接收所属来源定义，统一重定位后用region_precedes判断同资源插入区域的先后，再连接资源出口/入口及后续插入区域的已记录等待。所有变更先在副本完成再提交；单区域邻接与联合排序复用同一因果判定，不使用rewrite向量顺序或donor时间。缺区域身份、相交或不可排序仍拒绝。s4-joint-resource-order四组合已完整24/28阶段，参数/KV/发布/物理时序无错误。每图14个Index跨过滤序列绑定；两rank实际选择完成分支4206/9332，各插入7个merge与7个Index，merge→Index→Prepare、worker顺序和HostWait路径均通过，见verification_summary.json、resource_path_verification.json。该输入属于开发证据；第五批主回归s4-joint-resource-main已完整61/83阶段，状态/物理检查无错误，指标、阶段和缺口保持；独立结构与性能资格未由此关闭。

当前四批公共修改的六请求回归已通过：s4-resource-neighbor-main两配置complete/61、83，阶段记录、指标和缺口与s4-publication-members-main保持，参数/KV/发布及物理时序检查无错误。此结果不覆盖尚未提交的三请求失败批次。

**Idle 跳过更新。** 原生仅在准入后运行批次非空时调用 update_running_batch。目标前缀已清空立即完成的 Prefill，且 running=0、无新请求、无后续 update 过滤时，可撤销来源中已资格化的纯 CPU 空返回区域。复用 semantic_region/region_ports 证明单 CPU 入口/出口，删除区域节点与内部依赖，仅以 ThreadOrder 连接原端点；区域外服务、Selected 和资源状态不变。目标仍有 update 时保持原区域，反向增加未观测 update 不获资格。s4-idle-without-update/{early-finish,staggered}/verification.json 已核对两配置完整结束，以及 8 处 rank 区域各删除 5 节点、另 8 处各保留 5 节点。

CPU 区域的节点种类、非空性和单线程检查统一由 TemplateDefinition::is_cpu_region 提供，准入与 transition 共用；生产端的语义资格仍分别判断。纯 CPU 检查本身不授予任意分支变换。

**等待与资源。** is_resource_wait_import 要求明确来自本rank更早的serving调用、资源匹配且local completion对应完整stream_wait记录；调用顺序包含同批前序阶段，不再将一批全部阶段赋为同一order。当前支持的non-overlap路径按目标调用顺序重绑定：等待消费此前批次的资源前沿，以及本批更早调用的实际工作完成前沿；不得接入当前调用自身或后续调用的导出。同一调用内部的等待依赖由原子区域变换保留或重建。collect_stream_waits只收集存活/插入等待，Assembly移除对应原生连接的接收端和同批发送端，再统一接边；插入资源不再维护单独的跨调用等待补边分支。并发提交、overlap及未知同步对象仍不由此获得资格。

短初始化来源首发布成员的四条同批等待，在原图中可经Publish父区域同步和同线程CPU路径到达，证据为 `s3-source-composition/publication_wait_alternative_paths.json`。未据此删原始边或做全局传递约简：这种冗余关系不保证在跨来源替换后仍成立，目标等待必须查询实际已提交工作。当前修改及公开验证记录见执行日志“同批前序调用的等待重绑定”；完整人数变化和独立结构验收另行完成。

**输入预算与资源入口的当前验证。** 配置入口允许请求容量和 prefill token 预算变化，目标模板仍逐项校验。固定旧资产与显式给定耗时下，预算目标已完整展开 12 阶段；两次单请求 Prefill 因 input_budget 停止，随后双请求 Decode。设备/CPU worker 入口均连接前一 Prefill Publish，14 个 Schedule kernel 的有序签名、设备因果及 Selected 关系与原生一致。证据集中于 s4-input-budget-expansion。基线的新双请求 Prefill 长度组合仍被拒绝；完整 CPU 分支、复制操作数和独立验收未完成，不宣称任意预算/长度支持。

**Worker 服务分段。** rank 1 原生比目标多一个 ConcatD_Tiling 观测及两个 Dequeue 服务片段；真实观测保留。检查发现片段归属相同、内部边界无提交/等待，extract_worker_cpu 现合并连续且 owner/scope/source_interval 相同的覆盖，再由 observe_cpu_control 保留因果切口。s3-worker-coverage 证明覆盖、原始观测、边界及非 extent 依赖保持；主资产验证位于 s3-worker-main-coverage。新服务节点的给定耗时按旧连续片段求和，不能按旧 span ID 或每片段常量直接迁移；这不是性能拟合或真实 CPU 成本资格。

**有限 CPU 参数域。** 最新真实来源的 30 个准入/预算/统计区域均只有 CPU observation/control。四个单请求前缀在容量停止与队列耗尽下，操作身份（含 aten::empty）、端点别名和内部依赖一致；八个预算区域在不同 horizon 次数下保持同一观测结构。节点归属使用 scope_call 和父链，不能用 serving owner_call 代替。

生产端仅为单 CPU 执行线程、区域内无设备/等待、全部预算调用成功接纳的 prefix/statistics 授予 admission_cpu_request_count。核心 parameterized_cpu_path 对非空接纳限制为队列耗尽或接纳后容量停止，核对候选/请求/horizon 与等待数关系，并要求目标接纳数及预算调用数保持来源值。运行成员遍历、horizon、队列整理和尾部扫描的变化保存在目标工作参数中，不按等待数增加模板。内存/输入预算拒绝、调用数变化及额外原生机制不包含在该域。 预算退出不能仅按外层枚举分类：add_one_req_ignore_eos 可在接纳前返回 NO_TOKEN，也可先 append 并扣减预算，再由 budget_state 返回 NO_TOKEN/OTHER；OTHER 还包括延迟、chunk 等其他原因。后续资格化须记录原生退出位置及当前候选是否已接纳，不重复执行预算判断。最新来源的 32 个 prefix 仅包含 22 次 early_return、8 次容量停止、2 次队列耗尽，12 个 budget 区域只记录 horizon 次数，不能证明预算退出分支。证据 s4-admission-exit-audit/source_inventory.json。 现有预算区域已补充 return_site（原生函数源码内相对行号）、native_result 与 candidate_accepted：返回表达式只执行一次，接纳事实读取 can_run_list 末成员身份，嵌套辅助函数保持原样。该记录是来源诊断，不是核心按行号分支的规则；当前历史资产没有这些字段，7 个返回位置通过 Docker AST 改写/编译检查。随后一次有界真实采集验证了末端 budget_state 返回 OTHER 且 candidate_accepted=true：两 rank 各两次，两个输入分别为 127/65 token，按 128-token 页扣减同一输入预算；下一次入口 batch_is_full 仍为 false。证据 s4-admission-exit-capture/verification.json。尚需完整 CPU 区域/依赖比较与目标展开验收，NO_TOKEN 和其他 OTHER 原因未由此覆盖，提取器限制不变。 最新结构对照中，4 个输入预算停止前缀与4个已支持单候选前缀均为相同的 8 节点、13 内部边、4 切边；尾部统计的4个新区域与6个旧区域也保持节点/内部边/切边关系一致。对照覆盖嵌套预算区域和端点别名；切边仅比较本地端点、关系与外部 lane 类型，尚非完整外部工作身份或目标装配证明。证据 s4-admission-exit-audit/{topology_comparison,statistics_topology_comparison}.json，后续优先扩展公共 CPU 参数域，不增加逐长度模板。

零接纳使用同一个 CPU 参数域：候选、请求和 horizon 检查均为零，按入口状态区分 Closed、NoWaitingRequests 与创建 adder 前的 RequestCapacity。原生容量判断只读取配置及请求槽列表大小，非推测路径不进入 DFlash 延迟；这些早退均无 Prepare/statistics 或设备工作，batch_is_full 的目标变化由控制器维护。生产端仍要求区域仅含同线程 CPU 节点，核心按接纳的 Prefill 人数匹配；Decode 的运行人数不能被当作接纳数。请求预算失败和已进入候选循环的零接纳不在此域。

该规则服务于给定节点耗时下的结构展开。S5 必须消费目标工作参数重新估计 CPU 成本，不能直接复用来源实测 CPU 时延或增加经验倍率。排序等剩余工作及完整独立目标仍需验收；纯 CPU 或观测数量相同本身不是任意控制路径等价的证明。

**关键证据。** data/redesign/s3-admission-work-capture 的联合观测包含两 rank、64 阶段实例、5400 区域；30 条工作记录在 JSONL、source_facts、templates 中一致，来源仍 needs_data。s4-observed-admission-work 验证 16 组来源/目标工作匹配，s4-transition-selection 验证规划接入与同配置物理图保持，s4-admission-region-domain 和 s4-admission-topology 保存 CPU 工作及依赖证据。完整目标和成本结论以实施计划当前状态为准。


CPU 参数化不调用区域替换的 region_ports 资格：statistics 当前含替换接口尚未支持的外部关系（domain_audit.json），但本规则不删除区域，也不改任何节点、端点或外部依赖。生产端直接核对节点类型及起止 CPU lane 一致性，完整目标仍由原 assembly 检查连接。不能因为统计区域可参数化就把它视为可替换，或删除未知外部关系。


**结构验证边界。** s4-admission-cpu-parameters 首次验证目标首个 Prefill 的四阶段，实际图包含 transition 替换与非空准入 CPU 参数化。s4-decode-context-domain 验证有界长度候选；s4-decode-transition-scopes 验证 16 个 Schedule 中 6 次 transition 过滤与 10 次 update 过滤不重叠。这些阶段均保持同配置 32 阶段和目标首批 4 阶段的物理图，不构成完整 Decode 或独立目标验收。

s4-decode-update-region 验证普通更新路径资格及原有公开结果保持；其目标在准入检查处停止，不能作为更新区域已实际替换的证据。最新执行结果与后续缺口只在[实施计划](../redesign-workflow/2026-10-08_redesign_execution_plan.md)维护。所有上述时序仍使用显式服务节点 100 ns 假设，不是实测性能。
