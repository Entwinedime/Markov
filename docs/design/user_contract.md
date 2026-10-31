# 用户输入与正式流程

本文定义用户输入、准备行为和结果合同。完整预测已接通限定的串行 SGLang/Ascend 路径；自动准备及其他保留能力尚未全部恢复。

## 完整预测

```bash
scripts/markov.sh predict --request /path/to/request.json
```

沿用下方共同请求：base.manifest 复用采集，或 base.configuration 加 execution.capture 先采 base；
performance.database 指向已维护的性能库，或用 performance.models 显式选择离线模型；两者都有时 models 优先。
数据库只在准备阶段读取现行选择，并导出小型只读模型；预测容器不打开数据库，不自动训练未选中的原始记录。
可选 performance.sampling_budget 声明整组共享补采的六项累计预算，需要同时提供 database 和 execution.capture。
没有现行模型时可从空库开始；只有实际成本查询发现缺口才补采，维护后重新执行目标，绝不填零遍历分支。
execution.kernel_cache_initial_state 必须为 empty 或 prepared。
empty/prepared 是声明的初始条件，不是自动检测结果。输入准备另按原生工作规格记录新准备数量；
首次 phase 仍覆盖未细分的初次准备，不能保证还原全部编译器或驱动缓存状态。

当前要求相同部署、串行输入、ignore_eos=true、与 base 一致的输出数量和 formal_begin_index。
文本 prompt 需与采集原文相同，由 manifest 声明的配置取得实际 token，再核对源语义；预测不加载 tokenizer。
采集配置没有保留实际编码时不能猜 token，已有 input_ids 仍直接核对源前缀。目标配置覆盖 base 对应字段，name 仅用于结果标识。
支持变更 page_size、max_total_tokens、max_prefill_tokens、enable_hierarchical_cache、hicache_ratio、
hicache_write_policy、hicache_io_backend、hicache_mem_layout，以及下述文件存储设置；其余执行设置保持来源条件。
目前传输组合限 kernel_ascend/page_first_direct，batch=1；新增页几何、回载或其他成本域不满足时明确停止，不从相似目标借值。
设备容量还受 SGLang 的准入预留约束：页对齐后的输入加所需输出之外，再留一页，并满足严格小于池的条件。
否则框架可能缩减输出，甚至无法入场；HTTP 成功也不代表完成了原工作量，采集另核对真实结束原因和固定输出数量。
不满足时目标返回 resource_infeasible、HTTP 为 null，不生成假想的成功结果。显式采集的已知不可能请求在启动模型前拒绝。
当前区域预测不支持关闭 radix cache 的另一套调度分支。

主机池按原生规则计算：先把设备 token 池向下对齐页大小，再以浮点 ratio 相乘并截断 token，
最后取整页数加一；主机池必须大于设备池。当前只支持 hicache_ratio，不支持正的显式 hicache_size。
主机分配不足时按整次申请量淘汰可回收的 LRU 段，不只释放容量差额；被保护或在途的页不能回收。

文件存储通过 hicache_storage_backend="file" 开启，初始为空且不设文件容量上限。
hicache_storage_prefetch_policy 可选 wait_complete、best_effort、timeout。
hicache_storage_backend_extra_config 仅接收内联 JSON 字符串，允许原生 prefetch_threshold、
prefetch_timeout_base、prefetch_timeout_per_ki_token、prefetch_timeout_max 四项；不读外部 @file，其他设置明确拒绝。
文件 worker 按独立正常采样估计，发起与准入检查分开。首轮准备前的 source 控制残余在准入前支付一次，
预取可在其间推进；这不是源控制已完成正常校正或 target 查询成本的独立预测。
best_effort 在首次准入检查时停止，timeout 从发起计时，截止到达不直接接受缓存页。
后续检查按独立测得的空闲调度回合推进，报告 `prefetch.poll_rounds`；缺这项成本时由整组共享补采，未允许采集则明确缺成本。
查询与接受外围仍有 source 残余近似，不能据此认证这两个策略的精度或排名。

结果写入 execution.output/prediction/report.json。完整正式 HTTP 按首个正式请求开始到最后响应结束，
保留 gap、未归属控制、Prefill/Decode；后台排空另报。每个目标给出状态、逐请求端口、成本缺口、覆盖、图规模与仿真时间。
串行请求间的客户端准备沿用 base 观察，位于两条请求之外；它计入整个正式窗口，但不计入单条请求 HTTP。
报告另列共享来源准备、读取记录、按需细图状态、模型来源和测量限制。缺成本为 null，完成状态不等于精度通过。
cache_capacity_pages 按参与进程报告 device/host 页容量；文件目标另报 host_evicted_pages，是实际回收页数而非淘汰次数。
逐请求 prefetch 区分申请、查询命中、发布和最终接受页数，storage 汇总实际读写工作，不能只用接受页数代表 I/O 量。
storage 中的 rank_read_pages、rank_copied_pages、rank_rejected_copies、rank_new_pages、rank_existing_pages
累计所有 rank 的文件 worker 工作量；请求级页数仍表示逻辑 KV 页。例如 TP2 的同一逻辑页读两份，计为两个 rank_read_pages。
page-work 的 rank_disk_evictions 同样累计各 rank，peak_rank_stored_pages 则是单 rank 文件驻留的峰值。
issued_ns、first_check_ns、accepted_ns、worker_finished_ns 分别报告同一仿真时钟的发起、首次检查、接受与后台返回；
相应操作未发生时为 null。后台读完并不自动插入缓存，接受仍依赖请求端检查及已发布的前缀。
cost_requirements 将整组中相同的成本缺口归并，并列出受影响目标；每次只覆盖首个无法提交的片段。
自动准备按实际工作及来源上下文构造独立小负载，多目标共享；目前支持区域响应、批量备份、整批回载与 buffered 文件 worker。
文件配方不加载模型权重，不运行 HTTP 请求；在采集输出下临时构造真实 K/V 页，读写核对后删除页文件，只保留轻量计时。
这是暖文件缓存下的 buffered 响应，不保证冷介质、fsync 或争用时的性能。
同一缺口补采后仍未解决时停止，预算耗尽或采集失败保留原因及缺失结果，不无限启动模型。
preparation 中单列 base/performance 时间、sampling_rounds 和累计状态路径；这些不计入预测的 HTTP。

预测容器无设备、无网络，仅挂载可执行文件、所选 base 采集目录、模型、准备输入与结果目录。
不挂载项目/data 根目录或 evaluator 的正常测量。输出须与 base/模型输入分离；已有活跃容器时拒绝复用其输出。
一次进程共享 source 和只读模型，当前 targets 顺序执行，jobs 尚未用于预测并发。
execution.node_scale 可显式声明 [{"name": "prepare.prefill", "factor": 0.5}]；默认空列表，按声明顺序取首个子串匹配。
匹配标签为操作、实现和计时口径的组合，只缩放已绑定的工作响应，不修改来源、模型系数或残余等待。
区域响应只支持区域名字，不能把内部某个算子的倍率套到整个区域；更细的名字需要有对应的原生工作。
每目标报告规则及实际命中数；0 次命中表示本次工作没有该标签，不能当作所指算子已经缩放。
这是用户给定的敏感性假设，允许零及正的有限倍率，不授予跨硬件预测资格。
从 configuration 首次采集后，同一输出的重复 predict 会核对部署、运行参数和 workload 后自动复用该任务的 manifest，
不再要求用户手改输入。输入改变时要求新输出或显式 recapture；不会扫描其他目录猜来源。
镜像和设备用于实际采集；复用来源以该次采集记录为准，需要新运行环境的来源时显式 recapture。
同一输出有进程锁，活跃预测容器会在改写输入前被识别。
中断后的采集先核对原容器：仍在运行就等待，正常退出则记账后复用，句柄缺失记失败及保守用量。
容器自身也有时间上限；只有退出和用量写入状态后才移除容器，不凭产物存在推断成功。

## 独立 target 评价

运行 evaluate --request 时提供 prediction、target（报告中的条件名）、manifest（真实 target profile）、output，
可选 memory_limit_gib。示例为 [target_evaluation.json](../../configs/target_evaluation.json)。
预测必须启用 execution.record_graph；评价器核对部署、完整编码 workload、正式窗口及目标设置，不能把其他条件的 trace 当作真值。
SGLang 的 target 采集使用 capture --record-regions，同时记录通常的因果观察与实验区域计时；
KTransformers 的同步阶段默认已有对应观察。该开关不改变普通采集默认值，也不创建可导入性能库的独立校准元数据。
启用 file 存储时，SGLang 还记录原生文件批次的分段和完整 token 祖先关系，区分新键写入、已有键处理及实际页复制/发布。
评价只替换工作、rank、顺序和数据内容都对应的成本；没有对应或分段采集失败时保留原预测并报告欠项。

evaluate 使用独立 markov_evaluate 可执行文件，只挂载明确选择的预测、base、target 与自身输出，没有设备、网络或数据库。
评价输出必须在预测和采集目录之外，避免后续预测挂载到评分数据。
结果分别给出原成本回放、真实成本替换数、保留成本及未对应观察、选定 CPU 顺序的检查结果。
fully_replaced 只表示已有图中可替换的节点均已绑定真实数值，不表示图包含全部必要操作；
status=completed 仅表示评价完成。正常精度仍须使用 measure；profiled HTTP 及客户端跨请求间隔单列，未知成本不删除或置零。

## KTransformers 采集与独立数据

KTransformers 与 SGLang 使用相同的 capture、measure、calibrate、maintain 和 replay 入口及累计预算。
当前 KTransformers 真实范围为单 NPU、dense safetensors、原生 KTransformersLinear/KLinearTorch、同步 eager 阶段，
已验证 Llama-2-7B/float16 来源。没有 HiCache，每请求从空 KV 开始；MoE、量化、多卡及异步服务未取得资格。

配置示例为 [ktransformers_capture.json](../../configs/ktransformers_capture.json)。base.configuration 声明 max_context_tokens，
可选 cpu_threads、attention_backend（eager/sdpa）和 prefill_chunk_tokens；未声明 chunk 则整段 Prefill。
模型位置、dtype 和 TP 属于 deployment；当前仅支持 float16/bfloat16、TP=1。成本覆盖按实际 backend、线程及工作点分别判断。
采集明确生成原始 trace、轻量阶段观察和 HTTP 报告；正常测量关闭 profiler 与观察记录，独立校准只保留小型阶段计时。
predict 使用带完整 generate 边界的 base，按同一固定 token workload 生成 target 的 Prefill/Decode；
分块会产生新的合法阶段，不由 base 中出现过的模板决定。max_context_tokens 不足时返回 resource_infeasible。
当前要求 kernel_cache_initial_state=empty；prepared 的预初始化流程未取得资格，不静默忽略该声明。
执行成功不表示所有后端设置、完整结构或正常性能精度已验收。

独立阶段数据可经 maintain 的 ktransformers_phases 适配器导入既有 SQLite，显式选择来源再发布模型。
成本查询使用 inference.prefill/decode、synchronous_phase_interval 及相应实现，测点和域外缺口沿用共同模型合同。
生成控制使用 inference.request_control、exclusive_request_interval，完整生成区间扣除各计算阶段后计量。
普通预测也可只声明 performance.database 和 sampling_budget；缺口触发整组共享的轻量采样，再经共同维护入口发布模型。
模型只来自独立校准，不读 base 的 profiler 时长或正常评价；改变工作域时不能借相似后端的响应填零。

## 输入预览和采集检查

```bash
scripts/markov.sh plan --request configs/prediction.json
docker build -t markov-modeling:ubuntu24.04 -f docker/modeling/Dockerfile .
scripts/build.sh Release
scripts/run.sh modeling -- build/modeling/Release/markov_core inspect-manifest /path/to/profile_manifest.json
```

plan 读取请求、核对 workload/manifest/模型包的声明路径并列出已知准备动作，不构图、启动设备或创建结果目录。
configuration 的准备动作是采集或复用，预览不判断该任务是否可以复用，也不将数据库路径存在等同于模型已维护。
输出 not_evaluated 列出尚未分析的源语义、目标工作和成本覆盖，不能据此认定模型可用。
manifest 检查只读取 manifest 和文件元数据；输出 inputs_accessible 不代表通道内容、源 DAG 或正常性能已通过。

请求路径以声明它的文件目录为起点。base 二选一：manifest 复用真实来源，configuration 声明需要采集的配置。
每个 target 只有 name 和 configuration；name 用于定位结果，不作为结构或成本特征。
deployment 包含 platform、framework、runtime、model、dtype、tensor_parallel；工作配置由相应适配器解释。
performance.models 和 performance.database 均可省略作输入预览；实际预测需要其中之一。
execution 必须给 output，jobs 默认为 1，memory_limit_gib 可显式声明。
示例在 configs 中；模型位置、镜像、设备和预算须按实际部署修改，不含评分文件或实验身份。

采集文件可以相对 manifest 所在目录声明；完整采集移动后，原 run_dir 内的绝对引用按同一采集的相对位置解析。
这不扫描目录、猜测相似文件或自动兼容历史字段。采集外的独立绝对来源仍使用其明确路径。
manifest 声明 config_path 时，同样解析并检查该 base 配置文件；来源准备只在框架适配器读取所需设置一次，配置文件也受结果覆盖保护。
当前 SGLang 输入准备据其原生 server.command 判断确定性采样，开启时生成种子上传。缺少配置依据或命令由 shell 包装时不把未知设置当作关闭，也不为该准备序列授予完整规则资格。
同次批次观察明确记录该设置时，输入准备使用实际运行值；缺少观察时仍只采用可解释的原生启动参数。

### SGLang/Ascend base 采集

```bash
scripts/markov.sh capture --request /path/to/request.json
```

使用同一个预测请求：deployment.model 改为本地模型目录的绝对路径，base.configuration 使用原生 SGLang ServerArgs 字段，
例如示例中的 page_size、max_total_tokens、hicache_ratio 和 hicache_write_policy。模型、dtype、TP 来自 deployment，
采集器拥有容器内端口及设备逻辑编号，不能在 configuration 重复声明这些项。当前采集使用 SGLang/Ascend eager、FIA 注意力与 Triton 任务队列；
对应原生环境随调用输入和 base 配置保存。非 FIA 来源仍可构图，但现有注意力变换与独立成本不授予其支持资格；其他采集组合尚未恢复。

execution.capture 另声明推理镜像 image、独占设备数组 devices、容器 memory_limit_gib/cpus，以及 budget。
镜像需已安装本项目及相容的框架、PyTorch/CANN 和模型依赖；下方构建命令在已有 SDK 环境中安装项目并构建原生观察库，需有 C++23 工具链、CMake 和 CANN 头文件。
预算六项均为正整数：starts、wall_sec、requests、input_tokens、output_tokens、retained_mib。它们分别约束累计启动、墙钟秒、
请求、输入/输出 tokens 和当前保留 MiB；失败消耗不清零，输出失败未取得实际 token 数时按请求上限保守计入。
采集前检查声明设备空闲，并与同一入口的其他采集任务串行；只挂载所选设备、只读模型与驱动、本任务输出，容器无网络。

workload 当前支持 serial 到达，每个请求提供 prompt 或非空 input_ids 二选一，以及 max_new_tokens；
可选 ignore_eos、return_logprob 是布尔值。formal_begin_index 之前的请求用于准备，仍连续采集；其后为正式请求。
文本先用本地 tokenizer 编码并核对预算，再启动模型。预览列出请求数与输出上限；文本未经编码时输入 token 总量为未知。
客户端时间覆盖实际 HTTP 发送到响应读取，不包括本地 JSON 编解码、写报告或 profiler 导出。

成功返回本次 profile_manifest.json。其文件来自同一次运行：原始 trace、轻量观察与客户端报告，另保留 base 配置和时钟锚点。
探针记录实际接收与最终结果发送端口；导出前检查每个请求恰有一对端口及必要的输入准备观察。
profiling_ready 只授予这些声明通道的导出资格，不证明全部语义、设备依赖、正常性能或 E2E 预测正确。
采集期间另观察原生事件记录、流等待、事件/流同步，保留实际句柄和返回码，借 CANN 连接编号关联设备命令；
事件等待由目标生产者依赖产生，不迁移 source 阻塞时长。原生库只在推理 SDK 构建，纯 modeling 不加载它；采集窗口外关闭记录。

输出位于 execution.output/base，包含累计状态及每次 captures 子目录。已成功的任务默认拒绝重采；
将返回的 manifest 显式填入 base.manifest 可复用，predict 也可自动复用相同 configuration 任务；
capture 加 --recapture 重新采集并保留已有额度消耗和文件。
失败不会把上一份 manifest 当成本次成功返回；中断后同一入口自动接管本任务容器并保留累计用量。
采集入口不训练成本；完整 CPU 校正仍未恢复，独立数据由准备与维护流程负责。

### 正常测量与小型区域校准

```bash
scripts/markov.sh measure --request /path/to/normal_request.json
scripts/markov.sh calibrate --request /path/to/calibration_request.json
```

沿用采集请求中的 deployment、base.configuration、workload 及 execution.capture；两者不接受 base.manifest 代替运行配置。
measure 将 profiler、Markov Python 与原生观察全部关闭，输出 normal/normal_observation.json 及独立 workload_report。
该时间可以供后续评价读取，预测与训练不读取它。当前入口只运行声明的单个配置，并非已经实现整组 target 的评价编排。

calibrate 运行独立选择的小型 workload，安装输入准备、分配规格、eager batch、结果处理/发送包络和异步 NPU event。
它不安装底层算子探针、不采 snapshot、不启 profiler，输出 calibration/region_measurements.json 及轻量 JSONL。
线程 CPU、caller 返回与 current-stream 区间分开记录；查询已完成 event 不新增前向同步。
结果模型区分 HiRadixCache 与 RadixCache，不能用未启 HiCache 的结果 CPU 响应冒充启用分支。
结果发送前与发送后的线程时间由同一观察划分，不算两次独立实验。
warmup 依据 formal_begin_index 排除，请求身份仅用于关联与预热划分，不进入回归特征。
默认维护入口以 sglang_regions 导入，显式选 training 后导出模型；不会自动训练或根据 target 误差补采。

三种运行分别保留 base、normal、calibration 子目录及累计状态；失败与 recapture 均不重置所属预算。
校准仍需显式累计运行及存储预算，不能将额外性能采样转记到 base 或评价。
显式 calibrate 由维护者选择独立 workload 和训练来源；普通 predict 可按整组需求自动构造、采集并维护上述有限工作。

### SGLang 输入分支观察

已有兼容的 SGLang 推理环境可安装输入探针；SGLANG_RUNTIME_IMAGE 指向该环境，不是纯 modeling 镜像：

```bash
docker build --build-arg SGLANG_RUNTIME_IMAGE="$SGLANG_RUNTIME_IMAGE" \
  -t markov-sglang:ubuntu22.04 -f docker/sglang/Dockerfile .
```

在推理进程中设置 MARKOV_INPUT_FACTS，指向预先创建的本次输出目录。框架通过自身插件入口安装观察，
各 worker 输出 input_facts_<pid>.jsonl；未设置时不开启，也不因加载 Markov 插件导入推理框架。
观察覆盖 ScheduleBatch 准备、NPU 分配、页表写入、输入 buffer、注意力元数据，以及实际消息接收和最终发送。
计算批次另与结果异步拷回、CPU 结果处理及最终发送关联，保留处理前后的输出数量与结束状态；
提前执行但未被消费的结果仍属于实际工作，不能据此增加生成 token 数，也不能默认让最终响应等待它完成。
这些观察提供执行边界和主机元数据，不额外将父级区间重复计为 CPU 服务。
缓存前缀更新、提交及页释放另保留同 caller 的处理归属、线程 CPU 时间和主机规模信息；匹配另记录实际索引段数与元素总数，不读回设备数据。包络可能嵌套，不能相加计费；已解释调用进入目标执行，其余控制仍保留缺口。
计算观察按真实原生调用记录线性层、归一化、激活、embedding、reshape/RoPE、注意力、logits/采样及通信的形状、类型和实际执行分支。
这些信息供既有工作规则消费；没有受支持的规则或必要事实时明确保留缺口，不从函数包络猜测设备成本。

记录请求标识、输入/查询规模、logprob、额外 embeddings、位置覆盖、multimodal、Mamba、重算及采样分支的标量状态，
Prefill 另记录主机端 token 前缀以建立跨请求复用关系；张量只取 shape/dtype/layout，不读回设备内容或保存 snapshot。
缓存查询记录原生 key 的实际 token 上限，结束时记录有效 KV 的未按 base 页大小截齐的前缀；目标可以据此重新分页，
不将 base 的命中长度、页对齐结果或缓存快照当作目标状态。
惩罚器状态取自准备完成的调度批次；重叠执行的 forward 副本会移除该对象，不能在那里读取。
方法的墙钟与线程 CPU 时间只用于观察和定位，不直接作为正常成本。
活动 profiler 内每个 worker 另读取少量原生计数器/系统时间配对；manifest 记录计数器前后夹住同一次系统时间读取的区间与换算不确定性，
按实际 rank 关联 trace。它用于统一时钟，不是 CPU 成本校正；profiler 未初始化时不生成伪造锚点。

同一次完整 base 采集的 sidecar.python_probe_files 可声明 `{"path":"input_facts_123.jsonl","format":"jsonl"}`。
trace.ld_preload_trace_files 同样可以显式声明 jsonl，记录原生 API；失败 API 保留原始结果，不作为成功同步依据。
未声明 format 的既有记录仍按 Chrome 容器读取；不按文件名猜格式，未完成的 JSON 行会报错。
结构解释按请求、执行阶段、CPU 资源及原始时间区间关联，在所有通道读完后统一处理，不依赖 manifest 文件顺序。
只有已证明的普通批次入口才取消不相关的 token 长度条件；上传字节、张量分配和设备工作仍使用实际规模。
缺事实保留限制，已知未支持的分支不授予规则资格。其他运行取得的事实不能补写进旧 base，独立分支观察也不等于完整 DAG 或 E2E 验证。

## 用户提供什么

| 输入 | 含义 |
| --- | --- |
| platform/framework | 实际硬件、资源环境与 SGLang/KTransformers；不是实验别名 |
| model | 模型引用、dtype、TP 及影响实现的运行设置 |
| base | 一份真实可启动的配置，或已有明确 manifest |
| workload | 请求顺序、输入、输出 token 上限、到达方式与正式计时边界 |
| targets | 目标配置集合；只包含配置，不含测量、成本或 target trace |
| performance | 已有适用成本包，或独立数据库与训练声明 |
| execution | 输出位置、设备/容器声明、允许的采集预算和预测并行资源上限 |

配置只在入口解析一次。内部使用明确对象；已有 manifest 是复用方式，不是用户第一次使用的前置手工任务。
首次用户可以声明模型、base 和 workload，由准备流程安排受控采集及必要的 CPU 开销校正。
已有数据满足条件时直接使用，不为每组目标重建性能数据库。

### 来源与页几何诊断

```bash
MARKOV_MODELING_MEMORY=8g scripts/run.sh modeling -- \
  build/modeling/Release/markov_core page-work /path/to/page_work_request.json /path/to/page_work_report.json
```

诊断请求包含 manifest、page_sizes_tokens 及可选 models，例如 `{"manifest":"capture/profile_manifest.json","page_sizes_tokens":[64,128],"models":"performance/models.json","kernel_cache_initial_state":"empty"}`。
manifest 与 models 的相对路径以请求文件所在目录为起点；页大小必须为正整数。这项诊断读取真实来源，共享一次准备后生成各页几何。
同一 token 前缀在这次来源准备中只有一个身份，不能将该数字当作跨采集的地址。

报告保留准备请求和正式请求，区分查询输入、完整 prompt 与实际提交路径；查询输入长度不是命中长度。
可复用上限与最少 Prefill token 只描述页对齐的边界，实际值还取决于容量、缓存所在层及发布时机。
prior_committed_prefix_tokens 给出此前提交过的最长匹配前缀；它假设没有淘汰，不能当作实际命中。
radix_segments 按目标页大小表示压缩前缀树，匹配时会在共同前缀处分裂。报告同时保留客户端屏障的位置及控制含义，
不将源屏障的等待时间写入目标工作；观察用 checkpoint 不参与目标调度。
corpus_device_pages 统计完整保留此前前缀所需的逻辑页，不是目标硬件的峰值分配量；请求临时页与在途 I/O 不在此假设内。
projected_compute_work 只列已解释计算原语的工作量及次数，假设 query 没有 padding；缺绑定信息单独报告。
其中 source_work_unchanged 统计完整工作描述与源一致的实例，source_service_bound 进一步要求存在无歧义的源设备服务；两者都是迁移候选，不是已完成成本绑定。
source_compute_projection 用源变量核对这些原语是否保持原工作描述，不评价成本或完整 DAG。
源报告的 work_observations 计一次原生计算 probe 调用，work_descriptions 中每项的 operations 列出该配方展开的操作；操作数与调用数分别统计。
projected_compute_dependencies 只计配方内部的数据依赖，尚不包含调用之间、跨层及 I/O 发布依赖，不能当作完整目标图的边数。
提供 models 时，cost_execution 将已解释计算子集绑定独立成本并交给统一执行器，保留配方内部依赖和已证明的源设备队列顺序。
缺工作变量、资源依据或成本时停止该目标的数值执行，其他独立目标继续；已生成的合法工作不因缺模型删除；尚未触发的动态分支不算已构建。
成本缺口在 issue.gaps 汇总，scope 为 first_incomplete_mutation；相同工作点只出现一次。这只是首个无法提交片段的需求，不是整条目标路径的完整补采清单。
这一子集已描述已解释的原生调用、注意力准备、分配、输入准备和阻塞通信，但尚缺其余 CPU 和完整跨流依赖，不给出完整 E2E 耗时，也不验证目标 DAG 相似度或性能。
同次结果观察表明使用重叠调度时，下一帧可在前一帧的已建模调用提交后准备；各队列保持先后关系，同步读回仍等待相关前沿。
串行 int64 输出按实际规则生成长度更新、长度/token 缓冲区写入、异步 D2H 和完成标记；索引写入保留广播准备与更新两个设备任务。
同工同资源的成本可沿用源片段；其他输出类型、额外返回值和不同初始化状态尚未验证，正常 CPU 与 CANN 首次加载成本仍待校正。
cache_execution.native_call_execution.frames 给出进入、caller 提交、结果可读和已建模工作汇合时间；result_ready_ns 来自异步拷回的完成标记，不等待无关尾部。
工作汇合包括 caller、worker 与设备，不是单独的设备完成。
同一 frames 报告另列 modeled_result_callers_started_ns、modeled_result_callers_completed_ns 和 modeled_send_ns，分别表示已建模结果整理的开始、完成及发送端口。
modeled_result_work_completed_ns 单列结果处理的原生工作汇合；CPU 返回不强制等待未同步的设备尾部，后台程序仍须保留它。
结果序列包含纯 CPU 整理、空/非空释放、初始索引复制、插入索引克隆、匹配/页表回写和备份响应；请求查询另生成分裂复制与索引拼接，查询外围与其余请求控制仍缺。插入、匹配、回写和释放工作量来自执行时的目标缓存状态；序列等待自己的输出和所需的下一帧提交，后续调度再等待上一轮 CPU 整理完成，发送后的片段仍执行。
Prefill 内结束请求的重复页和尾部也立即释放，Decode 的重复页及尾部索引先收集再分组释放；当前只支持未排序页池。请求报告另给出 released_device_pages 与 device_pages_released_ns，空间归还等待全部参与者的已建模页池更新。
准入只预留预算，cache_execution 在每轮进入输入准备时才推进实际分配；其 native_call_execution.frames 中 allocator_view_offset_pages 给出该轮分配前的索引数组偏移。未发生归还时偏移会保留到下一请求，不能每轮重置为零。已验证范围仍为串行、同步 TP 逻辑请求，未覆盖任意并发调度或 allocator 分支。
未结束的 Prefill 在各参与者输出可读后发布页对齐的 prompt 前缀；重叠调度另等待下一帧 caller 提交，写策略据此决定是否提前备份。
请求报告中的 prefill_prefix_published_ns 给出该状态发布端口，预留 buffer 只消耗已提交页，剩余空间留给后续生成；最终提交保留生成部分。
有控制观察时，插入在序列中的实际位置生成，发布汇合各参与者索引工作与必要 KV 输出，可以早于整轮整理结束；缺少观察时保留原组件完成屏障。
这里仍未支付完整缓存控制 CPU 成本，不能当作完整处理时间或 HTTP 完成。新克隆尺寸没有合格成本时按缺口停止，不沿用忽略该操作时的组件耗时。
当前目标沿用来源的调度模式；缺少该事实时组件保留完成屏障，不授予完整执行资格，也不支持变更调度模式的预测。
source_cpu_gaps 另计帧内相邻调用间的残余近似数量；它们有独立覆盖类别，不归入派发延迟或设备服务。
可选 source_cpu_costs 为这些区间提供与 manifest 绑定的校正值，未覆盖部分仍保留原值；报告分别列覆盖、未校正量和估计值。
近似假设同 caller 配方下间隔保持，不能推广为任意配置的 CPU 精度。来源未覆盖、含未建模原生提交或尚未表达依赖的已知等待时报告带原始坐标的缺口，不补零。
模型按完整工作点共享响应，报告分别统计查询次数、实际求值次数和已绑定操作覆盖；复用响应不减少覆盖计数。
当前来源 manifest 尚不能建立完整模型/平台身份，source_deployment_qualified 会如实报告，不能把框架和几何匹配当作部署资格。
源回放使用观测成本，不能当作目标预测精度。

可选 cache 对象包含 device_pages、host_pages、write_policy、io_backend、host_layout；需要 SGLang 来源和已选 models。
容量为每个参与进程的正整数页数，策略支持 write_back、write_through、write_through_selective；当前传输仅支持 kernel_ascend/page_first_direct。
页大小仍由 page_sizes_tokens 逐项给出。原 requests 和 projected_compute_work 保留完整前缀语料的上限统计，
另列的 cache_execution 根据有限两级缓存状态运行备份、回载及计算子集，报告实际复用、回载、Prefill 工作和容量峰值。
回载等全部参与进程完成整批跨层 DMA 后才发布；写回式淘汰等待所选备份批次，传输前保护来源，完成确认后释放引用。
cache_execution.copy_ports 逐逻辑复制列出页数、各参与者的 cache_control_returned_ns、completion_observed_ns 与 published_ns。控制返回包含后续主机索引克隆；发布等待两个端口的全部参与者。completion_basis 区分 modeled_observation_upper_interval 和 device_service，前者包含同步观察开销，不能当作精确设备完成。写回淘汰也使用这组响应并在复制汇合前保留 caller 调用权；淘汰选择、ACK、同步就绪服务和页池归还的外围 CPU 尚未覆盖。缺少结果观察的旧非淘汰备份仍使用设备传输口径。
不同参与进程的计算用显式汇合边界衔接，串行请求依赖前一响应完成；实验响应端口仍只覆盖部分路径，入口和尾部沿用同请求/前端的源残余近似。
partial_request_ports 区分 start_ns、arrived_ns、received_ns 与响应开始/完成；received_ns 为首次取得参与 caller 的准入端口，准入等待不计作 CPU 服务。
有同次最终发送观察时，响应从 modeled_send_ns 接出；缺少该事实则保留全部所选工作完成的屏障。
后台计算、结果整理和 I/O 可在响应后继续，请求的缓冲区和缓存引用由后台生命周期归还；显式清空屏障另等其实际完成。
kv_published_ns 与 modeled_work_completed_ns 分别列出最终 KV 可读及该请求全部已建模工作完成；deferred_kv_pages 记录最终提交时还需等待输出的新增页数。
这类页暂等待全部所选输出，不能当作逐层发布的精确边界；不迁移源绝对时间、设备等待或诊断 checkpoint 的耗时。
这个组件假设每个参与进程有独立的回载和备份队列，未包含其余 CPU 和完整跨流依赖；计数与顺利执行不代表完整 DAG 或 HTTP 精度。
当前缓存控制要求未分块 Prefill，并保持结束请求的前缀插入；来源明确走其他路径时报告不支持。独立的 cost_execution 只运行计算子集，排除需要缓存状态的结果处理，不能与 cache_execution 的请求链耗时混用。

数值执行须声明目标的 kernel_cache_initial_state。empty 表示相关内存与磁盘缓存均空；prepared 表示本次所需规格已经全部准备，不应将部分磁盘命中填成 prepared。
每个目标单独维护规格准备状态；首次使用新规格时加入编译和加载，复用规格不重复加入。每种启动器接口分别首次加载，共享运行环境只在第一个接口的对应观察中计入。
报告中的 planned_specializations、planned_first_loads 和 planned_runtime_initializations 都是计划数量，未完成目标不能据此认定真实执行完成。
未声明初始状态、分配输入缺失或工作不在已支持的签名/分支内时，目标明确停止；不能把该部分当作零成本。当前仅覆盖未排序 NPU 页池的融合 Prefill 分配及单 token Decode 分配。

cache 内可增加 storage 对象，声明 backend 为 file、initial_state 为 empty，以及 prefetch_stop_policy。
capacity_pages 可为正整数或 null，分别表示每参与进程的文件页容量或不设该上限；预测不读取真实文件目录。
这里是组件诊断的逻辑 LRU 上限，不等同于原生文件后端的容量/水位淘汰规则；完整预测暂只允许无上限的文件存储。
prefetch_stop_policy 支持 wait_complete、best_effort、timeout；prefetch_threshold_tokens 可省略，使用当前原生默认值。
prefetch_timeout 可声明 base_sec、per_ki_token_sec、max_sec，按原生线性预算计算，从操作入队开始计时。
报告分别列出查询、工作进度、控制端接受、新增主机页、被拒绝发布的复制和最终 worker 状态，不能把它们都叫作命中。
当前只覆盖对称参与进程下的 I/O 与已有计算子集。best_effort 在首次准入检查时停止；timeout 在下一调度检查中使用已经到达的 deadline。
等待回合通过独立响应推进，查询及准入外围 CPU 仍有缺项；不能拿这些运行结果当作完整 HTTP 精度。

缓存组件报告另列 query_index_copies 和 query_operations，分别表示目标查询产生的索引复制描述数和成功提交的原生操作数。
它们不包含查询外围的全部 Python/CPU 工作，也不代表 KV 内容传输或完整请求覆盖。旧来源缺少匹配段数时，合法目标工作仍会生成，但不能授权对应源成本迁移；查询缺口与之后的插入缺口分别按实际遇到的位置报告。

## 统一任务入口的职责

用户入口将结构化请求交给正式 Python 包，不要求手动串联内部 bundle、CPU 导出、模型训练和 C++ 二进制。
预测、采集、性能维护、独立评价与搜索复用来源合同，不维护同义字段或隐式猜测格式。
每个实际交付入口同时提供最小有效配置示例，不发布无法运行的示例命令。

已准备输入直接进入预测；输入缺失时，准备阶段先汇总缺口和可执行动作。
未显式允许采集时不启动设备。预览只解析、检查已有来源和输出准备计划，
不构图、不仿真、不训练、不清理数据、不启动容器或模型。
失败恢复复用本任务已完成的合法采集，保留累计失败和预算；当前代码应重新生成的派生结果不能仅凭文件存在视为有效。

## 独立性能维护怎样使用

维护者声明部署和有界采样范围，流程负责采集、开销处理、去重入库及离线训练。
目标组使用共享的适用模型，不为每个 target 建一个专用包，也不要求应用用户逐节点选模型。
缺数据时按操作与工作变量归并需要，先查已有库、再判断可外推范围，仍不足才形成最小共享采集建议。
结构或变量未知时先报告事实缺口，不能靠重复采样把它变成成本缺口。
已授权执行的补采必须有累计预算和停止条件；不建立无界的“预测失败—补点—重跑”循环。

### 已实现的导入与离线训练

```bash
scripts/markov.sh maintain --request /path/to/maintenance.json
```

维护请求例如：

```json
{
  "deployment": {
    "platform": "Ascend 910B", "framework": "sglang", "runtime": "ascend",
    "model": "Qwen/Qwen3-32B", "dtype": "bfloat16", "tensor_parallel": 2
  },
  "database": "performance/measurements.sqlite",
  "imports": [{"adapter": "sglang_cache_io", "capture": "normal_capture/capture.json"}],
  "training": ["normal_capture/capture.json"],
  "output": "performance/models.json"
}
```

路径相对维护请求解析。sglang_cache_io 导入正常备份、主机克隆和回载准备/提交/完成观测；ascend_tensor_device 导入独立设备服务；ascend_tensor_queue 导入正常 C++ API/线程时钟与独立 profiler 分段共同形成的端口估计。ascend_tensor 只解释已有 Python API 响应采样，不能把这类整段响应作为当前原生队列的 CPU/worker 服务。
只填 imports 可以单独入库；已有数据可省略 imports，只用 training 的原来源路径离线训练，不再打开原始采样。training 必须显式选择，不扫描整库；预热、未选择的采集及独立留出不参加训练。
可选 models 指向已有选定模型；训练只替换相同操作/实现/口径，完整结果原子写到 output。输出与数据库、请求及原始采集目录分开。报告区分新增/复用记录与训练响应；单设备数据不绑定模型名或全局 TP，平台、runtime 和实际工作条件仍须相容。

当前原生张量校准在兼容 Torch-NPU 推理环境中运行：

```bash
python3 -m markov.adapters.ascend.tensor_responses \
  --library /path/to/libmarkov_tensor_response_sample.so \
  --recipe /path/to/recipe.json --output /path/to/new_capture --trace
```

配方含 device、warmup、repeats 和 cases；案例为 clone/concatenate、int32/int64 及正整数 input_sizes，例如 [128] 或 [64, 64]。库在相应推理镜像通过 src/markov/adapters/ascend/sampling 构建，提供 MARKOV_ASCEND_ROOT 与 MARKOV_TORCH_ROOT；纯 modeling 不加载 Torch 或该库。正常采样先记录 C++ API wall/thread CPU、线程 CPU 增量及空计时/同步对照；随后另行采 profiler，不混入正常计时。输入构造、同步、线程时钟读取及结果数值核对均在 API 计时外，线程观察窗口另行保留。

在 modeling 环境解释独立 trace：

```bash
build/modeling/Release/markov_core tensor-work \
  /path/to/new_capture/capture.json /path/to/performance/native_observations.json
```

报告必须放在原始采集目录之外，保护原始 JSONL、CSV 和 trace。解释复用 base 的同一队列端口与独占设备归属规则；不创建伪造的推理帧，也不按 kernel 顺序或时间接近猜配。
维护输入使用 `{"adapter": "ascend_tensor_queue", "capture": "/path/to/performance/native_observations.json"}`；只需设备响应时使用 ascend_tensor_device。预测仅读取维护导出的模型，不在查询时训练或打开原始 trace。

正常 CPU 总量和端口位置不是同一种观察：caller 总量来自正常 C++ API，worker 总量是扣除空同步对照后的线程 CPU 用量，可能包含队列管理；提交前后及发射/返回的比例来自另一遍 profiler。caller 的正常 wall 与 CPU 差额另作残余估计，dispatch 和显式 worker residual 仍沿用独立 profiled 观察。它们不是正常端口的直接实测，未观察到的正常线程调度空白仍有不确定性，不能把 ready 或 measured_work_point 当作精度通过。

CPU 在小型已测范围内使用典型响应，cat 按输入段数区分；元素数保留为覆盖范围，不强拟合 CPU 规模系数。设备使用元素规模响应。clone 的 worker 描述沿用固定提交变量，设备覆盖另行判断。外层仍负责设备空闲、严格串行、累计预算及回收；其他平台、内存压力、首次加载、队列拥塞、自动补采及应用准备尚未由此取得资格。

### 已实现的独立成本查询

```bash
scripts/run.sh modeling -- build/modeling/Release/markov_core costs /path/to/cost_queries.json /path/to/cost_report.json
```

这是模型维护与覆盖检查入口。查询文件给出 deployment、models 路径和 work 列表；相对模型路径以查询文件所在目录为起点。
deployment 沿用预测请求的六项条件。每项工作为 operation、implementation、timing_scope 和数值 features；
正式目标规划器接入后生成这些描述，应用用户不需要手工逐节点填写。

选定模型文件只有 deployment 和 models 两项。每个模型给出相同的操作/实现/计时口径、response、
特征 ranges、完整 measured_points、extrapolation_features、allow_new_combinations 及 sources。
可选 limitations 是已知测量与预测限制的文字列表；未提供时为空，不改变工作域或公式。
response.form 为 affine 时包含 intercept_ns 和 coefficients_ns；为 coefficient_curve 时包含 feature 与 points，
每个测点含 coordinate、intercept_ns 和 coefficients_ns。一个锚点表示仅支持该坐标；多个锚点严格递增，
曲线坐标的适用范围不得超出测点区间。
这些是离线维护结果，不在查询过程中拟合；旧成本资产需显式转换单位和合同，程序不猜格式或长期兼容两套字段。

报告逐项返回状态、可用时的整数 duration_ns、覆盖类别和模型来源引用。缺失或不支持的耗时为 null。
used_models 同时保留 sources 和 limitations；page-work 的 cost_catalog.queried_model_limitations 汇总本次共享查询会话遇到的限制，不表示这些工作已经提交或执行。
模型的部署条件必须与请求一致；不同 scope 的耗时不会被自动求和。查询完成不代表目标结构、完整 HTTP 或精度验收通过。
报告不能覆盖查询文件或选定模型文件，写入失败时不以不完整文件替代原报告。

独立性能维护还提供 SGLang Ascend 输入内核采样模块。在已有推理 SDK 环境、本项目 Python 包可导入时运行：

```bash
python3 -m markov.adapters.sglang.input_kernels --recipe /path/to/recipe.json --output /path/to/new_capture
```

采样输入给出 device、table_columns、warmup、repeats 和 cases；每项工作量包含 query_tokens、prefix_tokens，均以 token 为单位。
它用实际框架内核测量单行位置生成和请求页表写入，预热及结果校验位于 profiling 之外，输出原始 trace、逐任务 measurements 与完成记录 capture.json。
任务数量、顺序与同步边界必须对应后才发布完成；新目录避免覆盖已有资产。外层负责设备空闲、串行、预算与进程回收，正式准备入口尚未接入这一模块。
这些结果供独立成本维护使用；原始 DAG 仍由所选 base 的 profile_manifest.json 构建。

## workload 规模与可复现性

请求数量、每请求输入与生成上限由 workload 显式控制，不按实验名称硬编码，也不裁剪已采 trace 来缩小 DAG。
先选择足以触发命中、跨层传输和容量竞争的短负载，再逐步增加规模。
自然生成可先记录真实输出 tokens 与结束原因，再在配置间重放同一工作；这不等于预测尚未生成的内容或 EOS。
准备阶段可影响正式窗口的缓存和后台 I/O，必须连续执行；完整 HTTP 只统计正式请求边界。

## 结果与诊断

### 显式成本回放

```bash
scripts/markov.sh replay --request /path/to/replay.json
```

请求给出 manifest 和独立 output，可选 node_scale、overrides、inspect_name 和 memory_limit_gib。
路径相对请求文件解析。示例请求为 {"manifest": "/path/to/profile_manifest.json", "output": "/path/to/replay"}；
默认沿用源观测成本，完整细图只在该诊断命令中构建，不进入普通预测。
inspect_name 为空字符串时列出服务名字及数量；填入名字子串后列出匹配节点的 ID、原成本和绑定后成本。
默认不生成名字统计或逐节点数据。
overrides 例如 [{"node": 123, "duration_ns": 456}]，只替换明确节点，随后再应用 node_scale；
边界只能为零，重复 ID、负耗时和越界 ID 会拒绝。缩放只作用于服务，gap、等待和边保持原样。

预测可显式设置 execution.record_graph=true，把已完成目标的 execution_graph 和逐请求 start_node/response_node 写入报告。
默认关闭，不建立工作副本或输出逐节点明细；缺成本或未完成的目标不导出可回放图。
图保存工作变量、成本来源、组件归属、原执行节点、端口及依赖；被取消工作不作为零成本节点进入固定路径。
回调触发后续工作的关系另存为 submission 边，已触发的定时器保存为相对其启动原因的等待。
原开始/结束时间只用于比较，不作为回放调度约束。资源实际次序保留，改成本后下游可随依赖移动。

回放目标图的请求使用 prediction 与 target，替代 manifest，例如
{"prediction": "/path/to/prediction/report.json", "target": "candidate", "output": "/path/to/replay"}。
其余缩放、替换、检查名字和内存参数沿用；目标的 work 标签可包含 caller 延迟，按相应工作缩放，源残余不随之缩放。
相对 deadline 是策略条件，不能当成操作成本覆盖。报告给出逐请求变化和 operation_timing_differences，后者比较原有操作的全部起止时间。

该入口保持所选图及资源次序，替换值由调用者负责关联和说明来源。
目前没有自动 target 操作匹配；保存和复现预测图本身不能证明它与 target 真实结构相同，也不是正常精度或完整 oracle-cost 验收。
回放不重新选择时间分支，改成本后原分支可能不再成立，结论只适用于固定路径。
源回放容器只读指定采集，目标图回放只读所选预测报告；两者无设备、无数据库、无网络，输出不覆盖输入。

普通预测只给目标配置、完整 HTTP 时间、状态、成本来源覆盖、外推/近似说明和准备开销。
未知或失败时间为缺失值，不能为零。执行成功、存在适用模型和精度达标是不同事实。
源回放、目标结构比较、性能评分三份报告只在实验入口按需生成；预测请求不接收 target 真值路径。
详细图和逐操作报告通过显式诊断开关生成，不让默认应用承担全量诊断内存与写盘成本。

## 实验与搜索

矩阵案例、留出与跨运行汇总由 examples 编排，复用正式预测和独立评价入口；全部预测结束后才独立读取正常实测和结构证据。
搜索只声明可部署资源选项、策略空间、性能要求、资源计价和预测预算，共用同一预测器。
资源价格来源或计价口径不明确时，只输出资源占用和相应假设，不发布实际经济收益。
正常确认只测事先选定集合，失败和重复均保留；确认不回写本轮规则、参数或候选排序。

## 未具备条件时怎样结束

区分输入错误、来源事实不足、结构不支持、成本缺失、域外外推、资源不足、执行失败和评分未就绪。
报告缺什么、影响哪些目标、已有数据能否解决及建议动作；不让用户阅读内部英文堆栈才能判断下一步。
底层原始错误留在有限失败日志，摘要使用明确中文或简短业务描述。
