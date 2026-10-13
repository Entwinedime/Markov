# 当前构建与运行

本页维护可执行入口及其实际限制，主线为 SGLang + Qwen3-32B。阶段状态和开发是否暂停只见[实施计划](../redesign-workflow/2026-10-08_redesign_execution_plan.md)。README 按约定保持为空。

预测组合入口由 CLI 注入 `TemplateAdapter`；适配器解释来源配置、选择合格模板并提供公共控制限制，PredictionWorkflow 负责资产/成本隔离和组合执行。采集计划仅作为报告传入。Planner 的控制路径判断与配置分组已共用该接口；采集准备与配置序列化通过独立的 `CollectionAdapter` 注入 SGLang/KT 实现，保留 KT 仅采集的能力边界。提取/编译装配复用 TemplateAdapter；workflow 中的 ProjectReader 负责公共集合输入，具体框架参数解析通过 CollectionAdapter 注入。公共 JSON 输入工具集中在 contracts/json_input，框架配置值类型保留在输入合同中。

公开入口已接通集合输入、显式模板资产、给定节点耗时下的动态执行、报告和 status/resume。代表短请求可通过显式 profile 执行；predict 的代表性自动建库、受影响组合重算及正式节点成本闭环尚未完成，不能把历史自动补采入口描述当作当前能力。KT 原生采集保留，预测闭环为后续扩展。

模板 source_state 需要真实 worker 准入策略；缺少观测时返回 needs_data / admission_policy_not_observed，不手填原生默认值绕过。原完整输出预约及整批联合成本后端已删除。当前执行始终结构先行、成本后置；给定耗时结果不代表真实性能精度，机制见[准入策略](sglang_template_relations.md#准入策略与内存压力)。

现有独立采样的原生 workload/HCCL 条件由 adapters/frameworks/sglang/SGLangSamplingRecipes 准备；Planner 负责分派与公共权限、输出封装。prepare-sampling/prepare-communication 只准备输入，不执行设备任务；设施保留不代表 S5 节点成本模型已实现。

## 1. 工具链与构建

在仓库根目录执行：

```bash
docker compose -p markov-next-redesign -f docker/compose/inference.yml build modeling
scripts/build.sh
```

使用既有 Ubuntu 24.04 modeling 环境，C++23、语言扩展关闭，CMake≥3.20、Ninja 与镜像已有 nlohmann-json≥3.11。当前编译器为 GCC 13.3，CMake 3.28.3、libstdc++ 13.3、JSON 3.11.3；clang-format 18 用于格式检查。原 clang 18 与 libstdc++ 13 的组合未开放本次实际需要的 std::expected，因此镜像改用已有 GCC，没有添加旧标准回退。正式 target 为 markov_core、markov_adapters、markov_workflow 和 markov；默认 Release 产物位于 build/markov，未加入 CTest 或独立测试 target。已有构建目录切换编译器后须重新配置 Release，不沿用重置后的默认类型。

脚本固定 Compose project 为 markov-next-redesign，规避本机已有的重复 markov-next_default 网络，不改变其他网络或镜像前缀。编译、执行及 C++ 静态工具均在 modeling 容器内，宿主不作为验证工具链。

### C++ 目录与职责

`src/markov/include/markov/<职责>/` 与 `src/markov/src/<职责>/` 对应放置声明和实现；`cli` 仅有实现。不保留旧路径转发头文件。目录表达流程职责，框架与运行环境是适配层内的两个维度。

```text
src/markov/{include/markov,src}/
├── contracts/                         # 公共值合同及共用 JSON 输入校验
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
├── performance/                       # 给定耗时、CPU 经验模型、统一节点成本查询
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

`templates/template_topology.cpp` 集中既有拓扑 mutation 和 donor 插入；`templates/template_assembly.cpp` 集中实例身份与跨批资源前沿拼接。模板定义与候选选择仍在 templates.cpp，不以文件拆分增加新类或转发层。

构建依赖为 `markov → markov_workflow → markov_adapters → markov_core`。core 包含 contracts、graph、公共 production、templates、controller、execution 和 performance；核心源文件不包含 adapters/workflow 头文件，适配层不反向调用 workflow。三个库 target 对应实际职责边界，不为每个 block 建库。

`contracts/project` 仍包含用户可选择的框架配置值类型，这是输入合同；控制器与执行核使用 `ServingLimits` 等统一值，不读取原生框架对象。Qwen3/Ascend 模块位置、kernel 名称、原生 hook 和 HCCL 实现规则属于适配端。模型原生布局目前仍是 SGLang + Ascend 支持域，不宣称是任意框架的通用 Qwen3 适配。

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
│           ├── pytorch/               # ProfilerMarkers 的 PyTorch 实现
│           └── ascend/                # 环境、导出/规范化、HCCL 测量
│               └── torch_npu/         # profiler 配置与 workspace 快照
└── adapters/                          # 镜像内构建所需的原生适配资源
    ├── frameworks/{sglang,ktransformers}/ascend/  # 安装及框架/Ascend 组合适配
    └── runtimes/ascend/
        ├── native/                    # 单独构建的 CANN SDK 拦截库
        └── torch_npu/                 # workspace patch、观察头文件、安装脚本及适用说明
```

公共 instrumentation/artifacts 不导入具体框架或运行环境。`Recorder` 接收 `ProfilerMarkers`，负责身份、嵌套、标量和 CPU 计时；PyTorch 实现提供 marker。已有 native journal 的标量 C ABI 仍是记录器与 native 插桩库的交接，SDK 类型和拦截符号留在 Ascend native 目录。

`CaptureSession` 统一准备输入和进程失败生命周期；SGLang/KT 实现原生执行。workflow 选择框架并装配 Ascend 产物发布，runtime normalizer 不导入框架采样程序。`SamplingDomain` 属于共享产物合同。框架适配器可以组合环境适配器，运行环境层不得反向依赖框架层。CPU/设备/通信测量都使用同一个 Ascend 产物发布者，在执行前记录日志偏移。

Python 负责观察原生运行并写入显式文件；C++ 负责来源关联、子图提取、模板生产和目标展开。共同交接是 block/region 身份、父子关系、节点/依赖、参数、端口和来源，不传 Python 对象到 C++，也不复制两套流程定义。主模块内部路径已更新，公开 `scripts/markov.sh` 命令不变。

Dockerfile 选择具体组合、复制适配目录并调用 `docker/install`；该目录只保留安装调用入口。原生版本约束、框架源码适配、探针补丁及构建步骤由 `src/collection/adapters` 拥有。Python 包中的同名目录负责运行期间的观察与解析，两者共同实现同一个适配职责，不通过目录扫描动态注册。当前组合及安装归属见[适配组合](framework_paths.md#适配组合与源码归属)。

workspace 的原生记录通过 `adapters/runtimes/ascend/workspace_bindings` 绑定为公共 `TemplateWorkspacePool/TemplateWorkspaceUse`，其中操作/worker 和局部标记边界保留来源关系，初始容量只在资源级保存。`TemplateAsset` 保存这些类型及有限 `WorkspaceDemandLayout`；模板实例在拓扑变换后按实际 marker 绑定 workspace_uses，再按目标 BatchInput 计算需求；明细 workspace_demands 保留来源 manifest/call、使用点索引、目标局部边界、work_units 和 bytes。`TemplateBank` 报告来源绑定、demand_rules 数量与 partial 状态。执行层 WorkspaceState 独占容量，按目标拓扑的实际依赖顺序计算分配分支；SimulationRuntime 暂存整批变化，图装配成功后提交一次，重调度不重复推进。分支不匹配已进入 TemplateBank 的 donor 规划，allocator 与语义区域共用 plan_subgraph_replacement；插入wait在全部变换后按实际worker提交和依赖可达性绑定目标动作内前沿，已有四请求开发序列完成grow及后续复用；插入节点中的再次替换、其他提交线程和跨配置初值仍未资格化；框架和运行时解析不进入模板消费者。workspace需求布局现由TemplateSource的workspace_rules集合按完整参数值共享，使用点保存已确认规则的只读借用引用或生产端缺口原因，不另存有效性标志；codec复用AssetReferences写表索引，并先恢复规则所有者再恢复使用点。报告demand_rules仍统计已绑定使用点，demand_rule_definitions统计独立定义，demand_gaps按具体原因统计需要需求但未绑定规则的使用点（release不计入）。新增字段后需要正式 `compile-templates` 重建资产，不读取旧资产字段的默认值。

attention 参数使用相同的来源内规则表机制：`attention_rules` 保存完整参数规则，定义中的 `attention_kernel_layouts` 保存引用和来源注释；报告 `attention_parameter_rules` 分别列出 definitions 和 observations。该资产合同变更也须执行 `compile-templates` 重建，不提供旧格式回退。

原生采集清单现显式包含 `profile_controls` 文件列表，归一化后的事实保留运行环境启停记录和限定的活动归属。重处理历史原始数据前须由当前产物发布器重新生成清单；没有记录时列表为空，不补造历史启停时间，也不自动猜测旧字段。已编译模板的直接加载不需要重新读取这份清单。

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

独立任务目录使用日期时间加 UUID 命名，Docker 容器名从任务目录及轮次派生；跨输出目录的任务不能仅以时间戳区分。恢复继续使用已有任务身份，不因观察中断生成新任务。

项目使用固定模型、configurations、request_workloads和assets.templates；完整字段见[输入合同](02_user_contract.md)。plan不采集、不加载编译资产，逐组合说明声明范围；predict共享已有编译资产，逐组合动态执行，结果无隐含base比较。示例资产为空，因此返回缺数据；不是可用性能预测结果。

代表性建库按同部署配置组选择最大请求池、同池最大Prefill预算候选，运行固定短请求配方。predict先尝试显式资产，只为缺兼容来源的组在base授权及累计预算内采集一次，随后导入生成库重新预测；不采集目标workload，不因规则或成本缺口补采。节点cost model尚未实现，详细恢复行为见[集合流程](07_integration_workflow.md)。显式采集入口见下文。

## 3. 修改后的必要检查

按[代码与工程约束](engineering_constraints.md#1-简洁有序的代码风格)审阅 C++、Python 的块内排版：逻辑步骤之间适当留空行，重要的身份、时序和生命周期约定有简短注释。clang-format 和 Ruff 通过不代表这项可读性要求已满足。

仅对修改的源文件操作；以下工具在容器内使用仓库 .clang-format：

```bash
docker compose -p markov-next-redesign -f docker/compose/inference.yml run --rm --no-deps modeling bash -lc \
  'find src/markov/include/markov src/markov/src -type f \( -name "*.hpp" -o -name "*.cpp" \) -print0 | xargs -0 clang-format --dry-run --Werror'

bash -n scripts/build.sh scripts/markov.sh

git diff --check
```

JSON 用 jq empty 检查正式示例。输入行为通过公开 plan/predict 检查，临时修改输入只在 data 或 /tmp 中，不维护夹具和验证脚本。较大实现变化以新 build 目录做 clean Release；模拟时序变化使用真实资产在公开入口验证。Release、Debug 构建和命令退出均不能替代阶段验收或精度评价。

Docker 镜像本机曾被其他来源 untag/delete，发起者未知；缺镜像时按上述命令恢复。不自动清理 Docker 系统、历史资产或原仓库。

当前本机 workspace 观测使用已安装探针的 `markov-workspace-20261012-sglang:ubuntu22.04`，公开采集命令须显式设置 `MARKOV_IMAGE_PREFIX=markov-workspace-20261012`（同前缀 modeling 镜像也已存在）。默认 `markov-next-sglang:ubuntu22.04` 尚无该探针安装记录，不能用于要求完整 workspace 分配证据的采集。源码更新不会更新已构建镜像；更换环境时先读取实际镜像内 torch_npu 包的 `markov_workspace_build.json`，核对安装提交和探针覆盖范围。旧安装记录保留安装时的源路径，不改写历史来源。

### 恢复实施

当前任务、顺序与阶段门槛统一见[实施计划](../redesign-workflow/2026-10-08_redesign_execution_plan.md)，此处只维护运行与部署方法。

## 4. 显式诊断采集

```bash
scripts/markov.sh profile --project configs/redesign/sglang/project.json --configuration default --workload example
scripts/markov.sh profile --project path/to/project.json --configuration REPRESENTATIVE_ID --representative
scripts/markov.sh observe-target --project path/to/project.json --configuration CONFIG_ID --workload WORKLOAD_ID --budget-sec 600
```

profile读取选定配置和请求文件，使用collection.base的显式授权及累计预算；禁采时零attempt。SGLang普通profile/observe-target按相同arrival_time_ns批量提交预分词请求，不同到达组并发按计划提交；响应标记batch_response，不将组返回时刻当作逐请求完成。此命令是诊断采集，不是普通用户必须选择base的产品流程。KT使用同样的选取参数，服务部署取自所选配置，已有采集能力保留。

profile --representative仅接受plan选出的代表配置，不能同时传--workload。按实际页大小生成短请求，每组批量提交，等真实返回后再提交下一组；沿用collection.base预算，禁采时零attempt。配方及尚未覆盖机制见[实施计划](04_collection_templates.md#代表短请求候选与执行前门槛)。这仍是显式诊断入口，predict尚不会自动发起建库。

observe-target使用独立预算生成role=validation的观测，不能作为预测模板或成本。status/resume恢复已有任务不重置预算，原生容器存活时不能启动第二次采集或修改预算。首次部署与采集前的原生路径条件见[框架路径](framework_paths.md)。

预测宿主中断不等于模拟容器结束；prediction_live依据Docker状态。活跃预测的资产/预算修订返回requested_changes_applied=false；容器结束后恢复接收同轮结果。

## 5. 已有 base 的 source 提取

CPU API归一化也关联设备同步：`rtDeviceSynchronize`和`rtDeviceSynchronizeWithTimeout`按完整同线程序列对应`Runtime@DeviceSynchronize`，数量或已知scope不一致时保持未关联。关联只恢复调用归属，不生成硬件任务。提取器现按显式stream/device范围恢复提交前沿，CPU服务沿既有路径切除等待；模板存储、资源绑定和执行明细共用该范围。设备域存在未映射活动或重叠提交时保留未解析等待，不能宣称设备级动态展开已验收；workspace分支触发及完整资源域仍待落实。内部资产需从原始事实重提取，不兼容旧的无scope存储。 workspace需求探针已完成隔离构建和ATB分配/扩容/复用/清空运行验证，证据与边界见[workspace合同](sglang_template_relations.md#workspace-容量状态与设备级同步)。SGLang Dockerfile经`docker/install/torch_npu_workspace.sh`调用runtime适配器的`install.sh`，构建固定提交的带探针原生库，保留匹配wheel的Python扩展；安装脚本和独立验证镜像的实际库加载检查已通过，当前cached/non-overlap代表请求的serving窗口链已通过验证；完整Dockerfile重建仍未验收。`MAX_JOBS`构建参数控制并行度，默认16。采集环境的`workspace_probe`记录包内构建来源，未安装时为null；现有镜像不会因源码修改自动获得探针，诊断scope也不构成完整serving模板。

Ascend native journal同时保留 `rtEventQueryStatus` 的event handle、返回码和 `event_status`（失败为null）。这是原生查询的观测，不发起额外查询或同步；没有device提交身份。查询完成不能代替event record关联或证明预热后的后台清理为空。当前仅完成单event SDK拦截验证，后台调度与入口状态尚待资格化，见[模板关系](sglang_template_relations.md)。

native_capture 还显式提供 `event_history` 文件列表，来自 `native_api/initial_events/*.jsonl`。设置 `MARKOV_NATIVE_OBSERVATIONS` 的进程在窗口前只记录event的record/query/destroy；窗口入口标记使用单调时钟，业务journal另行保存。ServingNormalizer通过EventHistory输出 `event_states`：仅表示已观察record且未destroy的入口集合及查询事实，不等于pending设备任务或watchdog队列。跨入口/重叠调用保留缺口。旧采集重做归一化时显式声明实际存在的文件；没有历史用空引用列表并保留 `entry_history_not_observed`，不能补造空状态或使用旧字段fallback。当前完成单设备SDK入口历史验证，真实serving交接及跨边界因果消费仍须核对。

native_capture 还必须提供 communication_logs 列表，每项为明确的 rank、pid、path；无日志时使用空列表并保留配置缺口。历史捕获可引用已保留的原 worker plog 后重新归一化，不能用当前容器日志代替历史来源。源 collective 现在还要求 communicator 字段，其值为已观测初始化配置或 null，旧源图需重新生成。当前日志读取支持默认 Ascend plog 格式；日志缺失不触发重新采集或默认值回退。 communicator 还需设备类型、HCOMM 版本、按 operation 区分 AllReduce/AllGather 的 transports，以及 env_aicpu_unfold/env_aiv_mode；缺观测为 null。新增字段从已保留的原 worker 日志重新归一化，再执行 extract，无需启动原生采集；op_expansion_mode=0 本身不证明 host 展开。

native_capture 的每项 traces 必须提供 `device_info` 列表，显式引用该 worker 的 `PROF*/device_*/info.json.<设备编号>`，不包含 `.done` 完成标志。归一化保留 rank、pid、worker 可见设备 ID、cube/vector 核心数和文件来源到 `devices`，服务结构与独立采样共用此记录。设备核心数是硬件容量，不能用 kernel 的 Block Num 或同型号经验值代替；空列表保留 `device_geometry_not_observed` 缺口。历史原始捕获在派生目录补齐已有设备文件引用后重新归一化，不触发重新采集；没有旧字段回退。C++ 原生适配端现消费这些事实编译有限 workspace 需求规则，适用域见 workspace 合同。

native_capture 的每项 traces 现在必须显式引用 database，即对应 rank 的 ascend_pytorch_profiler 数据库。归一化以只读方式将 COMMUNICATION_OP 与 STRING_IDS 关联，再按 connectionId 和操作名连接 trace，保留 group_name 及数据库 opId 来源。数据库内的字符串索引不能跨 rank 比较。历史捕获若已有数据库，在明确的派生目录补齐其文件引用、重新归一化并 extract；仅重提取缺少组名的旧 source_facts 不足以恢复身份。无需因此重新运行模型，不提供缺字段猜组名的兼容路径。

```bash
scripts/markov.sh extract --base-manifest data/path/to/profile_manifest.json
```

`extract`读取role=base manifest引用的source_facts、配置和环境，生成source_graph.json和templates.json，再调用共同编译阶段写出compiled_templates.cbor。source_dag仅在图无环时发布；有环时不编译。报告中的template_compilation记录编译墙钟耗时和文件体积，不是全部后处理成本。重复提取替换本阶段extraction_gaps，不累加旧结论。

仅修改模板绑定、参数规则或编译资产表示，且已有来源图/实例字段完整时，可直接复用这些文件：

```bash
scripts/markov.sh compile-templates --base-manifest data/path/to/profile_manifest.json
```

`compile-templates`仍读取来源事实、图、实例和部署记录，但不重新构图或复制它们。可在派生目录的manifest中用绝对路径引用已有输入，编译资产写到该manifest目录。两个命令共用同一编译实现；编译前撤下旧compiled_templates引用，资产和manifest各自通过临时文件替换，成功后才发布新引用。输入缺失或不合格时直接失败，不自动重采、重提取或猜测旧字段，也不新增版本、指纹或缓存。此处不是整个任务及并发修订的事务保证。

来源记录/提取规则变化时仍必须先归一化或extract：source_facts须保留scope_call与owner_call，模板须有internal_edges；源图需要stream_waits、collectives及独立于物理device_spans的collective_spans。缺少原始语义的事实不能靠重编译恢复，缺字段不能解释为空通信。保留原生trace时可重做归一化，无须重新运行模型。成功解析与无环只证明已记录关系合法；身份、复合任务依赖或CPU服务边界未闭合时仍为needs_data，不能把模块级实例化当成完整物理依赖。

预测任务的`selecting`表示已有预测结果可复用、正在处理代表组选择。失败未产生新资产时，resume读取同轮prediction.json继续选组；成功导入资产或显式修改输入才安排新轮次。自动建库成功后，`prediction.recompute_configurations`记录原代表组的configuration_ids，下一轮project.json只包含该组；合并后的prediction.json保留其他组合。显式资产替换仍完整重算。新增持久字段要求新建当前格式任务，不兼容猜测旧未完成任务的重算范围。

原生启动命令退出后写入`launch_result.json`，保存退出码、单调时钟耗时和结束时间。Docker留下Created/Error且启动命令失败时，据此结束该attempt；resume不把恢复等待时间计入预算。旧任务若没有终态或启动记录，继续明确报告无法恢复计量，不猜测耗时或自动再采。

采集后处理的确定失败由处理进程写入`processing_failure.json`，预测任务按组隔离并保留原attempt；`resume`不会把这一失败变成自动重采或自动重处理。修复原因后，可对该attempt显式重跑原后处理入口（`PYTHONPATH=src/collection python3 -m markov_collection.workflow.processing --capture <原native_capture.json> --role base`，在modeling容器内执行），它在持有处理锁时清除上一失败记录并重用原输入。成功后通过已有`resume --template-assets`导入生成资产。没有失败记录的宿主异常仍需核对真实运行状态，不能据此重启。

编译模板保存lane、外部连接和等待的资源角色，来源报告resource_bindings列出原生编号及可选的control/submission/model/collective角色。未绑定角色不代表可跨来源迁移。此字段变更须从已有事实执行compile-templates；不支持旧资产格式兼容。

编译模板保留源图CPU服务的`control_part`，供目标成本查询使用。此字段加入后，已有源图只需执行`compile-templates`重建当前资产；无需重新采集或构图，不兼容读取缺该字段的旧编译记录。

编译模板同时保存来源共享的sequence_capacity_tokens，表示包含本次query的最大已处理序列长度；原生模型/表/缓存解释只在生产端发生。缺此字段的历史编译资产须从满足当前合同的来源重新compile-templates，不提供旧格式兼容；无需重新采集。Decode候选长度使用共享容量，Prefill候选仍限一页；整组结构和分支资格继续单独检查。

predict只挂载assets.templates中各manifest和compiled_templates所指资产，不挂载source_facts、原始DAG/实例或vendor trace。已有事实需要构图时用extract，已有完整来源只需更新绑定时用compile-templates；缺编译资产返回needs_data / compiled_templates_missing，不自动补采或回退。processing.template_load_wall_time_ns记录模拟器加载资产的实际墙钟开销，不是目标节点耗时，也不计入预测指标。 实际图根sources数组列出来源manifest，节点及边界的source_asset是该数组索引，source_span/source_boundary仍是资产内局部编号；图摘要中的ports也按其sources表限定来源；缺耗时结果同时提供source_manifest与source_span。来源身份在加载时绑定，增加这些运行时字段不要求重编译资产。

纯逻辑完成边界、CPU 包络、物理 kernel 和通信注解有不同含义。源码未绑定成本，不将 observed_time_ns 的差值自动变成服务时间。详细合同见[采集与模板](04_collection_templates.md)。

## 6. 独立性能采样

当前 CPU/device 入口启动一次原生 Engine，在同一引擎内按条件调用 `generate()`/`async_generate()`；并非每个数据点重启 serving，也未改用 `bench_one_batch.py` 或直接驱动 `ModelRunner`。语义与性能观测独立、统一适配及自动特征域规划的设计见[采样合同](04_collection_templates.md#独立性能采样合同)；这些后续能力尚未完整实现，下面的命令仍要求显式测量工作域。

```bash
scripts/markov.sh sample --project path/to/project.json --configuration CONFIG_ID --workload WORKLOAD_ID --measurement-requests work-domains.json --repetitions 3
scripts/markov.sh sample --project path/to/project.json --configuration CONFIG_ID --workload WORKLOAD_ID --kind device --measurement-requests work-domains.json --repetitions 3
scripts/markov.sh sample --project path/to/project.json --configuration CONFIG_ID --kind communication --repetitions 3
```

sample 使用 collection.performance 的 allow_run 和累计 max_wall_time_sec，关闭时返回 needs_data/performance_collection_disabled、零 attempts、零用量。kind 默认为 cpu，可选 device、communication；重复次数必须为正，默认每个测量条件三次。status/resume 与 base 共用原生容器预算、资源锁和恢复机制，base/performance 分开记账。

当前仅实现 SGLang lightweight 范围的 Ascend 路径，需要已给出的 token 输入；不支持文本解析、radix、overlap、graph、mixed prefill/decode、HiCache 或 chunked prefill。CPU模式同时按既有admission_budget hook记录完整原生调用到cpu_units目录，manifest显式列出文件、边界和未资格化原因；这些记录不同于cpu_samples中的包含式阶段测量。clock_pair_ns仅为探针量级观测，不是自动修正系数，禁止把两类样本同时收费。安装当前queue适配后，CPU模式还输出`cpu_workers`，manifest显式列出原生入队/worker记录及其needs_data原因。记录含原生任务名称/类型、pid/correlation_id、各线程时钟和已观测runtime点；不能跨线程相减CPU时钟，也不能按时间接近猜worker所属业务。采样结束后生成`cpu_worker_units.json`，manifest通过`cpu_worker_units`引用，并在`cpu_worker_coverage`报告关联覆盖与缺口。该资产共享阶段上下文，为enqueue和worker分别保存不重叠的相邻边界测量、等待/探针计数及原始行号；覆盖通过也不提升needs_data。目标工作匹配和服务/等待解释仍未完成，不能直接作为正式成本。模板资产新增按节点保存的CPU提交工作描述，旧编译资产需使用当前归一化生成的source_facts执行`extract`重建，仅重新`compile-templates`不能给旧源图补CPU工作描述；不读取旧字段默认值。CPU/device 必须用 measurement-requests 显式指定非空测量工作域；不再按目标输入长度、输出长度与批大小自动展开采样网格。缺少该参数时在创建采集任务前报错，空数组也不接受。通信采样仍从显式独立设备资产提取通信工作域。输入是用户明确提供的工作域数组（当前节点执行报告不再生成整批 measurement_requests），每项含 phase、requests 的 input_tokens/context_tokens/prompt_tokens，以及 finished_request_count。原生输入配方由 C++ Planner 生成，使用选定 workload 中同长度的真实 token 输入和新的测量身份，不运行其业务到达序列，不读取 target 实测或评分。

这是驱动原生请求的采样输入合同，不是以整批为成本粒度：已有 CPU 单元和设备节点记录可供拟合，整批条件仍不能表达所有单元特征，请求完成数也不足以区分具体结束成员。已有入口与资产保留，后续依据公共工作描述规划采样并核对实际覆盖，不恢复按 9/8、10/9 等整批组合逐项补采。

不同 decode 上下文通过独立请求的真实 token 进度分批释放形成；同组请求一起提交。生成 token、原生 scheduler 和 KV 更新保持原生行为，既不覆盖 token 也不暂停 scheduler。配方是请求的工作域，实际原生 batch 与 shape 才是测量证据；客户端释放时刻不能保证所有域都会到达。预热、完成覆盖和实际成本查询继续决定资产是否可用。

cpu 采样运行官方 Engine 的 所选配置，关闭 profiler 和原生 API 文件日志。记录 schedule 前段、prepare 本体、schedule 后段、execute 和 publish 的线程 CPU；嵌套 prepare 及其记录开销不重复计入 schedule。原生观察库单列已有 stream/event/device 同步 API 的线程 CPU、墙钟及探针 CPU，保留原始值和扣除后的非等待 active 值；没有新增同步。同步墙钟用于解释等待，不能当作服务。

每个测量条件前先清空原生请求/cache 状态并执行预热，预热和正式测量各有声明身份。SamplingDomain 按实际 rank、阶段、batch 和每请求输入/context/prompt/generated 长度核对先前预热覆盖，不根据客户端 batch 大小认定成功。第三轮 CPU 采样已实际覆盖全部测量条件：128 条预热、384 条测量记录，missing_conditions 为空。首次 B=2 的约 2.86 秒 prepare 留在预热中，正式测量为约 2.7–2.9 毫秒；没有删除样本或添加时延补偿。预热中的长延迟几乎不在已拦截的 SDK 同步 API 内，具体初始化来源尚未确认。

device 采样复用相同测量条件和预热规则，启用框架 profiler 及原生 API 观察，单独保存硬件计算/传输 span、shape/dtype/layout、业务工作量、精确来源和可关联的传输字节/方向。已完成真实采集：169184 条记录，正式测量计算 76776 条、传输 50112 条，预热域无缺失。归一化只生成 device_measurements.json 与 role=performance manifest，不生成 source DAG 或语义模板。通信汇总保留为非叠加观测，compound transport 包络不直接当作通信服务；本模式的 profiler CPU 时间不作为 CPU 成本。实际来源和缺口见[执行记录](../redesign-workflow/2026-10-08_redesign_execution_log.md)。

communication 需要 assets.performance_manifests 显式引用同一 SGLang TP/设备部署的独立 device 测量。准备阶段从原生 collective_descriptors 提取操作、元素数、dtype、算法和来源；当前实现 BFP16 allReduce/allGather、Ascend HCCL、每 rank 一个设备且 TP≥2。采样不加载模型权重，每项先预热再重复测量；同步仅用于独立 benchmark 的完成隔离，未插入 serving 捕获。collective_measurements.json 保留逐 rank 原始时刻、配对包络、启动错峰及所有 rank 启动后的尾部观测。首轮六项条件得到 48 条 rank 记录、24 组配对观测，其中正式测量 18 组；启动错峰与尾部耗时相近，尚未确认可迁移的通信服务。profiler 本身未启用 warmup 的提示及开销资格问题保留，操作预热不等于 profiler 预热。

三种产物均为 performance_manifest.json，明确引用实际输入、环境、完成计数和样本，不可用于 extract 或填补 base 语义。当前保持 readiness=needs_data：CPU 非 SDK 等待、观测开销，以及设备/通信成本适用性仍需处理。cpu 当前还记录原生准备、提交、完成代理及发布边界，生成 native_serving_timing；节点预测不消费旧联合延迟，不能将其当作纯设备服务。

独立通信采样的 `collective_measurements.json` 另含 `task_samples`：按原生任务身份关联的物理工作、等待与标记，以及字节量和来源引用。该数据仍保留 manifest 的成本资格限制，不应直接将所有任务的观测时长求和作为通信成本。

### 从独立单元样本拟合候选

```bash
scripts/markov.sh fit-costs --performance-manifest path/to/performance_manifest.json > candidate-model.json
```

该命令在modeling Docker内执行C++拟合，默认处理`native_serving_timing`中的完整准入预算单元。它只读取显式manifest引用的cpu_units，不扫描目录或采集；排除预热、未确认完整接纳路径和含runtime等待/探针的记录，并报告排除原因。以初始化路径、成员数量与horizon工作规模分组，合并rank取中位数，保留数量和原始范围，不去掉高值、不按输入长度拟合或外推。

使用 `--unit worker` 则读取 manifest 引用的 `cpu_worker_units`。只纳入完整单次提交、无已观测等待的 worker；从各段线程 CPU 时间中扣除已记录的成对探针开销，再按操作名称与 before/call/after 分组取中位数。探针计数不得超过所属线程区间，不截断为零。原生队列事件名称由 runtime 适配器提供，拟合器不解释队列类型。该候选假设固定模型和部署下可暂不按 shape、rank、阶段细分，须用独立数据检验；未测出的观测开销没有被扣除。worker 的独立性检查同时追溯原始记录，不能通过重新归一化同一份数据作为留出样本。

使用 `--unit kernel` 则读取 `native_device_work` 的 `device_measurements`。Ascend 生产端为每条计算记录提供 `kernel_work`，统一表达 kernel 名称、操作、输入/输出 shape、dtype、format、任务类型与实际启动块数；原始 vendor 字段和来源继续保留。C++ 按完整描述分组取中位数，排除预热、非计算工作与缺少描述的记录，不按 source span、request 身份或整批 decode 长度拟合。启动块数不同则属于不同工作键，设备核心容量不能替代启动规模。

kernel 候选目前是物理设备 span 的经验基线，未扣 profiler 开销或隐藏停顿，尚未纳入数据相关的序列/KV 工作特征，通过 `assets.cost_models` 可进入条件预测。目标按完整张量描述匹配，只有单一观测启动变体且不含额外语义工作时才使用候选；目标启动规模沿用该观测变体属于显式假设。未匹配 shape/启动描述不外推；原生 trace 参与训练/评价重叠检查。历史数据只能在明确导入任务中转换，缺少的 runtime 条件须显式保留为未知与缺口，不增加旧格式回退或伪造观测。

输出为`candidate`，没有可用样本时为`needs_data`；原测量资格与缺口保留，并随候选输出声明的配置、部署、runtime及相应 CPU/设备条件（observed_context），不据此自动授权成本迁移。该命令不修改源资产、独立留出数据或预测输入；准入/worker/kernel 候选可通过 `assets.cost_models` 显式导入条件预测。拟合成功不意味着探针边界、部署迁移或精度验收通过。

评价当前fit-costs生成的固定候选：

```bash
scripts/markov.sh evaluate-costs --model-candidate candidate-model.json \
  --performance-manifest path/to/held-out-performance-manifest.json
```

评价复用同一单元读取合同，只读取候选数值，不调用拟合。候选保存显式sample_sources；直接重用任一拟合样本文件会报错。输出匹配/未匹配工作、分组及总体MAE、带符号误差、最大绝对误差和WAPE（比例值），同时报告观测上下文是否相同、排除记录及双方测量缺口。零样本或零观测总时长的相关比率为null，不伪造零误差。文件分离及上下文相同都不是完整独立性或迁移资格证明；结果保持not_established，不回写候选、补采或提高预测资格。

拟合候选的显式消费：在项目 `assets.cost_models` 中填写候选路径，运行原有 `predict`。预测只读模型文件，不重拟合或加载源样本；独立测量 manifest 仍不是成本模型。当前匹配模型、框架和部署，由适配器判断配置关系，再按实际目标工作查询。SGLang 请求池容量、Prefill 预算可在已支持路径内变化，仍须满足模板与成本键；这只是候选消费条件，未建立真实性能或硬件迁移资格。未匹配模型不会触发采集。需要修订已保存任务时：

```bash
scripts/markov.sh resume --run path/to/run --cost-model path/to/admission-candidate.json \
  --cost-model path/to/worker-candidate.json
```

该列表替换全部成本模型，重算组合；重复无修订恢复不增加轮次。实际基准、来源和限制见[经验成本合同](02_user_contract.md#显式经验成本输入)。

## 7. 条件预测与恢复

每份assets.templates包含manifest和可选node_durations，给定耗时必须引用同一来源。预测容器仅挂载明确的 manifest、编译资产、给定耗时和拟合模型，不读取原始trace/source_facts；关闭diagnostics时跳过逐实例详细报告构造，仅汇总连接规则和执行结果；打开时按combination_N_graph.json输出物理图，按combination_N_graph.details.json保存逐工作/模板明细。主结果通过structure.graph.artifact和execution_details.artifact引用这两个文件，execution_details.work_count记录工作数；不再内嵌execution_graph数组，predict/status/resume不读取明细文件。语义区域与操作切口在一次报告中按不可变来源定义计算一次，目标参数和连接仍逐实例描述；明细文件仍含按实例重复的来源描述，尚未完成内容去重。

模板按模型、配置变换和部署筛选后共享；控制器、KV及执行状态按配置×workload组合隔离。来源缺失、控制范围外和节点耗时缺失分别报告，部分组合失败不阻断其他组合。当前支持共同原生条件下的多来源候选、整批来源切换及有限局部donor替换；完整代表性建库规划仍未完成。provenance.source是首选来源，实际使用集合见structure.graph.sources；工作明细的source_manifest限定source_call/source_batch，耗时说明统一见assets[].node_durations。

恢复输入用--template-assets FILE显式替换完整来源列表，数组项与项目assets.templates一致，路径相对FILE。资产修订后的当前实现重算全部组合；完成任务无修订的resume只读已有结果。改变模型、配置集合或请求序列建立新任务，不修改持久快照冒充恢复。缺兼容来源且base授权/预算允许时，predict为每个配置组自动运行一次代表配方并导入生成库；已有资产优先，加载失败先修复，规则/成本缺口不触发结构采集。失败及恢复不自动重试已尝试组。当前持久任务保存representatives（代表到组失败原因的映射，空原因表示尚无失败）与active_representative，旧任务快照需重新创建，不提供旧格式兼容。自动导入资产后按受影响配置组重算，保留其他组合结果；显式替换资产仍完整重算。真实节点成本与独立evaluate的当前交付状态见执行计划。


SGLang `native_environment.initialization` 记录实际 warmup token、长度、sampling_parameters 和窗口位置。生产端按[固定预热合同](02_user_contract.md#仿真起点统一预热后开始)检查并保存 initialization_gap，TemplateBank::qualify_initialization 消费该结果；缺失记录返回 initialization_not_observed，不匹配返回 fixed_warmup_not_observed。来源的共同资格与初始化资格分别报告：缺初始化证据的来源不能提供起点；后续整批候选须有完整workspace绑定、共同原生条件及整组接口资格，局部候选须满足区域合同，两者均消费唯一目标容量状态，不导入供体初始容量。旧编译资产必须从实际来源重编译，不能补造元数据或用默认值绕过缺口；通过配方检查不等于目标容量状态已资格化。allocator marker 现在也切分 worker CPU 服务，使用该可替换边界须从事实重新 `extract`，只 `compile-templates` 不能给旧图补 CPU 切口。

Workspace窗口状态由采集器写入`workspace_states/rank-*-pid-*.json`，manifest显式引用；ServingNormalizer保留初始/结束状态并核对原生分配记录的连续性。此入口已通过当前cached/non-overlap代表请求的真实serving验证，证据见`data/redesign/s4-workspace-serving/serving_verification.json`；目标容量推进与分支变换按现有workspace规则资格处理，当前验证范围见执行计划。旧manifest缺少该字段不做兼容解析，也不能把未观测状态填为空缓存；现有缺状态资产不具备workspace生命周期资格。该检查不提供目标需求公式或节点耗时。
