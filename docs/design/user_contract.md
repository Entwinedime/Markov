# 用户输入与正式流程

本文定义用户输入、准备行为和结果合同；下方操作说明仅列已实现入口，完整预测合同仍作为后续接入边界。

## 输入预览和采集检查

```bash
scripts/markov.sh plan --request configs/prediction.json
docker build -t markov-modeling:ubuntu24.04 -f docker/modeling/Dockerfile .
scripts/build.sh Release
scripts/run.sh modeling -- build/modeling/Release/markov_core inspect-manifest /path/to/profile_manifest.json
```

plan 读取请求、核对 workload/manifest/模型包的声明路径并列出已知准备动作，不构图、启动设备或创建结果目录。
输出 not_evaluated 列出尚未分析的源语义、目标工作和成本覆盖，不能据此认定模型可用。
manifest 检查只读取 manifest 和文件元数据；输出 inputs_accessible 不代表通道内容、源 DAG 或正常性能已通过。

请求路径以声明它的文件目录为起点。base 二选一：manifest 复用真实来源，configuration 声明需要采集的配置。
每个 target 只有 name 和 configuration；name 用于定位结果，不作为结构或成本特征。
deployment 包含 platform、framework、runtime、model、dtype、tensor_parallel；工作配置由相应适配器解释。
performance.models 可省略，表示独立模型尚未选定；execution 必须给 output，jobs 默认为 1，memory_limit_gib 可显式声明。
示例在 configs 中，不预置本机模型路径、设备编号、评分文件或实验身份。

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
将返回的 manifest 显式填入 base.manifest 可复用，或加 --recapture 重新采集并保留已有额度消耗和文件。
失败不会把上一份 manifest 当成本次成功返回。进程中断遗留 running 状态时，目前须先核对本任务容器与实际用量，再恢复状态；尚无自动孤儿恢复。
用户仍需自行提供适用的独立成本；采集入口没有恢复完整 CPU 校正和数据库准备。

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
cache_execution.copy_ports 逐逻辑复制列出页数、各参与者的 cache_control_returned_ns、completion_observed_ns 与 published_ns。控制返回包含后续主机索引克隆；发布等待两个端口的全部参与者。completion_basis 区分 modeled_observation_upper_interval 和 device_service，前者包含同步观察开销，不能当作精确设备完成。淘汰或缺少结果观察的旧路径仍使用设备传输口径，不能据此认为其外围 CPU 已覆盖。
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
prefetch_stop_policy 支持 wait_complete、best_effort、timeout；prefetch_threshold_tokens 可省略，使用当前原生默认值。
prefetch_timeout 可声明 base_sec、per_ki_token_sec、max_sec，按原生线性预算计算，从操作入队开始计时。
报告分别列出查询、工作进度、控制端接受、新增主机页、被拒绝发布的复制和最终 worker 状态，不能把它们都叫作命中。
当前只覆盖对称参与进程下的 I/O 与已有计算子集。查询及准入 CPU 尚未绑定，因此 best_effort 按即时准入停止，
timeout 也基于这组部分成本；阶段耗时拆分含近似，不能拿这些运行结果当作完整 HTTP 精度。

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

路径相对维护请求解析。当前唯一导入适配器读取真实 cache_io 采样的完成记录及同目录 measurements.jsonl，核对声明案例、重复数、预热和物理校验。它导入备份返回、完成观测及主机克隆三类响应，不加载模型权重、base/target trace 或 SDK。
只填 imports 可以单独入库；已有数据可省略 imports，只用 training 中的原来源路径离线训练，不再打开原始采样文件。training 必须显式选择，不自动扫描整库，也不能重复选择同一采集。预热及未选择的采集不用于训练，独立留出另经成本查询评价。
可选 models 指向已有选定模型文件；训练结果只替换相同操作/实现/口径的响应，并将完整选定包写到 output。输出必须与数据库、请求及原始采集目录分开，完成后原子发布；失败不把半写模型当作成功。
报告给出新增/复用记录、有效训练响应数、训练模型数和输出位置。单设备数据不按模型名或 TP 声明重采，但平台、实现条件及实际工作几何必须相容；这一入口不提供任意模型/平台的精度保证，也未接入自动设备补采、一般 runtime trace 导入或 CPU 校正。

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
每个测点含 coordinate、intercept_ns 和 coefficients_ns。测点严格递增，曲线坐标的适用范围不得超出测点区间。
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
