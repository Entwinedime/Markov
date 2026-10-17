# 当前构建与运行

本页只维护当前代码的构建、目录职责、公开命令和故障处理。当前能力与验证程度见[主计划](../redesign-workflow/2026-10-08_redesign_execution_plan.md)，稳定合同由[设计入口](00_overview.md)导航。根 README 保持为空。统一路径、正式对照与 `example/` 已通过空资产有限主线验收；清理后重建、故障及终态恢复的范围见[专项审查](../redesign-workflow/history/2026-10-16_code_cleanup_review.md)，不将其扩大为一般结构或精度资格。

主线是 SGLang + Qwen3-32B / Ascend；KT 保留采集，尚无模板预测。计时范围以[性能合同](06_performance_execution.md)为准，运行完成不代表精度通过。以下路径相对仓库根；示例中的 `path/to` 和配置 ID 需替换为实际输入。

按任务查阅：[构建与代码目录](#1-工具链与构建)、[集合入口](#2-集合入口)、[全流程示例与对照](#全流程示例与独立对照)、[修改后检查](#3-修改后的必要检查)、[显式采集](#4-显式诊断采集)、[重提取/编译](#5-已有-base-的-source-提取)、[性能采样与拟合](#6-独立性能采样)、[结果与恢复](#7-条件预测与恢复)。

## 1. 工具链与构建

在仓库根目录执行：

```bash
docker compose -p markov-next-redesign -f docker/compose/inference.yml build modeling
scripts/build.sh
```

使用既有 Ubuntu 24.04 modeling 环境，C++23、语言扩展关闭，CMake≥3.20、Ninja、nlohmann-json≥3.11 与 Zlib（读取显式 gzip JSON 资产）。当前编译器为 GCC 13.3，CMake 3.28.3、libstdc++ 13.3、JSON 3.11.3；clang-format 18 用于格式检查。原 clang 18 与 libstdc++ 13 的组合未开放本次实际需要的 std::expected，因此镜像改用已有 GCC，没有添加旧标准回退。正式 target 为 markov_core、markov_adapters、markov_workflow 和 markov；默认 Release 产物位于 build/markov，未加入 CTest 或独立测试 target。已有构建目录切换编译器后须重新配置 Release，不沿用重置后的默认类型。

脚本固定 Compose project 为 markov-next-redesign，规避本机已有的重复 markov-next_default 网络，不改变其他网络或镜像前缀。编译、执行及 C++ 静态工具均在 modeling 容器内，宿主不作为验证工具链。

### C++ 目录与职责

`src/markov/include/markov/<职责>/` 与 `src/markov/src/<职责>/` 对应放置声明和实现；`cli` 仅有实现。不保留旧路径转发头文件。目录表达流程职责，框架与运行环境是适配层内的两个维度。

```text
src/markov/{include/markov,src}/
├── contracts/                         # 公共值合同、目标服务工作及共用 JSON 输入校验
├── graph/                             # 来源图、节点和依赖表达
├── production/                        # SourceBindings、SourceCall、公共 BlockExtractor
├── adapters/
│   ├── frameworks/sglang/             # serving 绑定、子图提取、模板生产及采集配方
│   │   └── models/                    # Qwen3 模块声明、模型/attention 原生布局解释
│   ├── frameworks/ktransformers/      # 已有采集的配置与准备；不提供模板预测
│   └── runtimes/ascend/               # CANN 事件关联、AscendSourceExtractor、HCCL 布局
├── templates/                         # 统一模板、参数化、实例化及有限变换
├── controller/                        # 轻量控制器及业务状态
├── execution/                         # 目标执行图、资源、完成事件及唯一模拟时钟
├── performance/                       # 给定耗时、物理工作经验模型、统一成本查询
├── workflow/                          # ProjectReader、输入规划、生产与预测编排
└── cli/                               # 命令解析及适配器具体实现的组合
```

| 抽象步骤 | 实现与交接 |
| --- | --- |
| 定义流程 | `contracts/control` 定义 block、控制动作和端口；不是框架函数名集合 |
| 确定插桩位置 | `adapters/frameworks/sglang/bindings` 声明 serving 位置；`models/qwen3_regions` 声明模型模块及参数；Python wrapper 执行这些绑定 |
| 取得子图 | Ascend 提取器关联设备/CPU 事件，经 `SourceBindings` 使用框架语义；公共 `BlockExtractor` 处理归属和依赖，SGLang 实现解释原生端口 |
| 模板化 | `SGLangTemplateBindings` 将原生形状、KV、模型及通信关系编译为拥有数据的 `TemplateSource`；原生解释不进入目标运行时 |
| 变换与展开 | `templates` 消费统一参数及有限规则；`controller` 决定动作；`execution` 绑定依赖、资源并反馈完成；`performance` 提供节点耗时 |

`templates/template_definition.cpp` 负责来源结构、区域接口及替换计划；`templates/template_bank.cpp` 负责候选选择与目标参数化；`templates/template_topology.cpp` 应用拓扑变换和 donor 插入；`templates/template_assembly.cpp` 负责实例身份与跨批资源前沿拼接。各文件沿用现有类，不增加转发层或另一份请求状态。

`execution/simulation.cpp` 负责事件推进和汇总；可选的 `ExecutionDetails` 只生成详细诊断，将不变来源描述与目标调用分开，不参与调度、变换或成本查询。

构建依赖为 `markov → markov_workflow → markov_adapters → markov_core`。core 包含 contracts、graph、公共 production、templates、controller、execution 和 performance；核心源文件不包含 adapters/workflow 头文件，适配层不反向调用 workflow。三个库 target 对应实际职责边界，不为每个 block 建库。

`workflow/project` 组合配置、请求集合与资产输入；`adapters/configuration` 汇总可选择的框架配置，具体值类型由各框架目录拥有。核心仅包含 `contracts/request`、`contracts/environment` 和 `ServingLimits` 等统一值；头文件的间接依赖也不得把具体框架类型带入核心。Qwen3/Ascend 模块位置、kernel 名称、原生 hook 和 HCCL 实现规则属于适配端。模型原生布局目前仍是 SGLang + Ascend 支持域，不宣称是任意框架的通用 Qwen3 适配。

### Python 采集目录与职责

```text
src/collection/
├── markov_collection/
│   ├── workflow/                      # Docker 任务、预算、采集/处理/预测装配与恢复
│   ├── instrumentation/               # BlockWrapper、NativeRegions、Recorder、CaptureSession
│   ├── artifacts/                     # JSON 发布、独立测量身份与覆盖合同
│   └── adapters/
│       ├── frameworks/
│       │   ├── sglang/                # 插件、wrapper、AST 插桩、engine/request、框架采样
│       │   └── ktransformers/         # 已有 BalanceServe 观察路径
│       └── runtimes/
│           ├── pytorch/               # ProfilerMarkers、观察范围与归一化
│           └── ascend/                # 环境、导出/规范化、HCCL 测量
│               └── torch_npu/         # profiler 配置与 workspace 快照
└── adapters/                          # 镜像内构建所需的原生适配资源
    ├── frameworks/{sglang,ktransformers}/ascend/  # 安装及框架/Ascend 组合适配
    └── runtimes/
        └── ascend/
            ├── native/                # 单独构建的 CANN SDK 拦截库
            └── torch_npu/             # workspace patch、观察头文件、安装脚本及适用说明
```

公共 instrumentation/artifacts 不导入具体框架或运行环境。`Recorder` 接收 `ProfilerMarkers`，负责身份、嵌套、标量和单调时间边界；PyTorch 实现提供 marker。不再采集无消费者的线程 CPU 服务时间。已有 native journal 的标量 C ABI 仍是记录器与 native 插桩库的交接，SDK 类型和拦截符号留在 Ascend native 目录。

`ProfilerMarkers.observer()` 负责显式观察范围，wrapper 的元数据准备和记录工作由 runtime 提取时排除；语义身份及原生因果保持。边界与限制见[观察工作与原生工作分开](04_collection_templates.md#观察工作与原生工作分开)。

`NativeCapture` 在 workflow 入口检查权限并选择框架会话；`CaptureSession` 保存原生输入和失败生命周期，SGLang/KT 实现原生执行。结构及 device 采集的 Ascend 产物由外层统一发布，并在原生启动前记录日志偏移；transfer 会话记录成功 driver 搬运叶子，不发布 profiler 产物。独立通信入口继续使用同一 Ascend 发布器。runtime normalizer 仅作为库被 `workflow.processing` 调用，后者独占处理锁和终态发布。`SamplingDomain` 属于共享产物合同；框架适配器可以组合环境适配器，运行环境层不得反向依赖框架层。

`workflow.modeling_job.ModelingJob`统一预测与成本拟合的具名CPU容器、只读输入挂载、活动状态、等待及日志；各自workflow准备输入并发布有锁结果。原生采集的设备租约、预算及进程清理仍由`NativeAttempt`负责，不并入这个CPU处理类。

`artifacts/json_file` 负责普通/gzip JSON 读取和原子发布，发布时创建父目录；快照与环境观察器不重复维护编码/目录助手。JSONL 追加保留自己的目录及流生命周期。transfer 性能插桩使用必要批次范围与原生队列身份，不安装 CPU 成员体、函数 gap 或模型语义区域。

Python 负责观察原生运行并写入显式文件；C++ 负责来源关联、子图提取、模板生产和目标展开。共同交接是 block/region 身份、父子关系、节点/依赖、参数、端口和来源，不传 Python 对象到 C++，也不复制两套流程定义。主模块内部路径已更新，公开 `scripts/markov.sh` 命令不变。

Dockerfile 选择具体组合、复制适配目录并直接调用其中的安装脚本，不再经过无行为的 `docker/install` 转发层。原生版本约束、框架源码适配、探针补丁及构建步骤由 `src/collection/adapters` 拥有。Python 包中的同名目录负责运行期间的观察与解析，两者共同实现同一个适配职责，不通过目录扫描动态注册。当前组合及安装归属见[适配组合](framework_paths.md#适配组合与源码归属)。

workspace、attention 等规则由生产端解释，统一资产保存共享规则及使用点引用；执行层独占目标状态。字段、资源和生命周期见[编译资产合同](04_collection_templates.md#编译资产与预测加载)及[workspace 机制](sglang_template_relations.md#workspace-容量状态与设备级同步)。运行说明不另维护一份内部字段迁移日志。

### 改动应该落在哪里

- **只换 request 序列**：使用已有合同与模板参数，修改输入；不新增 wrapper 或逐长度模板。
- **换模型**：在框架的 `models` 中声明原生模块结构及绑定；缺少真实语义或有限变换时才扩展公共合同，不能默认已有 attention/MLP 规则适用所有模型。
- **换框架或框架原生 API**：修改对应框架适配器的插桩位置、观察字段、子图归属及模板生产；workflow 负责选择实现。不能把框架分支塞入模板消费者或控制器。
- **换 env/profiler/SDK**：修改 runtime 适配器的 marker、导出、事件关联和 native 构建；同名框架不意味着不同环境的 trace 格式一致。
- **换并行策略**：生产端解释原生通信/分区为统一关系，公共模板层执行有限变换；若新策略改变调度语义，再扩展控制合同与控制器。不能承诺任何并行策略仅改 shape 即支持。

框架、运行环境的真实版本属于部署条件。版本变化先核对原生位置及输出合同，再调整当前适配实现，不建 v1/v2 内部兼容目录、动态插件注册中心或空适配器。跨框架复用的前提是适配后满足同一合同，目录分层本身不代表新框架或新版本已经通过验收。

## 2. 集合入口

```bash
scripts/markov.sh plan --project configs/redesign/sglang/project.json
scripts/markov.sh predict --project path/to/project.json
scripts/markov.sh status --run data/path/to/run
scripts/markov.sh resume --run data/path/to/run
scripts/markov.sh resume --run data/path/to/run --template-assets template-assets.json
```

当前 `RunDirectory` 在项目输出目录下创建“UTC 开始时间（Z）+框架/模型名”的运行根，例如 `20261016T115105.140401Z_sglang-qwen3-32b`；UTC 格式避免 torch_npu profiler 拒绝时区偏移中的 `+`。UUID 保存为任务身份，用于容器命名，通常不写入目录名。恢复沿用原根和身份。公开 Shell 的 Python 分发位于 `workflow/cli.py`，不直接执行 `collection.py`。

已有路径职责如下：`inputs/` 保存规范化输入，`captures/<role>/<编号>/` 保存捕获与 manifest，`assets/templates/`、`assets/costs/`、`assets/catalogs/` 保存正式资产，`results/` 保存预测、实际对照和摘要，`work/` 保存未交接处理，`logs/` 保存任务日志。诊断开启时写 `diagnostics/`。空资产示例已通过新采集到独立验证的有限主线验收；当前证据及边界统一见[审查记录](../redesign-workflow/history/2026-10-16_code_cleanup_review.md)。

`ContainerEnvironment` 负责宿主/容器路径和 Docker 环境，`RunDirectory` 负责一次运行内的布局。父任务状态与 `capture` 子记录分别描述全流程和当前采集，原生 attempt、处理收据和 manifest 分别记录执行、交接和资产；状态所有权已按专项完整审阅，原生/处理收据不直接改父账本。

预测任务的`predict/status/resume`终端JSON是任务与组合摘要：保留状态、活动、预算、范围、缺口和整体指标，`request_count`来自已生成的逐请求指标数量，未生成时为null。完整需求、资产来源及逐请求指标保存在`run`目录下`prediction.result`指向的报告；`prepare-performance --prediction-result`读取这份完整报告，不使用终端重定向文件。C++内部JSON和普通采集任务输出不按此摘要裁剪。

性能需求对象以`actual_services`和`measurement_required_services`分别统计实际关联及待测的物理服务次数；对象种类由objects/coverage统计，不再维护同义的units计数或旧字段别名。

Python 的 predict/profile/sample/observe-target 准备入口显式只读挂载 project、配置、请求文件和必要采样文件；通信准备同时挂载声明测量的 input 与 device_measurements。路径按声明文件解析，仓库外文件及宿主绝对路径保留其名称。直接 C++/Compose 命令使用容器命名空间，其仓库外输入由调用方显式挂载；原生模型仍须在部署环境可读，文件准备成功不代表模型执行成功。

项目使用固定模型、configurations、request_workloads 和 assets.templates；完整字段见[输入合同](02_user_contract.md)。plan 不采集、不加载编译资产；predict 共享已有资产，缺兼容来源时按明确授权与预算选择代表采集。空资产且禁采会返回缺数据；空资产并允许采集的全流程示例会进入建库，两者不能混为一谈。

有合格模板时，`predict` 同时生成性能需求。`assets/catalogs/performance-objects_<index>.json` 按实际内容跨轮复用来源对象与已有参数关系；各组合的 `performance_requirements.plan.catalog` 指向该文件，目标包络和实际查询覆盖保留在组合结果中，无需开启完整图诊断。没有成本模型或给定值时仍生成前置规划并汇总已装配前沿，后续动态路径不填零推进。

Python 编排调用 `prepare-performance --project FILE --prediction-result FILE`，归并实际缺数据需求；当前源码将未完成运行的计划保存在 `results/sampling-plan.json`，由 `run.json.performance_plan` 引用，完成后撤下引用并清理。计划通过适配接口构造现有采样域，先核对声明测量及候选来源的独立工作域元数据；准备只读取 manifest 和 input，不读取 trace 或样本时延。允许采集时沿用父任务预算、原生生命周期、独立拟合及受影响配置重算。该逻辑已在一键示例的局部重算与终态清理中核对，见[审查记录](../redesign-workflow/history/2026-10-16_code_cleanup_review.md)。

采样点按实际原生请求配方消冗，保留全部需求归属和各自特征覆盖条件，不能因共用配方就宣称已采到全部目标工作。合并结果和状态发布后，已有代码清理成功交接的预测/拟合工作目录，保留共享目录及正式候选；终态恢复可继续清理。原完整重跑曾依赖人工收尾，新逻辑目前只有导入资产的局部验证，失败、原生处理和多组合引用仍须按专项计划复核。

代表性建库按同部署配置组选择最大请求池、同池最大Prefill预算候选，运行固定短请求配方。predict先尝试显式资产，只为缺兼容来源的组在base授权及累计预算内采集一次，随后导入生成库重新预测；不采集目标workload，不因规则或成本缺口补采。节点成本候选可条件消费，资格与完整覆盖仍以实施计划为准；详细恢复行为见[集合流程](07_integration_workflow.md)。显式采集入口见下文。

### 全流程示例与独立对照

以下入口已在源码中接通，但完整新采集验收尚未完成；各项运行证据见专项审查。

```bash
example/sglang_qwen3/run.sh --model /models/Qwen3-32B --devices 4,5 --image-prefix YOUR_PREFIX
example/sglang_qwen3/run.sh --resume data/YOUR_RUN --image-prefix YOUR_PREFIX
```

前置模型、设备、配套镜像及默认预算见[示例说明](../../example/sglang_qwen3/README.md)。脚本调用正式 `explore`，每次新运行在 `data/` 创建时间根，先读 `results/summary.md`。单独 `predict` 只做预测；`explore` 还会在所有组合预测完成后固定资产，逐组合取得独立 ports 并生成 `comparison.json`。预测仍缺数据时保留采样计划与可修订状态，不进入验证；当前已取得空资产原生链、验证预算失败及终态恢复证据；资格限短示例范围。

对已有预测和独立观测，可单独调用已实现的正式对照入口：

```bash
scripts/markov.sh compare --run data/YOUR_RUN \
  --validation-manifest path/to/first-validation_manifest.json \
  --validation-manifest path/to/second-validation_manifest.json
```

对照只读已有预测与 `role=validation`、`purpose=ports` 的观测，核对输入、端口与时钟。已有两配置对照/摘要有局部证据，不能证明新验证编排完整。无观测不伪造成功对照，不可比较项说明原因；偏差很大也原样保留。`explore` 的终端打印运行与结果路径，完整 JSON 在正式文件中。

## 3. 修改后的必要检查

按[代码与工程约束](engineering_constraints.md#1-简洁有序的代码风格)审阅 C++、Python 的块内排版：逻辑步骤之间适当留空行，重要的身份、时序和生命周期约定有简短注释。clang-format 和 Ruff 通过不代表这项可读性要求已满足。

普通修改只检查受影响文件；下例为整体审查时的只读格式检查，在容器内使用现有 `.clang-format`，不修改该配置：

```bash
docker compose -p markov-next-redesign -f docker/compose/inference.yml run --rm --no-deps modeling bash -lc \
  'find src/markov/include/markov src/markov/src -type f \( -name "*.hpp" -o -name "*.cpp" \) -print0 | xargs -0 clang-format --dry-run --Werror'

bash -n scripts/build.sh scripts/markov.sh

git diff --check
```

JSON 用 jq empty 检查正式示例。输入行为通过公开 plan/predict 检查，临时修改输入只在 data 或 /tmp 中，不维护夹具和验证脚本。较大实现变化以新 build 目录做 clean Release；模拟时序变化使用真实资产在公开入口验证。Release、Debug 构建和命令退出均不能替代阶段验收或精度评价。

缺 modeling 镜像时按本节命令构建；推理镜像安装入口见[框架路径](framework_paths.md)。构建命令不包含 Docker 系统、历史资产或原仓库的清理。

### 队列 ABI 与镜像

当前 transfer 采样使用 `markov-transfer-20261016-sglang:ubuntu22.04`，公开命令设置 `MARKOV_IMAGE_PREFIX=markov-transfer-20261016`。该镜像已成套重编译 torch_npu 队列头文件与原生观察库；真实采样、独立评价证据及资格以实施计划为准。

`markov_native_queue_begin(correlation_id, worker)` 是当前两参数入口。历史 `markov-enqueue-20261014`、`markov-worker-20261013`、`markov-scope-20261014` 镜像只保留当时的安装/观测资格，不能直接用于当前 transfer 接口。`NativeAttempt` 构建 preload 库不会重编镜像内 torch_npu；更换环境须核对真实 `markov_workspace_build.json`，将队列头文件和原生库成套重编，不只换 preload、不猜测 ABI、不保留兼容入口。

## 4. 显式诊断采集

```bash
scripts/markov.sh profile --project configs/redesign/sglang/project.json --configuration default --workload example
scripts/markov.sh profile --project path/to/project.json --configuration REPRESENTATIVE_ID --representative
scripts/markov.sh observe-target --project path/to/project.json --configuration CONFIG_ID --workload WORKLOAD_ID --budget-sec 600
scripts/markov.sh observe-target --project path/to/project.json --configuration CONFIG_ID --workload WORKLOAD_ID --budget-sec 600 --purpose structure
```

profile读取选定配置和请求文件，使用collection.base的显式授权及累计预算；禁采时零attempt。SGLang普通profile/observe-target按相同arrival_time_ns批量提交预分词请求，不同到达组并发按计划提交；响应标记batch_response，不将组返回时刻当作逐请求完成。此命令是诊断采集，不是普通用户必须选择base的产品流程。KT使用同样的选取参数，服务部署取自所选配置，已有采集能力保留。

profile --representative仅接受plan选出的代表配置，不能同时传--workload。按实际页大小生成短请求，每组批量提交，等真实返回后再提交下一组；沿用collection.base预算，禁采时零attempt。配方及尚未覆盖机制见[实施计划](04_collection_templates.md#代表短请求候选与执行前门槛)。这是显式诊断入口；predict 的分组建库触发和停止条件见[集合流程](07_integration_workflow.md)。

observe-target使用独立预算生成role=validation的观测，不能作为预测模板或成本。默认ports只记录原生提交/发布返回，不构建或加载结构观察库；structure启用完整profiler和语义插桩。两者共享统一预热、原请求序列、设备租约及后处理生命周期，差异见[评价边界](06_performance_execution.md#6-输出与误差)。目的保存在prepared及产物中，resume不能更改。status/resume恢复已有任务不重置预算，原生容器存活时不能启动第二次采集或修改预算。首次部署与采集前的原生路径条件见[框架路径](framework_paths.md)。

预测宿主中断不等于模拟容器结束；prediction_live依据Docker状态。活跃预测的资产/预算修订返回requested_changes_applied=false；容器结束后恢复接收同轮结果。

## 5. 已有 base 的 source 提取

根据变化位置选择最小的重新处理步骤：

| 变化 | 使用的来源与入口 |
| --- | --- |
| 原生记录解释、归一化字段 | 用原捕获明确保留的文件重新规范化，再 `extract`；缺失事实不能补造 |
| 来源节点、依赖、语义切口 | `extract` 从规范化事实重新生成来源图、源实例及编译资产 |
| 完整来源上的绑定、参数规则、编译表示 | `compile-templates` 复用来源图/实例；这些派生文件已清理时转入 `extract` |

```bash
scripts/markov.sh extract --base-manifest data/path/to/profile_manifest.json
scripts/markov.sh compile-templates --base-manifest data/path/to/profile_manifest.json
```

输入必须为 `role=base` manifest，显式引用 source facts、配置和部署；原生文件清单见[来源交接](04_collection_templates.md#原生输入清单与重新处理)。可在派生目录的 manifest 中用绝对路径引用已有输入，不修改只读基线。`extract` 和 `compile-templates` 已接受 `--output-directory DIR` 指定编译资产目录、`--work-directory DIR` 指定中间目录；省略时使用 manifest 所在目录。直接 C++ 命令的路径属于容器命名空间。

`extract` 生成 `source_graph.json` 与 `templates.json`；只有无环检查通过才发布 `source_dag` 并编译 `compiled_templates.cbor`。重复提取替换本阶段 `extraction_gaps`。编译前撤下旧 `compiled_templates` 引用，成功后发布新引用；`diagnostics=false` 时再撤下图/实例引用，仅清理属于本次工作目录的派生 JSON，不删除借用的外部文件。`template_compilation` 的时间和大小只代表编译步骤。

当前事实重建取得同字节 CBOR，输入发布、活动任务、宿主丢失与终态恢复已核对，具体范围见[专项审查 O3/B7/B8](../redesign-workflow/history/2026-10-16_code_cleanup_review.md#确认问题与关闭依据)。后续改动不能自动沿用较早的等价结论。

预测直接消费 manifest 和编译资产，不重新解释原始图；缺资产返回 `needs_data / compiled_templates_missing`。来源事实不完整、有环或必要身份/依赖未闭合时保留缺口，不通过兼容字段、默认值或重新采集掩盖。旧内部格式通过以上入口明确重建，不维护旧版读取器。成本标签删除、规则表和角色绑定等当前资产合同统一见[编译资产](04_collection_templates.md#编译资产与预测加载)。

## 6. 独立性能采样

当前采样只接受 `transfer`、`device`、`communication`，默认 `transfer`，没有 CPU 服务采样模式。范围与近似只维护在[性能合同](06_performance_execution.md)。

```bash
scripts/markov.sh sample --project path/to/project.json --configuration CONFIG_ID --workload WORKLOAD_ID --kind device --measurement-requests work-domains.json --repetitions 3
scripts/markov.sh sample --project path/to/project.json --configuration CONFIG_ID --workload WORKLOAD_ID --kind transfer --measurement-requests work-domains.json --repetitions 3
scripts/markov.sh sample --project path/to/project.json --configuration CONFIG_ID --kind communication --repetitions 3
```

transfer/device 必须显式提供非空请求域；字段及对象一致性见[采样合同](04_collection_templates.md#独立性能采样合同)。`sample` 共用 performance 累计预算、容器锁和 status/resume，在常驻模型上用短请求测量，不重放完整目标生命周期。原生 scheduler 组批，`SamplingDomain` 核对各 rank 的实际工作、完成成员和预热，不能以客户端 batch 或记录数替代覆盖。

模型批次域、过滤选择与 `pool_slots` 的构造及适用条件见[采样合同](04_collection_templates.md#从请求构造到特征覆盖)。正式需求规划负责生成配方，独立观察核对实际成员、槽位、工作特征和完成情况；配方提交成功不代表所需节点已被测量。

SGLang结构采集、端口观察及性能采样的统一预热，经`SGLangSession._reset`等待原生idle后才返回；性能采样在各次正式重复前、每个点结束时复用同一方法。它在Engine既有event loop中调用`tokenizer_manager.flush_cache(timeout_s=permission.max_wall_time_sec)`，由原生scheduler等到fully idle后清理，并检查返回对象的success/message。`Engine.flush_cache()`不传timeout，会在尚未清理上一批时立即返回false，不作为这个等待接口。原生timeout不替代`NativeAttempt`的剩余总deadline，不新增计时器或请求状态。初始化资源要求见[固定预热合同](02_user_contract.md#仿真起点统一预热后开始)；收尾不归目标成本，base组间语义确认及ports最终发布确认保持各自职责，transfer仍只接收实际完整queue/driver配对。

device 路径通过 profiler、步骤归属和原生 API 取得计算、异步搬运、设备命令和通信物理任务，发布 `device_measurements.json.gz`；数据相关节点还需实际长度、行索引、请求池访问及批次过滤选择。transfer 路径关闭 profiler，只用原生批次范围与 queue 身份取得完整成功 driver 复制，发布 `cpu_worker_units.json.gz` 的 `transfer_units`。其 manifest 延用 `native_serving_timing`，只拟合 I/O；准备、worker CPU 与前置同步等待不作为传输样本。transfer 命令使用[配套部署镜像](#队列-abi-与镜像)，采样成功和训练候选可用不授予精度资格。

communication 使用独立 HCCL 工作域，当前支持 Ascend TP≥2、每 rank 一设备的 BFP16 allReduce/allGather；不加载模型权重。各项先预热再重复，测量同步只作完成隔离；`collective_measurements.json.gz` 保存物理 `task_samples`，父包络和启动错峰不收费。

产物以 role=performance manifest 显式引用输入、环境、样本和覆盖，不进入 extract。trace/CSV 支持无损压缩，base/structure 事实以 gzip JSON 发布。`CaptureProcessing` 用 `normalized.json` 收据继续处理，资产发布后释放所属 vendor 导出及已归入事实的原始 journal；诊断模式保留原始包。journal 定位符继续用于来源身份，`raw_exports=released` 后不能当作可重放文件。当前一键运行已核对新原生发布与清理；base 释放原始包后仍能从保留事实重建同字节资产。导出量、保留量和峰值分别记账，证据见专项审查。旧 CPU 资产不改写，旧采样/模型入口不兼容。

### 从独立单元样本拟合候选

拟合入口（省略 `--unit` 时为 `all`）：

```bash
scripts/markov.sh fit-costs --performance-manifest path/to/performance_manifest.json --unit kernel > candidate-model.json
```

| unit | 现有输入与计时边界 |
| --- | --- |
| kernel | 独立计算 span 的公共工作描述、张量/启动条件及必要操作数；排除预热，不按 request/source span 身份拟合 |
| transfer | 异步设备服务或完整同步 host driver 本体；执行方式、方向和字节组成工作键，CPU 准备及前置等待不并入传输费用 |
| communication | 已关联的物理通信任务与实际工作量，不叠加 collective 父包络或等待 |
| command | 物理设备命令类型，不使用 CPU 提交包络或 EVENT_WAIT 代替 |

候选使用同一工作合同及 `MeasuredCostFitter`，保留真实来源、部署、观测缺口和适用条件。kernel 当前仍为物理 span 的经验基线，只有已有生产合同允许的张量遍历投影或真实语义操作数才可查询；未匹配描述不外推。传输样本按 `execution` 区分异步设备与同步 driver，不能因同步记录来自 `cpu_worker_units` 就遗漏。重新编码或归一化同一份 raw 不形成独立留出。

`fit-costs` 仅接受 `all|kernel|communication|transfer|command`。`all` 从 device manifest 拟合四类工作，从 collective manifest 拟合 communication，从 `native_serving_timing` 仅拟合已声明的 `transfer_units`；不解析或加载 CPU 服务候选，不保留旧选项兼容。

公开 `predict` 将显式 `assets.performance_manifests` 交给独立拟合进程，持久化候选后供组合共享。拟合与预测各自持锁，活动拟合不接受资产修订，中断复用原容器；候选为空保留缺口，不通过重采保证成功。当前已贯通的真实 transfer 拟合与无 given 预测，不代替独立目标对照或单元留出；阶段证据见实施计划。

固定候选的单元评价入口继续可用：

```bash
scripts/markov.sh evaluate-costs --model-candidate candidate-model.json \
  --performance-manifest path/to/held-out-performance-manifest.json
```

评价复用工作匹配，只读候选与独立样本，报告匹配/未匹配、排除原因、误差及上下文差异，不拟合、不补采或回写候选。同源文件不能作为留出；零样本或无意义比率为 null。它是单元评价，不是整次请求预测的独立评价，资格仍为 `not_established`。

已生成候选通过 `assets.cost_models` 声明，或在尚未固定验证的任务中用 `resume --run DIR --cost-model FILE` 替换完整列表；当前按模型、框架、部署、适配关系及实际工作查询，仍保留条件性质。单元评价与[整次请求对照](#全流程示例与独立对照)是不同入口；后者注明省略的 CPU、模板外工作与观测影响，不减残差构造纯设备真值。

## 7. 条件预测与恢复

`assets.templates` 的每项包含 manifest 和可选 `node_durations`；给定耗时绑定同一来源，只用于条件结构执行。预测容器只读挂载声明的 manifest、编译模板、给定耗时和候选，不挂载 vendor trace、source facts、性能时延样本或 validation。模板与成本只读共享，控制器、KV/资源状态及模拟时钟按配置 × workload 隔离。

`diagnostics=false` 时仅生成组合摘要；开启时写 `combination_N_graph.json` 与 `combination_N_graph.details.json`，由 `structure.graph.artifact`、`execution_details.artifact` 引用。终端和恢复不读取完整图。details 包含共享的 `source_definitions` 和动态 `work`：调用内的 `templates[].source_definition` 引用来源下标，只保存目标参数、连接、资源及变换；通信目标条目通过 `source_collective` 引用该定义内的通信项。来源只在本组合诊断中发布一次，不随重复调用复制。旧诊断数组格式不保留兼容读取器。

结果追溯使用 `structure.graph.sources`、`source_manifest` 和来源内的 span/call 身份；`provenance.source` 只是首选来源。加载/编译墙钟是软件处理成本，不是目标节点耗时。初始化必须有真实统一预热与 workspace 记录，缺失返回资格缺口，不能默认空缓存；合同见[仿真起点](02_user_contract.md#仿真起点统一预热后开始)。

### 恢复与失败定位

```bash
scripts/markov.sh status --run data/path/to/run
scripts/markov.sh resume --run data/path/to/run
scripts/markov.sh resume --run data/path/to/run --template-assets template-assets.json
scripts/markov.sh resume --run data/path/to/run --cost-model candidate.json
```

- `--template-assets` 替换完整来源列表，数组项与项目合同相同、路径相对该文件；`--cost-model` 替换候选列表，可重复传参。显式替换资产会完整重算；改变模型、配置集合或请求序列须新建任务。
- 输入修订与结果失效先写入同一运行账本，短暂的 `pending_input` 记录尚未交接的输入。恢复先完成输入发布并移除失效结果，再继续；成功后删除该临时字段，不保留多套输入版本。
- `explore` 一旦进入独立验证，预测及其资产固定；此后只允许继续验证或明确调整 validation 总预算，资产替换须新建探索。预测未完成时仍允许资产/预算修订；这一最新分支尚未完成运行验证。
- 无输入修订的终态恢复只读已有结果，不增加轮次。自动建库成功只重算受影响配置组；失败没有新资产时复用原结果继续选组。状态和预算属于原任务，不因宿主中断或恢复重置。
- 宿主命令中断不等于容器结束。活动预测/拟合依据 Docker 实际状态恢复，活动期间的修订返回 `requested_changes_applied=false`，不启动替代任务。
- 原生登记后尚未启动时可沿同一 attempt 继续，宿主暂停不计入 bootstrap；存在活动容器或 launcher 时不替代启动。确定的启动失败按 `launch_result.json` 计量。流程保留原生容器用于恢复；若外部强删已启动容器且无终态证据，仍明确报告无法恢复计量。
- 后处理的确定失败读取 attempt 的 `processing_directory` 所指工作目录中的 `processing_failure.json`。原生 completed 与处理 failed 分开，`resume` 不自动重采或重处理确定失败；没有失败收据的宿主异常须先核对真实进程状态。

修正后处理原因后，可在 modeling 容器中显式重处理保留的原捕获，再通过 `resume --template-assets` 导入成功资产：

```bash
PYTHONPATH=src/collection python3 -m markov_collection.workflow.processing \
  --capture path/to/native_capture.json --role base
```

该入口持有处理锁时清除旧失败记录并复用原输入，不是新的原生采集。可用 `--directory` 指定原处理收据目录，`--template-directory` 指定编译资产位置，`--work-directory` 指定来源中间目录；正常恢复以账本登记路径为准。若原始导出已释放，不能从 `native_capture.json` 重新获取缺失原始字段；现有规范化事实的重提取使用前述 C++ 入口。集合合并、隔离及恢复规则见[集成合同](07_integration_workflow.md)。
