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

诊断请求只有 manifest 和 page_sizes_tokens，例如 `{"manifest":"capture/profile_manifest.json","page_sizes_tokens":[64,128]}`。
manifest 的相对路径以请求文件所在目录为起点；页大小必须为正整数。这项诊断读取真实来源，共享一次准备后生成各页几何。
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
它不运行目标配置，也不验证目标 DAG 相似度或性能；源回放使用观测成本，不能当作目标预测精度。

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
response.form 为 affine 时包含 intercept_ns 和 coefficients_ns；为 coefficient_curve 时包含 feature 与 points，
每个测点含 coordinate、intercept_ns 和 coefficients_ns。测点严格递增，曲线坐标的适用范围不得超出测点区间。
这些是离线维护结果，不在查询过程中拟合；旧成本资产需显式转换单位和合同，程序不猜格式或长期兼容两套字段。

报告逐项返回状态、可用时的整数 duration_ns、覆盖类别和模型来源引用。缺失或不支持的耗时为 null。
模型的部署条件必须与请求一致；不同 scope 的耗时不会被自动求和。查询完成不代表目标结构、完整 HTTP 或精度验收通过。
报告不能覆盖查询文件或选定模型文件，写入失败时不以不完整文件替代原报告。

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
