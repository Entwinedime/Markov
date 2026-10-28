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
不同参与进程的计算用显式汇合边界衔接，串行请求依赖前一响应完成；实验响应端口仍只覆盖部分路径，入口和尾部沿用同请求/前端的源残余近似。
后台 I/O 可在响应后继续，显式清空屏障依赖其实际完成；不迁移源绝对时间、设备等待或诊断 checkpoint 的耗时。
这个组件假设每个参与进程有独立的回载和备份队列，未包含其余 CPU 和完整跨流依赖；计数与顺利执行不代表完整 DAG 或 HTTP 精度。

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
