# Markov

项目正在全量重构。旧实现、配置、环境和文档在本地 deprecated 中保留，并已保存到历史提交。
当前提供输入预览、SGLang/Ascend base 采集、manifest 检查与 modeling 构建；原始 DAG 回放已进入真实输入验证，完整目标预测和搜索仍在恢复。

项目继续使用三阶段路线：base trace 构建原始 DAG，配置与执行规则生成目标工作和依赖，
独立成本模型与未变工作的源成本填入后执行，预测完整 HTTP。实验与资源成本搜索在上层共用这一预测器。

## 先运行输入预览

```bash
scripts/markov.sh plan --request configs/prediction.json
```

预览只核对声明输入并列出准备动作，不启动容器或设备，也不创建预测结果。
请求中的相对路径以请求文件目录为起点；示例 base 需要采集且未提供独立模型，输出会明确这些缺口。
源语义、目标结构和成本覆盖必须由后续正式阶段分析，预览成功不表示能够预测。

声明本地模型、兼容推理镜像、设备及累计预算后，可用同一请求采集 base：

```bash
scripts/markov.sh capture --request /path/to/request.json
```

采集串行执行 workload，导出 trace、轻量语义观察和客户端时间，并返回 manifest；具体输入见用户合同。
采集成功只表示所需文件和观察已导出，尚不表示完整源因果、正常成本或目标预测已验收。

构建和检查已有采集输入：

```bash
docker build -t markov-modeling:ubuntu24.04 -f docker/modeling/Dockerfile .
scripts/build.sh Release
scripts/run.sh modeling -- build/modeling/Release/markov_core inspect-manifest /path/to/profile_manifest.json
```

manifest 检查核对声明通道文件和访问路径，不加载 trace 内容或构建 DAG。
modeling 开发容器无设备和网络，默认内存上限 4 GiB；通过 `MARKOV_MODELING_MEMORY` 显式调整。

原始 DAG 的实验诊断入口：

```bash
MARKOV_MODELING_MEMORY=8g scripts/run.sh modeling -- \
  build/modeling/Release/markov_core source /path/to/profile_manifest.json /path/to/source_report.json
```

目前只验证 SGLang 来源；报告保留源等待与 gap，分别列出请求回放，以及服务、等待和零时长边界的时间差异，不能当作正常运行预测精度。
设备依赖与 KTransformers 尚未完成验收。此命令严格读取 manifest 声明的文件，不自动修复不完整采集。

HiCache 页几何的实验诊断入口：

```bash
MARKOV_MODELING_MEMORY=8g scripts/run.sh modeling -- \
  build/modeling/Release/markov_core page-work /path/to/page_work_request.json /path/to/page_work_report.json
```

请求文件包含 manifest 路径与 page_sizes_tokens 数组，可选 models 指向独立模型。它共享一次来源准备，按各页大小重新划分请求前缀，
同时给出压缩前缀树、先前提交的前缀交集及源回放；报告中的可复用上限没有考虑缓存容量和可见性，
不能当作实际命中或目标 DAG 预测。已解释的计算原语也会按 query/context 重新生成工作量，
但这只是在保留全部前缀、没有 padding 的假设下检查部分计算，不是完整目标程序或性能预测。
提供 models 时，未变且独占关联的设备工作沿用 base 服务，其余工作查询独立成本，并在统一执行器运行；缺成本明确停止，不给出帧或 E2E 耗时。
可选 cache 参数进一步运行有限容量的设备/主机缓存、回写、回载及计算子集；其中 storage 可声明空初始文件存储及预取策略。
逐页发布、超时停止、缓冲区归还及已解释的阻塞通信接入同一时钟；通信等待实际输入到齐后才绑定共享服务。
注意力元数据、已支持的 Prefill/Decode 分配与计算调用共用 CPU 提交、后台发射、设备及返回链路；分配流的产出依赖保留到计算流，各参与者暂共享最晚通信完成时刻。
分配目前限未排序 NPU 页池：Prefill 使用 int64 位置及少于 200 个新页的融合路径，Decode 使用 int32 位置及单 token 步长。
数值执行须显式声明目标的 kernel_cache_initial_state：empty 表示相关内存/磁盘缓存均空，prepared 表示所需内核均已准备。
每个目标维护自己的规格缓存及启动器加载状态，新规格按独立数据支付编译与加载成本；缺少初始状态会停止数值执行，不猜测已有准备。
请求页表写入、输入/位置生成和必要的采样位置准备已生成工作与流交接，常规执行成本不足时会汇总当前片段缺口，不填零或继续发布耗时。
输入 caller 的连续准备归入相邻调用前后段；实验报告另列与 base 原生队列的匹配，匹配成功不代表正常成本或完整设备依赖已验证。
实验入口另列请求开始、到达、准入、响应开始与完成端口；串行请求在前一响应完成后启动，后台计算、整理与 I/O 可继续运行，显式 drain 屏障等待其实际完成。
入口与响应尾部暂保留同一请求、同一前端部署的来源间隔，并单列为残余近似；它们不占 CPU 服务资源。
同次观察到重叠调度时，已建模调用在 CPU 提交后允许下一帧准备，worker/设备队列分别保留前沿，Prefill 与 Decode 连续执行。
串行 int64 输出已生成长度更新、token 中继、异步拷回与完成标记；帧内 CPU 间隔仍是来源残余近似。
结果可读端口由拷回完成标记产生，单列于 caller 提交和全部已建模工作汇合；后者不能替代结果就绪。
未结束的 Prefill 在各参与者结果可读、必要的后续提交已到达后发布 prompt 页前缀，写策略可在后续 Decode 期间启动备份。
纯 CPU 结果整理等待对应输出及所需提交后执行，并约束后续调度；发送端口与发送后的整理分开保留。
缓存更新/提交在执行中的控制点读取目标状态，生成插入索引克隆、匹配拼接及页表回写；初始索引复制、即时/分组释放也进入同一原生链。这些元数据复制不与 KV 内容传输相加，缺少来源段数等工作依据时不能直接迁移成本。释放等待实际队列、另计就绪同步服务，各参与者完成页池更新后才归还容量；成本范围不足时停止。
观察到最终发送时即接出响应，缺少该观察时保留完成屏障；后台请求各自保留缓冲区和缓存引用，调度线程及原生队列跨请求共用。
发布同时等待参与者索引工作与必要 KV 输出；最终 KV 仍保守地等待全部所选输出。准入预留与实际分配分开，每轮输入取得执行权后才读取页池快照、展开原生工作。请求查询的索引分裂和拼接也进入原生链；缺少来源段数或克隆尺寸依据时不迁移成本，所缺独立响应明确报告。
结果控制点的备份分别生成主机调用与异步完成工作，再执行主机索引克隆；各参与者的调用返回和完成观测汇合后才发布主机数据，不再另加裸 DMA。完成模型估计的是含同步观察开销的区间，当前只适用于已测初始化条件，不能当作精确设备服务或竞争模型。淘汰触发备份的外围 CPU、查询外围 CPU、其余请求控制与跨流依赖尚缺，不输出完整 HTTP 预测。

可选 source_cpu_costs 指向与该 manifest 对应的 CPU 校正文件。报告按原始图区间区分服务、等待和空白，
分别列出已知计算调用及其外部 CPU 的覆盖，同时汇总全部请求与正式请求。校正不改原始 DAG，
调用之间的区间按同 caller、同调用配方保留残余近似：覆盖部分使用校正估计，未覆盖部分保留原始值，二者分别报告。
输入原生调用可按各自工作与资源沿用 base CPU 片段估计；worker 提交量与设备 shape 分开，框架入口和实际数组规模保留必要条件。已知同步等待由依赖承担，编译/加载及上传 API 不重复收费。报告另列候选区间与查询次数，校正未覆盖的源时间仍有近似。
已解释的变长张量分配单独生成工作，并用正常 ATen API 的独立响应估计；源 CPU 包装先扣除对应分配区间，避免重复计费。就绪分配器的测量不覆盖所有内存压力或首次初始化状态。
已计费调用和已标识采集开销不再相加；遇到尚未建模的原生提交、未表达依赖的已知等待或来源缺失时停止该片段。
输入派发仅沿用入队与前一 worker 完成后剩余的来源间隔，单列为残余近似；设备事件只引用已经证明未阻塞的源标记区间，目标仍等待实际生产者。
每次发射的未变输入设备工作须有唯一提交及完整独占服务依据才能迁移；已支持的 Decode 页表写入分别生成广播与索引写入。普通 worker 片段与独立测量的首次使用增量分开计费。
其他 CPU 路径、变化工作量的 CPU 成本和完整请求控制仍未恢复，不能当作完整 HTTP。

独立成本查询已有单独入口，供模型维护时检查工作覆盖与响应：

```bash
scripts/run.sh modeling -- build/modeling/Release/markov_core costs /path/to/cost_queries.json /path/to/cost_report.json
```

它只读取选定模型和工作描述，不读取 base 或 target trace，不训练模型；结果是操作成本，尚未接入完整目标预测。

SGLang 推理环境另有轻量输入分支观察，启用方式见[用户合同](docs/design/user_contract.md#sglang-输入分支观察)。
它不采 snapshot，提供请求边界、实际输入分支及准备工作；集成在 base 采集中，不直接提供正常成本。

## 新文档从哪里读

- [整体架构](docs/design/architecture.md)：三阶段、共同合同、粒度与建议源码职责。
- [用户合同](docs/design/user_contract.md)：输入、已交付入口及后续准备边界。
- [成本与数据库](docs/design/cost_models.md)：源成本迁移、独立测量、离线模型和补采边界。
- [验证方式](docs/design/validation.md)：三份报告、完整 HTTP、60/186 格和 10× 效率口径。
- [工程与文档约束](docs/design/engineering_constraints.md)：代码表达、维护边界及三层文档规则。
- [项目工作流](docs/workflow.md)：精简里程碑、当前判断和仍保留的资产。
- 本轮重构：[详细计划](docs/windows/2026-10-27_full_rebuild/plan.md)、[当前状态](docs/windows/2026-10-27_full_rebuild/status.md)、[执行日志](docs/windows/2026-10-27_full_rebuild/log.md)。

## 目录

| 路径 | 当前用途 |
| --- | --- |
| 本地 `deprecated/src/` | 旧采集器、Python probe、C++ hook、DAG 与预测后端 |
| `deprecated/scripts/` | 旧用户入口、准备、成本维护、预测、评分和搜索工作流 |
| `deprecated/examples/` | 旧复现和搜索研究脚本 |
| `deprecated/configs/` | 旧实验、workload、profiling 与 modeling 配置 |
| `deprecated/docker/` | 旧 SGLang、KTransformers 与 modeling 环境定义 |
| `deprecated/CMakeLists.txt`、`deprecated/.dockerignore` | 随旧实现保留的构建入口和 Docker 上下文规则 |
| `deprecated/docs/` | 完整旧文档及临时计划、日志，保留原内容与历史口径 |
| `python/markov/` | 用户输入合同与任务编排 |
| `src/markov/` | C++ 来源输入、后续 DAG/结构/成本/执行核心 |
| `scripts/`、`docker/` | 薄入口及独立建模环境 |
| `configs/` | 可直接预览的最小请求与 workload |
| `docs/design/` | 新实现的静态设计与约束，不记录每轮进度 |
| `docs/workflow.md` | 项目级当前判断与精简里程碑 |
| `docs/windows/` | 同一任务维护一组 plan/status/log，任务、当前状态和历史事实分开 |
| `third_party/` | 原位保留 SGLang、KTransformers 子模块，未归档或删除上游依赖 |
| `data/`、`build/`、`ppts/` | 原位保留本地数据、构建产物与汇报资料；不提交生成物 |

`.clang-format`、`pyproject.toml` 与 `AGENTS.md` 留在根目录，作为后续开发的格式和工作约束。
默认 Ruff 检查跳过 `deprecated/`；显式检查归档代码时需使用相应路径和排除覆盖选项。

## 旧实现与实验

归档前的主仓库提交为 `8668eec`，已按采集、C++ 建模、Python 工作流、实验脚本和文档分层提交并推送。
SGLang 配套改动已推送，子模块提交为 `99e8be732`。完整旧文档的历史归档提交为 `824b458`。

旧使用说明保留在本地 deprecated/README.md。参考目录不再跟踪；完整内容可由归档提交恢复。
归档中的源码、配置和旧说明保留迁移前内容，
其中的命令与相对路径按原仓库布局理解；它们不是根目录当前可运行的入口。
需要运行旧流程时，使用归档前提交的独立 checkout，不将归档目录直接视为已适配的新运行根目录。

当前结论及资产位置见 [`docs/workflow.md`](docs/workflow.md)。旧搜索任务已终止；新实现的采集与验证范围由当前窗口记录，
历史成绩不代表新版预测已经恢复。

归档不计作源码瘦身，输入检查不计作预测恢复；新实现的精度和速度仍需后续实际验证。
