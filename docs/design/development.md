# 构建、运行与实现入口

本文说明当前构建、公开命令及修改归属，不记录实验结果。命令从仓库根执行；`YOUR_RUN` 和模型路径替换为实际值。

## 环境与构建

```bash
docker build -t markov-next-sglang-env:ubuntu22.04 docker/images/base/sglang
docker compose -p markov-next-redesign -f docker/compose/inference.yml build sglang-profile modeling
scripts/build.sh
```

已有匹配基础镜像时可跳过第一步。基础镜像只提供原生依赖，最终推理镜像还需安装当前 SGLang 和采集包；镜像标签就绪不表示构建或运行已经验收。KT 在正式适配接入前没有镜像服务入口。

modeling 使用 Ubuntu 24.04、GCC 13、C++23、CMake≥3.20、Ninja 和 nlohmann-json≥3.11；关闭语言扩展。正式目标为 `markov_core` 和 `markov`，Release 程序位于 `build/automatic-graph/markov`。构建、C++ 运行与静态工具均使用 modeling 容器。

公开脚本固定 Compose project `markov-next-redesign`；`MARKOV_IMAGE_PREFIX` 选择已有镜像前缀，默认 `markov-next`。修改编译器或工具链时使用新的构建目录。

镜像按职责稳定命名，不包含日期、运行时间或实验编号：`markov-next-modeling:ubuntu24.04` 用于核心构建与仿真，`markov-next-sglang-env:ubuntu22.04` 提供基础依赖，`markov-next-sglang:ubuntu22.04` 安装当前框架与采集代码。精确环境以运行记录中的镜像 ID 为准；重新标记已有镜像不代表重建或重新取得运行资格。

真实采集使用具备匹配 PyTorch/torch_npu/CANN、SGLang 及模型文件的推理镜像。Dockerfile 组合官方框架源码、相应安装适配及 `markov-collection`；Python 基线为 3.10。源码修改后须使运行容器使用本次代码，例如在开发容器中安装：

```bash
python -m pip install --no-deps --no-build-isolation -e .
```

正式镜像安装采集包，由 SGLang plugin 入口加载观察适配。官方子模块不保存采集业务修改；安装补丁和环境差异随适配器维护。镜像名称不能代替实际依赖和运行资格的核对。

## 完整任务入口

```bash
example/automatic_graph/run.sh --input example/automatic_graph/exploration.json
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
| `src/markov/controller` | 请求进度、批次、槽位和逻辑页的唯一状态 |
| `src/markov/execution` | 节点耗时接口、资源/依赖推进与完成事件 |
| `src/markov/adapters` | 原生端口、状态编码、并行角色及目标模板绑定 |
| `src/markov/workflow` | 控制、图装配、执行和状态提交的 serving 主循环 |
| `src/markov/cli` | 公开文件输入与原子发布 |
| `src/collection/markov_collection/instrumentation` | 通用原生调用与记录生命周期 |
| `src/collection/markov_collection/adapters/frameworks` | Batch、状态和框架入口差异 |
| `src/collection/markov_collection/adapters/runtimes` | 操作、形状、设备观察与环境差异 |
| `src/collection/markov_collection/artifacts` | 共同文件发布 |
| `src/collection/adapters` | 原生依赖安装资源及随环境编译的观察代码 |
| `example/automatic_graph` | 真实输入示例、机制观察及设备清单对照 |

C++ 头源按职责同目录，以 `src` 为 include 根。核心不导入框架配置，runtime 不反向依赖框架；框架组合 runtime 取得事实和必要接口。原生框架拥有请求、KV 和 worker，观察器不维护第二份原生状态机。

本地 deprecated 源码只供查阅，由 Git 忽略，不作为远端交付内容，也不进入 build、import 或 fallback。验证矩阵、批量编排和窗口判据留在 example，正式业务不导入示例。

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

固定预热配方由 `NativeServingSession.warmup_requests(page_size)` 维护，代表采集与单步示例共用它。当前 Ascend dense 路径还通过同目录原生观察库调用实际 allocator，按后端源码上界预留计算 stream 的 workspace，并记录预热及观察后的容量。单步的 `--input` 改变被观察请求及原生池配置，预热继续使用固定配方；`session-summary.json` 记录实际声明的预热请求，不预先跑完整目标序列。

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

完整 serving 使用带物理事实编译的全部 rank 资产：

```bash
scripts/markov.sh templates \
  --source data/YOUR_RUN/captures/source/rank-0.json \
  --physical data/YOUR_RUN/captures/source/physical.json \
  --output data/YOUR_RUN/assets/rank-0.json
scripts/markov.sh simulate \
  --templates data/YOUR_RUN/assets/rank-0.json \
  --input example/automatic_graph/serving.json \
  --output data/YOUR_RUN/results/serving.json
```

逻辑 TP 重复 `--templates` 提供各 rank 资产。`simulate` 接受目标请求及给定耗时，由完成端口驱动后续组批；`--details` 额外保留图、批次和输出事件。`compose --templates ... --target ... --output ...` 按目标 `invocations` 的命名端口连接区域，只是公开诊断，不维护另一套 serving 控制。

同一物理输入还包含搬运和等待的 `runtime_actions`。`runtime_coverage` 报告其操作归属，内部的 `native_arguments` 单独报告 API 参数观察及缺失名称；区域输出分别携带 `source_runtime_coverage` 与 `source_runtime_arguments`。没有物理观察时明确为 `not_observed`，参数覆盖不完整时保留 `partial`，二者都不能当成完整资源因果资格。

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
