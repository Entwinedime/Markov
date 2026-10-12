# 当前构建与运行

当前预测要求模板 source_state 中有真实 worker 准入策略。旧资产缺少该观测时，predict 返回 needs_data / admission_policy_not_observed，且不自动重采；此前完整输出预约路径已删除。不要手填原生默认值绕过缺口。新策略正向执行及完整 S3/S4 仍待验收，机制见[准入策略](sglang_template_relations.md#准入策略与内存压力)。

本页说明当前实现，开发主线为 SGLang + Qwen3-32B。plan、输入准备、SGLang base 采集、独立 CPU/设备/通信采样及部分 source 结构提取已接入；predict 已接通限定范围的自动采集、有限域补采、重算和任务恢复。主线的完整控制/成本资格与独立评价尚未完成。KT 原生采集已有成功结果，保留入口和资产，其预测闭环在后续扩展。README 按既有要求保持为空。

2026-10-09 修订的[实施计划](../redesign-workflow/2026-10-08_redesign_execution_plan.md)要求结构先行：统一流程与插桩 → 参数化 base 模板 → 给定节点耗时下的动态展开 → 节点 cost model。本页命令仍描述实际已有实现，现有整批精确查表与联合延迟不是最终节点模型；统一记录已开始落实原生 API 的显式阶段身份，给定节点耗时入口已接入，完整端口/结构资格尚未通过，不在此列出伪运行命令。用户已恢复实施，继续推进参数化、分支和共同消费接口；结构验收通过前不进入节点成本模型。集合输入、快照与组合执行已切换，代表性建库规划和节点成本仍未实现；当前范围以本页和输入合同为准。

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
├── contracts/                         # 统一流程、控制输入、观测值及用户输入
├── graph/                             # 来源图、节点和依赖表达
├── production/                        # SourceBindings、SourceCall、公共 BlockExtractor
├── adapters/
│   ├── frameworks/sglang/             # serving 绑定、子图提取、模板生产及采集配方
│   │   └── models/                    # Qwen3 模块声明、模型/attention 原生布局解释
│   └── runtimes/ascend/               # CANN 事件关联、AscendSourceExtractor、HCCL 布局
├── templates/                         # 统一模板、参数化、实例化及有限变换
├── controller/                        # 轻量控制器及业务状态
├── execution/                         # 目标执行图、资源、完成事件及唯一模拟时钟
├── performance/                       # 给定节点耗时；后续成本模型
├── workflow/                          # 输入规划、适配器选择、生产与预测装配
└── cli/                               # 命令解析
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

代表性建库已报告配置分组、最大请求池候选和短请求配方，并提供显式配方采集；已有资产覆盖、自动建库和节点cost model尚未实现。允许采集但缺来源时明确返回representative_collection_planning_not_implemented，不把目标workload自动重采当作代表性规划。显式采集入口见下文。

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

### 恢复实施后的优先工作

1. 按[动作合同](01_system_framework.md)复核完整主线迭代，补齐原生位置、状态效果与异步端口。
2. 落实[统一语义记录与模板](04_collection_templates.md)，把真实节点/依赖、参数映射和合法变体提供给核心。
3. 按[动态展开设计](05_controller_semantics.md)接入显式给定节点耗时，以同一执行核验证同配置、准入 2→1 和时间反馈；报告条件，不调用成本补采。
4. 结构完成条件满足后，再按内部性能单元复核样本和实现模型，集成正式预测与恢复。

输入/产物和公开入口变更同步更新；旧项目字段和旧恢复任务不提供兼容执行路径，历史资产按当前输入合同显式引用。

## 4. 显式诊断采集

```bash
scripts/markov.sh profile --project configs/redesign/sglang/project.json --configuration default --workload example
scripts/markov.sh profile --project path/to/project.json --configuration REPRESENTATIVE_ID --representative
scripts/markov.sh observe-target --project path/to/project.json --configuration CONFIG_ID --workload WORKLOAD_ID --budget-sec 600
```

profile读取选定配置和请求文件，使用collection.base的显式授权及累计预算；禁采时零attempt。SGLang普通profile/observe-target按相同arrival_time_ns批量提交预分词请求，不同到达组并发按计划提交；响应标记batch_response，不将组返回时刻当作逐请求完成。此命令是诊断采集，不是普通用户必须选择base的产品流程。KT使用同样的选取参数，服务部署取自所选配置，已有采集能力保留。

profile --representative仅接受plan选出的代表配置，不能同时传--workload。按实际页大小生成短请求，每组批量提交，等真实返回后再提交下一组；沿用collection.base预算，禁采时零attempt。配方及尚未覆盖机制见[实施计划](../redesign-workflow/2026-10-08_redesign_execution_plan.md#代表短请求候选与执行前门槛)。这仍是显式诊断入口，predict尚不会自动发起建库。

observe-target使用独立预算生成role=validation的观测，不能作为预测模板或成本。status/resume恢复已有任务不重置预算，原生容器存活时不能启动第二次采集或修改预算。首次部署与采集前的原生路径条件见[框架路径](framework_paths.md)。

预测宿主中断不等于模拟容器结束；prediction_live依据Docker状态。活跃预测的资产/预算修订返回requested_changes_applied=false；容器结束后恢复接收同轮结果。

## 5. 已有 base 的 source 提取

CPU API归一化也关联设备同步：`rtDeviceSynchronize`和`rtDeviceSynchronizeWithTimeout`按完整同线程序列对应`Runtime@DeviceSynchronize`，数量或已知scope不一致时保持未关联。关联只恢复调用归属，不生成硬件任务。提取器现按显式stream/device范围恢复提交前沿，CPU服务沿既有路径切除等待；模板存储、资源绑定和执行明细共用该范围。设备域存在未映射活动或重叠提交时保留未解析等待，不能宣称设备级动态展开已验收；workspace分支触发及完整资源域仍待落实。内部资产需从原始事实重提取，不兼容旧的无scope存储。 workspace需求探针已完成隔离构建和ATB分配/扩容/复用/清空运行验证，证据与边界见[workspace合同](sglang_template_relations.md#workspace-容量状态与设备级同步待实现)。SGLang Dockerfile经`docker/install/torch_npu_workspace.sh`调用runtime适配器的`install.sh`，构建固定提交的带探针原生库，保留匹配wheel的Python扩展；安装脚本和独立验证镜像的实际库加载检查已通过，当前cached/non-overlap代表请求的serving窗口链已通过验证；完整Dockerfile重建仍未验收。`MAX_JOBS`构建参数控制并行度，默认16。采集环境的`workspace_probe`记录包内构建来源，未安装时为null；现有镜像不会因源码修改自动获得探针，诊断scope也不构成完整serving模板。

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

编译模板保存lane、外部连接和等待的资源角色，来源报告resource_bindings列出原生编号及可选的control/submission/model/collective角色。未绑定角色不代表可跨来源迁移。此字段变更须从已有事实执行compile-templates；不支持旧资产格式兼容。

编译模板同时保存来源共享的sequence_capacity_tokens，表示包含本次query的最大已处理序列长度；原生模型/表/缓存解释只在生产端发生。缺此字段的历史编译资产须从满足当前合同的来源重新compile-templates，不提供旧格式兼容；无需重新采集。Decode候选长度使用共享容量，Prefill候选仍限一页；整组结构和分支资格继续单独检查。

predict只挂载assets.templates中各manifest和compiled_templates所指资产，不挂载source_facts、原始DAG/实例或vendor trace。已有事实需要构图时用extract，已有完整来源只需更新绑定时用compile-templates；缺编译资产返回needs_data / compiled_templates_missing，不自动补采或回退。processing.template_load_wall_time_ns记录模拟器加载资产的实际墙钟开销，不是目标节点耗时，也不计入预测指标。 实际图根sources数组列出来源manifest，节点及边界的source_asset是该数组索引，source_span/source_boundary仍是资产内局部编号；图摘要中的ports也按其sources表限定来源；缺耗时结果同时提供source_manifest与source_span。来源身份在加载时绑定，增加这些运行时字段不要求重编译资产。

纯逻辑完成边界、CPU 包络、物理 kernel 和通信注解有不同含义。源码未绑定成本，不将 observed_time_ns 的差值自动变成服务时间。详细合同见[采集与模板](04_collection_templates.md)。

## 6. 独立性能采样

```bash
scripts/markov.sh sample --project path/to/project.json --configuration CONFIG_ID --workload WORKLOAD_ID --measurement-requests work-domains.json --repetitions 3
scripts/markov.sh sample --project path/to/project.json --configuration CONFIG_ID --workload WORKLOAD_ID --kind device --measurement-requests work-domains.json --repetitions 3
scripts/markov.sh sample --project path/to/project.json --configuration CONFIG_ID --kind communication --repetitions 3
```

sample 使用 collection.performance 的 allow_run 和累计 max_wall_time_sec，关闭时返回 needs_data/performance_collection_disabled、零 attempts、零用量。kind 默认为 cpu，可选 device、communication；重复次数必须为正，默认每个测量条件三次。status/resume 与 base 共用原生容器预算、资源锁和恢复机制，base/performance 分开记账。

当前仅实现 SGLang lightweight 范围的 Ascend 路径，需要已给出的 token 输入；不支持文本解析、radix、overlap、graph、mixed prefill/decode、HiCache 或 chunked prefill。CPU/device 必须用 measurement-requests 显式指定非空测量工作域；不再按目标输入长度、输出长度与批大小自动展开采样网格。缺少该参数时在创建采集任务前报错，空数组也不接受。通信采样仍从显式独立设备资产提取通信工作域。输入是用户明确提供的工作域数组（当前节点执行报告不再生成整批 measurement_requests），每项含 phase、requests 的 input_tokens/context_tokens/prompt_tokens，以及 finished_request_count。原生输入配方由 C++ Planner 生成，使用选定 workload 中同长度的真实 token 输入和新的测量身份，不运行其业务到达序列，不读取 target 实测或评分。

这是当前联合批次测量合同，尚未按最终节点/局部子图粒度定义；请求完成数也不足以区分具体结束成员。已有入口与资产保留，后续在 S5 依据已验收结构确定正式单元及特征，当前不继续靠补齐 9/8、10/9 等整批组合推进结构验收。

不同 decode 上下文通过独立请求的真实 token 进度分批释放形成；同组请求一起提交。生成 token、原生 scheduler 和 KV 更新保持原生行为，既不覆盖 token 也不暂停 scheduler。配方是请求的工作域，实际原生 batch 与 shape 才是测量证据；客户端释放时刻不能保证所有域都会到达。预热、完成覆盖和实际成本查询继续决定资产是否可用。

cpu 采样运行官方 Engine 的 所选配置，关闭 profiler 和原生 API 文件日志。记录 schedule 前段、prepare 本体、schedule 后段、execute 和 publish 的线程 CPU；嵌套 prepare 及其记录开销不重复计入 schedule。原生观察库单列已有 stream/event/device 同步 API 的线程 CPU、墙钟及探针 CPU，保留原始值和扣除后的非等待 active 值；没有新增同步。同步墙钟用于解释等待，不能当作服务。

每个测量条件前先清空原生请求/cache 状态并执行预热，预热和正式测量各有声明身份。SamplingDomain 按实际 rank、阶段、batch 和每请求输入/context/prompt/generated 长度核对先前预热覆盖，不根据客户端 batch 大小认定成功。第三轮 CPU 采样已实际覆盖全部测量条件：128 条预热、384 条测量记录，missing_conditions 为空。首次 B=2 的约 2.86 秒 prepare 留在预热中，正式测量为约 2.7–2.9 毫秒；没有删除样本或添加时延补偿。预热中的长延迟几乎不在已拦截的 SDK 同步 API 内，具体初始化来源尚未确认。

device 采样复用相同测量条件和预热规则，启用框架 profiler 及原生 API 观察，单独保存硬件计算/传输 span、shape/dtype/layout、业务工作量、精确来源和可关联的传输字节/方向。已完成真实采集：169184 条记录，正式测量计算 76776 条、传输 50112 条，预热域无缺失。归一化只生成 device_measurements.json 与 role=performance manifest，不生成 source DAG 或语义模板。通信汇总保留为非叠加观测，compound transport 包络不直接当作通信服务；本模式的 profiler CPU 时间不作为 CPU 成本。实际来源和缺口见[执行记录](../redesign-workflow/2026-10-08_redesign_execution_log.md)。

communication 需要 assets.performance_manifests 显式引用同一 SGLang TP/设备部署的独立 device 测量。准备阶段从原生 collective_descriptors 提取操作、元素数、dtype、算法和来源；当前实现 BFP16 allReduce/allGather、Ascend HCCL、每 rank 一个设备且 TP≥2。采样不加载模型权重，每项先预热再重复测量；同步仅用于独立 benchmark 的完成隔离，未插入 serving 捕获。collective_measurements.json 保留逐 rank 原始时刻、配对包络、启动错峰及所有 rank 启动后的尾部观测。首轮六项条件得到 48 条 rank 记录、24 组配对观测，其中正式测量 18 组；启动错峰与尾部耗时相近，尚未确认可迁移的通信服务。profiler 本身未启用 warmup 的提示及开销资格问题保留，操作预热不等于 profiler 预热。

三种产物均为 performance_manifest.json，明确引用实际输入、环境、完成计数和样本，不可用于 extract 或填补 base 语义。当前保持 readiness=needs_data：CPU 非 SDK 等待、观测开销，以及设备/通信成本适用性仍需处理。cpu 当前还记录原生准备、提交、完成代理及发布边界，生成 native_serving_timing；限定模块预测可以显式接受其联合延迟近似，不能称为纯设备服务。

## 7. 条件预测与恢复

每份assets.templates包含manifest和可选node_durations，给定耗时必须引用同一来源。预测容器仅挂载明确的manifest、编译资产和耗时，不读取原始trace/source_facts；关闭diagnostics时跳过逐实例详细报告构造，仅汇总连接规则和执行结果；打开时按combination_N_graph.json输出物理图，按combination_N_graph.details.json保存逐工作/模板明细。主结果通过structure.graph.artifact和execution_details.artifact引用这两个文件，execution_details.work_count记录工作数；不再内嵌execution_graph数组，predict/status/resume不读取明细文件。语义区域与操作切口在一次报告中按不可变来源定义计算一次，目标参数和连接仍逐实例描述；明细文件仍含按实例重复的来源描述，尚未完成内容去重。

模板按模型、配置变换和部署筛选后共享；控制器、KV及执行状态按配置×workload组合隔离。来源缺失、控制范围外和节点耗时缺失分别报告，部分组合失败不阻断其他组合。当前支持共同原生条件下的多来源候选、整批来源切换及有限局部donor替换；完整代表性建库规划仍未完成。provenance.source是首选来源，实际使用集合见structure.graph.sources；工作明细的source_manifest限定source_call/source_batch，耗时说明统一见assets[].node_durations。

恢复输入用--template-assets FILE显式替换完整来源列表，数组项与项目assets.templates一致，路径相对FILE。资产修订后的当前实现重算全部组合；完成任务无修订的resume只读已有结果。改变模型、配置集合或请求序列建立新任务，不修改持久快照冒充恢复。真实节点成本、自动代表性建库、组合依赖重算及独立evaluate仍待完成。


SGLang `native_environment.initialization` 记录实际 warmup token、长度、sampling_parameters 和窗口位置。生产端按[固定预热合同](02_user_contract.md#仿真起点统一预热后开始)检查并保存 initialization_gap，TemplateBank::qualify_initialization 消费该结果；缺失记录返回 initialization_not_observed，不匹配返回 fixed_warmup_not_observed。来源的共同资格与初始化资格分别报告：缺初始化证据的来源不能提供起点；后续整批候选须有完整workspace绑定、共同原生条件及整组接口资格，局部候选须满足区域合同，两者均消费唯一目标容量状态，不导入供体初始容量。旧编译资产必须从实际来源重编译，不能补造元数据或用默认值绕过缺口；通过配方检查不等于目标容量状态已资格化。allocator marker 现在也切分 worker CPU 服务，使用该可替换边界须从事实重新 `extract`，只 `compile-templates` 不能给旧图补 CPU 切口。

Workspace窗口状态由采集器写入`workspace_states/rank-*-pid-*.json`，manifest显式引用；ServingNormalizer保留初始/结束状态并核对原生分配记录的连续性。此入口已通过当前cached/non-overlap代表请求的真实serving验证，证据见`data/redesign/s4-workspace-serving/serving_verification.json`；目标容量推进已接入同配置、保留原分支路径，分支替换仍未接通。旧manifest缺少该字段不做兼容解析，也不能把未观测状态填为空缓存；现有缺状态资产不具备workspace生命周期资格。该检查不提供目标需求公式或节点耗时。
