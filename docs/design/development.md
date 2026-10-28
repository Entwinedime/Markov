# 构建、运行与实现入口

本文说明当前构建、公开命令及修改归属，不记录实验结果。命令从仓库根执行；`YOUR_RUN` 和模型路径替换为实际值。

## 环境与构建

```bash
docker build -t markov-next-sglang-env:ubuntu22.04 docker/images/base/sglang
docker compose -p markov-next-redesign -f docker/compose/inference.yml build sglang-profile modeling
scripts/build.sh
```

已有匹配基础镜像时可跳过第一步。基础镜像只提供原生依赖，最终推理镜像还需安装当前 SGLang 和采集包；镜像标签就绪不表示构建或运行已经验收。KT 在正式适配接入前没有镜像服务入口。

宿主代理使用回环地址时，构建命令前加 `MARKOV_BUILD_NETWORK=host`，使构建进程能访问该代理；默认网络下的 `127.0.0.1` 指向构建容器自身。该选项只决定依赖下载网络，不改变纯预测的隔离环境。

modeling 使用 Ubuntu 24.04、GCC 13、C++23、CMake≥3.20、Ninja 和 nlohmann-json≥3.11；关闭语言扩展。正式目标为 `markov_core` 和 `markov`，Release 程序位于 `build/automatic-graph/markov`。构建、C++ 运行与静态工具均使用 modeling 容器。

公开脚本固定 Compose project `markov-next-redesign`；`MARKOV_IMAGE_PREFIX` 选择已有镜像前缀，默认 `markov-next`。修改编译器或工具链时使用新的构建目录。

镜像按职责稳定命名，不包含日期、运行时间或实验编号：`markov-next-modeling:ubuntu24.04` 用于核心构建与仿真，`markov-next-sglang-env:ubuntu22.04` 提供基础依赖，`markov-next-sglang:ubuntu22.04` 安装当前框架与采集代码，`markov-next-control:ubuntu22.04` 运行无设备原生控制。control镜像从固定推理栈移除NPU发现及采集插件，安装控制适配，不改上游源码或原推理镜像。精确环境以运行记录中的镜像 ID 为准；重新标记已有镜像不代表重建或重新取得运行资格。

真实采集使用具备匹配 PyTorch/torch_npu/CANN、SGLang 及模型文件的推理镜像。Dockerfile 组合官方框架源码、相应安装适配及 `markov-collection`；Python 基线为 3.10。源码修改后须使运行容器使用本次代码，例如在开发容器中安装：

```bash
python -m pip install --no-deps --no-build-isolation -e .
```

正式镜像安装采集包，由 SGLang plugin 入口加载观察适配。官方子模块不保存采集业务修改；安装补丁和环境差异随适配器维护。镜像名称不能代替实际依赖和运行资格的核对。

## 完整任务入口

```bash
example/automatic_graph/run.sh --input example/automatic_graph/exploration.json
example/automatic_graph/run.sh --input example/automatic_graph/disaggregated.json
example/automatic_graph/run.sh --resume data/YOUR_RUN
```

脚本在 modeling 容器中组织原生采集、解释、C++ 建库和全部配置 × 请求组合，产物集中在一个 UTC 时间目录。输入声明模型、部署、配置、请求、给定节点耗时及累计预算；首次加 `--details` 才保留原始导出和完整目标图，恢复沿用原策略。恢复仅可通过 `--input` 修改总预算或完整耗时映射，模型、配置、请求及部署固定。

任务从保留事实恢复解释和建库，不因后处理失败重复启动 Engine。C++ 二进制复用项目构建缓存；SDK 观察库属于任务私有 work，正式资产发布后释放。纯目标执行只挂载程序、明确输入和资产，不加载模型或设备。

共享程序的构建持独占锁，正式任务和 `scripts/markov.sh` 在调用期间持同一文件的共享锁；任务启动端保护挂载，容器内工具持锁直到退出，客户端丢失也不提前释放工作进程的保护。多个读取可以并行，重建等待使用者结束；运行根锁另行保护单个任务的状态与产物，不能代替构建锁。

## 职责与目录

| 位置 | 职责 |
| --- | --- |
| `src/markov/contracts`、`graph` | 有限表达式、值与存储依赖、来源检查 |
| `src/markov/production` | 声明接口与私有实现的公开投影 |
| `src/markov/templates` | 分层归一化、资产编译、区域选择与逻辑实例化 |
| `src/markov/controller` | 请求进度和批次决策，借用共同资源 |
| `src/markov/resources` | 槽位、逻辑页和驻留的唯一状态，按请求需求生成分配结果 |
| `src/markov/execution` | 节点耗时接口、资源/依赖推进与完成事件 |
| `src/markov/adapters` | 原生端口、状态编码、并行角色及目标模板绑定 |
| `src/markov/workflow` | 控制、图装配、执行和状态提交的 serving 主循环 |
| `src/markov/cli` | 公开文件输入与原子发布 |
| `src/collection/markov_collection/instrumentation` | 通用原生调用与记录生命周期 |
| `src/collection/markov_collection/adapters/frameworks` | Batch、状态和框架入口差异 |
| `src/collection/markov_collection/adapters/runtimes` | 操作、形状、设备观察与环境差异 |
| `src/collection/markov_collection/artifacts` | 共同文件发布 |
| `src/collection/adapters` | 原生依赖安装资源及随环境编译的观察代码 |
| `src/control/markov_control/adapters` | 原生控制、资源协议视图与无设备runtime接管 |
| `src/control/markov_control/workflow` | 原生控制与核心进程的管道连接、运行时限、日志与清理 |
| `example/automatic_graph` | 真实输入示例、机制观察及设备清单对照 |

C++ 头源按职责同目录，以 `src` 为 include 根。核心不导入框架配置，runtime 不反向依赖框架；框架组合 runtime 取得事实和必要接口。原生框架拥有请求、KV 和 worker，观察器不维护第二份原生状态机。

ServingWorkflow 依赖 ServingControl 与 BatchExpansion，CLI 组合具体控制器并保持借用对象的生命周期。动作带目标组和组内任务序号，事件按相同身份交回；控制器拥有请求进度，独立 KvPool 持有槽位/页状态；CLI 保持资源池比控制器存活更久，workflow 仅保留在途动作、页归还结果及可选报告。执行器注册端口通知，按依赖就绪和资源空闲推进唯一时钟；图回收后未完成工作仍由执行器持有，装配侧保留活跃状态绑定。ParallelTopology 提供逻辑 rank、位置及组角色的绑定，ParallelComposition 按相应参与组和任务调用身份连接通信，不默认使用全部 rank，也不按构图顺序预占共享通信资源。

PD 由 DisaggregatedController 维护一份请求进度，借用两个独立 KvPool；页组件同时支持即时归还与待合并后排序，决策不复制页所有权。SGLang 的 DisaggregatedStructure 组合两个 RankStructure，绑定预约、无模型接入、搬运和释放；原生 sorted allocator 的两个缓冲区保持独立，只有分配需要时展开合并模板。框架观察、声明和双服务生命周期分开维护，memfabric 接口事实留在 Ascend runtime adapter。详细结果保留动作完成时刻、页复制对应和展开入口名；默认结果保留请求、两池终态和来源资格。

TemplateLibrary 逐个消费角色来源，共用一份规范化目录并在发布时移交结果；编译中的来源和中间调用不复制进正式共享定义。CLI 持有类型化 TemplateCatalog，各 TemplateAsset 仅持有本角色的调用/资源绑定并借用该目录，整个展开期间保持目录和角色资产存活。

本地 deprecated 源码只供查阅，由 Git 忽略，不作为远端交付内容，也不进入 build、import 或 fallback。验证矩阵、批量编排和窗口判据留在 example，正式业务不导入示例。

## 无设备原生控制入口

```bash
docker compose -f docker/compose/inference.yml build control
bash scripts/markov-control.sh \
  --templates data/YOUR_RUN/assets/templates.json \
  --input data/YOUR_RUN/inputs/serving.json \
  --output data/YOUR_RUN/results/native.json
```

启动端使用Python≥3.10、Docker与已有Release程序，启动C++执行容器和原生控制容器；后者按来源TP规模启动独立的原生worker。控制侧协调进程只交接消息，不选择批次或维护另一份KV账本。双方只通过本地管道交接必要值；不挂载设备、权重或目标观察，不启用网络。原生模型配置及dtype来自模板资产，不另选模型目录。`--timeout-seconds`默认300，使用真实单调时钟限制运行；正常完成、错误、中断或超时均清理本次拥有的容器；核心结果先写入本次临时目录，全部参与进程成功退出后才替换最终结果，失败保持旧结果。原生wall-clock watchdog不另起后台资源查询，运行时限统一由外层负责。

默认结果仍为精简JSON，`--details`才输出完整图与执行记录；上述命令的运行日志位于`results/native.logs/core.log`和`native.log`。内部C++接口为`simulate --control native`，其标准流属于协议；直接运行该接口需要连接控制端，通常使用外层脚本。

当前接基础单worker及单阶段TP/EP，完整Prefill、无前缀缓存和无overlap；PP/PD与调度优化尚未接入。各worker运行真实原生主循环，入口成员沿原生CPU组广播请求；只读容量查询独立处理，一致的资源变更和执行动作只向核心提交一次，返回值形成各rank的本地视图。分片计算与设备通信仍由已有模板展开，不以控制进程间的CPU通信替代目标设备工作。原生结果处理通过copy_done等待已有结果读取子图，模型host返回不直接授权CPU消费token；随后沿同一结果调用展开释放和输出。原生运行批次的实际筛选作为Select动作交接，撤回通过独立Release归还驻留，原生保留输出历史并决定重新Prefill。给定输出长度用占位token驱动原生结果处理，不预测自然文本；目标释放路径仍须有合格模板，例如混合dtype且含空列表的拼接不能借用同dtype原型。入口可运行不代表完整结构资格或性能精度通过。

## 来源与区域命令

单步观察前，在同一个已安装采集包的推理容器构建 SDK 工具，再传入生成的矩阵选择程序：

```bash
MARKOV_OBSERVER_BUILD="$PWD/data/YOUR_RUN/work/observer-build"
cmake -S src/collection/adapters/runtimes/ascend \
  -B "$MARKOV_OBSERVER_BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DMARKOV_ASCEND_INCLUDE_DIR=/usr/local/Ascend/ascend-toolkit/latest/include
cmake --build "$MARKOV_OBSERVER_BUILD" -j2
mkdir -p data/YOUR_RUN/captures/source/runtime
LD_PRELOAD="$MARKOV_OBSERVER_BUILD/libmarkov_ascend_observer.so" \
MARKOV_NATIVE_RUNTIME_DIR="$PWD/data/YOUR_RUN/captures/source/runtime" \
python example/automatic_graph/serving.py \
  --model /models/Qwen3-8B --profile \
  --matrix-selector "$MARKOV_OBSERVER_BUILD/markov_ascend_matmul_selection" \
  --output data/YOUR_RUN/captures/source
```

示例通过原生 Engine 加载完整模型的 dummy 权重，统一预热后分别执行未插桩和观察路径，关闭会话后调用共同解释入口。未启用 `--profile` 时只解释语义关系；启用时同时生成 `physical.json`。`--tp-size N --rank R` 用于物理单卡执行一个逻辑角色；每个角色使用独立输出目录。单卡辅助传输提供结构来源，不提供真实多卡数值、通信时延或争用样本。

启用 profiler 时，原生阶段只记录、停止并落盘原始缓冲；解释阶段调用 Torch NPU 的官方 analyse 生成 trace，再建立物理归属。这样 PP 不会因前一个 stage 解析导出而使后续 stage 持续记录。原始落盘计入 native 额度，解析与建库计入 processing；恢复复用原始记录及已完整发布的物理归属，未发布的角色从原始记录重新解析，不能以 SDK 输出文件存在推定其已完成。阶段分离不减少必需观察，也不保证任意采集都能在给定额度内完成。

PP语义观察在原生循环提交完已有proxy/output发送句柄后发布，保留延后发生但仍属于原批次的等待。采集结束以完整观察文件为边界；发送、接收和等待仍由框架执行。

多 stage 来源通过一键任务的 pp 和采集设备列表选择原生 TP×PP 会话；实际启动全部必要参与者，只为规划的代表发布观察及对应 physical-rank 文件，其余角色保留代表引用和独立绑定。TP 本地角色与 PP 来源组共享采集/解释接口，任务环境在 work 中保留 launcher 缓存；不能把缺少 peer 的单卡运行当作完整流水线来源。

独立角色对照可在 adapter 内部采集请求中设置 `observe_all_workers`，启动真实参与组并观察所有 worker；它不改变代表分类或派生绑定，实际记录名单另由 `observed_roles` 交出。此选项不进入探索输入合同；正常采集仍只观察必要代表。

`serving.py --observe-all-workers`提供相同的示例入口。共同实例的`--input`读取正式serving输入，当前要求请求同时到达；TP数量仍用`--tp-size`指定。输入包含EP拓扑时，adapter按请求、层及逻辑token将同一份`ep_routes`注入原生TopK结果，包含padding，保留原生组批、gate计算和真实dispatch/combine。baseline和观察路径使用相同条件，统一预热不变；辅助注入不进入待预测工作，其时长不能作为性能样本。这种对照检验给定路由下的结构，不验证自然专家选择或文本数值。

仿真明细记录每个动作完成时已有的成员效果；`commits_requests` 表明该动作是否提交外部请求状态。中间stage、Prefill计算或资源交接可以有计划效果而不提交请求，不能把这些明细字段本身当作状态已提交；默认精简输出不保留动作历史。

固定预热配方由 `NativeServingSession.warmup_requests(page_size)` 维护，各采集入口共用。结构观察使用独立的 `structure_requests(page_size)` 五请求配方，覆盖页前/页边界/页后及对齐和非对齐输入，保留槽位复用后仍需Decode的尾请求；PD沿用相同prompt，保留较长输出和接入即结束的请求。原语辅助观察覆盖固定实现分支，不代替实际模型来源。当前 Ascend dense 路径还通过同目录原生观察库调用实际 allocator，按后端源码上界预留计算 stream 的 workspace，并记录预热及观察后的容量。单步的 `--input` 改变被观察请求及原生池配置，预热继续使用固定配方；`session-summary.json` 记录实际预热请求。PD 的 capture.json 保存采集输入，responses 目录保存各轮响应，二者职责分开，不预先跑完整目标序列。

宿主调用 modeling 消费来源：

```bash
scripts/markov.sh inspect --source data/YOUR_RUN/captures/source/rank-0.json
scripts/markov.sh produce \
  --source data/YOUR_RUN/captures/source/rank-0.json \
  --output data/YOUR_RUN/diagnostics/source-graph.json
scripts/markov.sh templates \
  --source data/YOUR_RUN/captures/source/rank-0.json \
  --output data/YOUR_RUN/assets/templates.json
scripts/markov.sh expand \
  --templates data/YOUR_RUN/assets/templates.json \
  --target data/YOUR_RUN/inputs/target.json \
  --output data/YOUR_RUN/diagnostics/region.json
```

`inspect` 检查声明的形状、条件及状态关系，以 `contract_gaps` 报告来源缺口；`produce` 发布公开来源图；`templates` 分开自身定义、组合布局和来源绑定；`expand` 根据角色、阶段及目标参数实例化逻辑区域。机器输入和输出是完整合同，终端只打印摘要。写入不会覆盖输入，产物先写临时文件，再原子替换。

目标文件例如：

```json
{"role":"model","phase":"decode","parameters":{"M":3,"B":3,"R":4,"max_sequence_tokens":130}}
```

动态参数由目标调用方提供，固定模型/层绑定来自资产。来源动态状态和耗时不作为目标默认值。布局条件未满足或参数缺失会明确失败；逻辑区域生成不能证明完整 serving 控制、物理执行或预测精度。

状态区域的前态和实际工作量是输入，后态由有限关系推导。程序核对相同关系是否还原原生观察，编译资产保留关系和状态效果；目标不能覆盖派生字段。C++ 区域返回类型明确的状态变化，JSON 只承担公开交接，不作为内部控制状态机。

可变只读 Tensor 列表通过目标的 `input_groups` 提供成员参数，数量与汇总量由模板关系计算。父子区域的组绑定同时传递值和参数，来源成员不作为目标默认值；组生成与逐项访问仍需对应正式规则。

`probe.py`、`state.py`、`collectives.py` 分别观察真实操作、页池和 collective。`physical.py` 消费来源记录及原生 trace_view，生成规范事件、设备归属及清单摘要；名称与数量一致只证明该点的设备工作清单，没有时序或成本资格。

`produce` 和 `templates` 可在 `--source` 后接 `--physical path/to/physical.json`，消费同一捕获的设备事实。`physical_coverage` 区分未观察、全部已归属及部分未归属，并聚合列出缺少公开操作的工作。此结果只说明归属，不能代替资源顺序或读写端口的资格；区域输出用 `source_physical_coverage` 保留来源整体状态。

完整 serving 使用带物理事实编译的来源域模板目录：

```bash
scripts/markov.sh templates \
  --source data/YOUR_RUN/captures/source/rank-0.json \
  --physical data/YOUR_RUN/captures/source/physical.json \
  --output data/YOUR_RUN/assets/templates.json
scripts/markov.sh simulate \
  --templates data/YOUR_RUN/assets/templates.json \
  --input example/automatic_graph/serving.json \
  --output data/YOUR_RUN/results/serving.json
```

逻辑 TP 在同一 `templates` 命令中重复 `--source`，每项后可接本次捕获的 `--physical`；输出一份共享 catalog 和独立 roles，`simulate` 只接收这一份文件。旧内部资产须从来源明确重建，不兼容读取。`simulate` 接受目标请求及给定耗时，由完成端口驱动后续组批；`--details` 额外保留图、批次和输出事件。`expand`/`compose` 的目标可用 `source_rank` 选择来源角色，默认 0；`compose` 按目标 `invocations` 的命名端口连接区域，只是公开诊断，不维护另一套 serving 控制。

原生组批可以独立导出并回放，不增加 workload 字段：

```bash
scripts/markov.sh record-batches \
  --source data/YOUR_RUN/captures/source/rank-0.json \
  --input data/YOUR_RUN/inputs/serving.json \
  --output data/YOUR_RUN/results/batches.json
scripts/markov.sh simulate \
  --templates data/YOUR_RUN/assets/templates.json \
  --input data/YOUR_RUN/inputs/serving.json \
  --batches data/YOUR_RUN/results/batches.json \
  --output data/YOUR_RUN/results/replayed.json
```

导出使用保留的完整语义观察和同目录的 `session.json`，输入应为当次原生运行的请求，按原始输入顺序对应原生请求 ID。P/D 分别提供各实例 TP rank 0、PP rank 0 的 `--source`。SGLang adapter 过滤辅助观察，转换实际完成的 Prefill、Decode 和无模型接入批次；只导出实例内顺序、阶段、微批次上下文 `lane`、请求身份及 token 范围。PP 的 lane 直接观察原生调度循环，缺少此事实的旧 PP 记录不可回放；非 PP 只有 lane 0。记录不包含图、节点耗时或物理页号。

共同控制消费记录选择成员，继续独占请求/KV 状态，并等待执行器的完成反馈；P/D 预约按记录的 P 批次顺序，交接按请求身份连接，支持多成员 Prefill 和接入。CLI 先统一校验、按到达时间稳定排序请求，再交给控制器与展开器；记录按 ID 对应，不依赖排序后的下标。默认不传 `--batches` 时仍使用轻量组批策略；结果的 `control_source` 区分两者。

当前回放只接收冷缓存、完整 Prefill、逐 token Decode 及固定 lane 的完整序列，不支持 chunk、prefix cache 或 speculative。记录替代组批策略的选择，实际槽位、KV 容量和进度仍须满足；模板若缺少该批次的结构变体，仍会失败，不自动补采或放宽条件。回放固定组批条件以核对展开，不等于原生在线调度器，不重放原生物理分配顺序、跨实例事件时序或全部 PP 重叠。使用同一次观察建库再回放仅证明功能链路，不能作为独立泛化或性能精度验收。

同一物理输入还包含搬运和等待的 `runtime_actions`。`runtime_coverage` 报告其操作归属，内部的 `native_arguments` 单独报告 API 参数观察及缺失名称；区域输出分别携带 `source_runtime_coverage` 与 `source_runtime_arguments`。没有物理观察时明确为 `not_observed`，参数覆盖不完整时保留 `partial`，二者都不能当成完整资源因果资格。

同步回调与 API 按相同线程、归属和事件种类的一一调用顺序交接参数，允许回调出现在 API 前或后；数量不符或调用交错时保留缺口，不能依最近时间关联。私有 ELF 符号不可用不影响回调已观测到的 stream；是否排空队列仍需独立的原生协议依据。

Ascend SDK 工具由 `src/collection/adapters/runtimes/ascend` 的独立 CMake 工程在所选推理容器内构建，`MARKOV_ASCEND_INCLUDE_DIR` 指向同一环境的 SDK。`markov_ascend_observer` 由推理进程预加载，记录实际参数并提供 workspace 预留桥，使用安装环境的 Torch 头文件及 ABI；`markov_ascend_matmul_selection` 仅查询无设备数据的矩阵描述符，匹配供应商字符串 ABI。二者独立于 modeling 容器中的核心 C++23 构建，不进入 `markov_core` 或纯预测依赖。

当前专用 workspace 容量由固定 torch_npu 的原生 memory snapshot 提供，提交队列身份来自原生 enqueue/dequeue 事件，ACL 参数由本任务构建的预加载库记录。对应消费者分别是 `AscendWorkspaces`、`NativeQueues` 和 `NativeRuntimeCalls`；安装能力按这些实际交接维护。

物理实现选择随 runtime 维护：矩阵读取原生元数据选择，拼接、布局物化和一维 int64 索引写入使用固定 CANN 的源码判据与有限原型。后解释的条件优先读取调用作用域继承的端口几何，不能把复用张量在另一调用中的参数名直接带入当前模板。框架只提供实际操作配方和参数含义；辅助原型在统一预热后沿同一 worker 观察，不进入第二套调度或状态管理。

## 修改后的必要检查

沿实际输入、生产者、消费者与资源生命周期审查；只运行受影响的正式入口。重要 C++ 修改做必要的 Docker Release 构建。

```bash
bash -n scripts/build.sh scripts/markov.sh
ruff check src/collection example/automatic_graph
git diff --check
```

格式工具使用 modeling 内的版本，保持 `.clang-format`；逻辑分段和重要注释需要人工审阅。Python 在实际环境做语法和已有 lint 检查。不新增独立 test 工程、fixture、mock 或内部自测入口。
