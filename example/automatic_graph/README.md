# 自动取图与条件 Serving 示例

输入一个固定模型、配置集合和请求集合，流程自动选择代表来源、单卡采集逻辑角色、建立参数化模板，再按目标状态动态展开。模型使用完整结构的 dummy 权重，节点耗时来自显式假设；输出用于检查执行结构，不构成真实性能预测或精度结论。

## 准备与一键运行

从仓库根执行。先准备 `/models` 下的模型文件，并修改 [exploration.json](exploration.json) 中的模型路径、物理设备、目标配置、请求和预算。

默认镜像为 `markov-next-sglang-env:ubuntu22.04`、`markov-next-sglang:ubuntu22.04` 和 `markov-next-modeling:ubuntu24.04`，名称不包含日期。基础镜像提供依赖，最终推理镜像安装当前 SGLang 与采集代码；已有基础镜像或更改标签不表示最终镜像已经构建和验证。

需要制作镜像时使用：

```bash
docker build -t markov-next-sglang-env:ubuntu22.04 docker/images/base/sglang
docker compose -p markov-next-redesign -f docker/compose/inference.yml build sglang-profile modeling
```

镜像就绪后，一条命令完成结构探索：

```bash
example/automatic_graph/run.sh --input example/automatic_graph/exploration.json
```

`MARKOV_IMAGE_PREFIX` 可选择其他已准备好的前缀，默认 `markov-next`。脚本在 modeling 容器内组织任务，通过宿主 Docker 创建本任务的原生容器；采集前会构建当前 C++ 核心和任务私有 SDK 观察库。物理设备只选择采集位置，目标 TP 不要求现场拥有同样数量的卡。

示例预算是有限上限，不保证任意模型或配置都能在其中完成。初始化、统一预热、观察、导出、处理和失败均计入累计额度；额度不足时保留状态并停止，不隐式扩大预算或循环补采。

## 来源复用与结果含义

每次任务固定一个模型。dtype、TP、attention backend 和 page_size 相同的配置共享来源；每个域按实际逻辑 rank 取得本地分片。不同模型或域分别建立来源，不以普通 rank 的相似 shape 自动替代所有角色，也不声明同库跨 TP 或跨页大小变换。

同域内根据请求、并发、KV 容量与 Prefill 预算重新组批、求值几何、重复成员子图和连接端口。原生实现变化由已声明的条件和局部原型选择；超过模型上下文、参数范围或原型覆盖时保留明确失败。正式入口提供 SGLang/Ascend，单卡辅助通信只提供结构接口，不提供真实跨卡时延。

控制基线为 FCFS、分开的 Prefill/Decode、固定输出长度和冷逻辑 KV。统一预热使用与目标序列分开的固定五请求，并按原生 ATB 上界预留 workspace；预热不计入目标服务时间，但计入采集费用。当前 runtime 规则针对 CANN 9.0/Ascend910B 的 dense 路径，workspace 预留限制为相等且不大于 128 的 head 维度。其他缓存、chunk、overlap、speculative 或内容相关路径需要相应能力。

给定耗时改变后，完成事件会改变后续组批。一次运行完成、条件结构可展开、独立原生对照相符和性能精度通过分别判断，不能相互代替。

## 输出与恢复

每次运行创建一个 UTC 时间目录并打印位置：

```text
data/<UTC时间>_automatic-graph/
  inputs/                       原任务、环境身份及逐组合输入
  captures/<角色-尝试>/         会话摘要、操作来源及规范物理事实
  assets/<实现域>/              每个逻辑 rank 的正式模板
  results/summary.md             人读入口、失败与累计费用
  results/<配置>/<请求集>.json   默认的精简组合结果及逐请求结果
  diagnostics/                  --details 时的完整组合结果
  logs/run.log                  子进程命令与失败原因
  state.json                    任务状态、资产位置及费用
  work/                         SDK、观察库与尚未发布的草稿
```

默认保留后续消费需要的规范来源与模板，随状态提交释放无引用历史。首次运行加 `--details` 才保留完整图、逐批/逐 token 记录和原始 profiler/runtime 导出；完整组合结果改写到 diagnostics。全部域资产发布后释放私有 work，即使目标结构仍为 partial。数据和构建产物不提交 Git。

恢复沿用原目录：

```bash
example/automatic_graph/run.sh --resume data/<UTC时间>_automatic-graph
```

模型、配置、请求和部署固定；通过 `--input 修改后的任务.json --resume 原目录` 仅可调整总预算或替换完整给定耗时。累计费用不清零，耗时变化从初态重算结果，模板继续复用。已有事实足够时只重做解释或建库，不重复启动 Engine；恢复不自动升级旧资产，源码合同变化须明确建立新基线。

运行根锁保护唯一写者，共享核心构建锁协调编译和消费。预算使用单调时钟；临时峰值按运行目录采样，不包含容器层、共享构建缓存和系统临时空间。`state.json` 的 complete 仅表示组合产物已经发布，结构资格仍由结果单独说明。

## 单步原生观察

以下命令在已经安装当前采集包的 SGLang 推理容器内执行，工作目录为仓库根。为每次观察选择新的输出目录；SDK 必须在该原生环境内构建。

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

`--tp-size N --rank R` 在单张物理卡上执行指定逻辑角色；实际请求、KV、槽位和 worker 仍由原生框架维护。物理卡由容器的 `ASCEND_RT_VISIBLE_DEVICES` 选择，worker 使用映射后的逻辑卡号。`--dtype float16` 可用于对应 FP16 模型，必须与目标资产一致。

`--input` 可读取 serving 输入中的配置与请求，但原生观察入口只接受全部 `arrival_ns=0` 的请求。不提供时使用固定代表配方；自定义目标请求不替换统一预热。输出包括 `rank-N.json`、`session-summary.json`，启用 profile 后还生成规范 `physical.json`。预加载目录须与 runtime 输出目录一致。

## 建库与目标执行

在宿主通过 modeling 容器消费来源：

```bash
scripts/markov.sh inspect --source data/YOUR_RUN/captures/source/rank-0.json
scripts/markov.sh produce \
  --source data/YOUR_RUN/captures/source/rank-0.json \
  --output data/YOUR_RUN/diagnostics/source-graph.json
scripts/markov.sh templates \
  --source data/YOUR_RUN/captures/source/rank-0.json \
  --physical data/YOUR_RUN/captures/source/physical.json \
  --output data/YOUR_RUN/assets/rank-0.json
scripts/markov.sh simulate \
  --templates data/YOUR_RUN/assets/rank-0.json \
  --input example/automatic_graph/serving.json \
  --output data/YOUR_RUN/results/serving.json
```

直接调用脚本前须已有当前核心构建，可用 `scripts/build.sh` 制作。逻辑 TP 重复传入 `--templates`，每个 rank 对应自己的资产。只有语义来源时可省略 `--physical` 建立逻辑诊断库，不能据此执行完整物理 serving。

`inspect` 报告声明的形状、条件和状态缺口；`produce` 发布公开图。模板分开自身工作定义、子调用组合布局、具体调用绑定与物理执行变体；层号、动态成员和目标实例不自动形成新定义。`simulate` 依据目标状态和完成事件持续展开，CPU 服务、设备工作、等待及输出端口各有边界。

`expand --templates ... --target ... --output ...` 用于单区域诊断。例如模型 Decode 的目标参数可包含 `M`（新增 token）、`B`（本批成员）、`R`（请求池容量）和 `max_sequence_tokens`。固定模型绑定来自资产；动态参数和子调用仍须满足模板合同。`compose` 按具名端口连接多个区域，不另建调度器。

## 辅助入口

| 文件 | 用途 |
| --- | --- |
| [probe.py](probe.py)、[state.py](state.py)、[allocator.py](allocator.py) | 观察公共操作、页池与分配机制 |
| [collectives.py](collectives.py) | 观察原生 collective 及本地接口 |
| [physical.py](physical.py) | 从保留的 trace/runtime 事实形成物理归属清单 |
| [structure.py](structure.py) | 比较独立观察与预测的批次、工作、几何、状态及端口 |
| [execution.py](execution.py) | 核对 API、事件、队列排空与 stream 完成前沿 |

这些入口读取公共结果和原生事实，不回流修改被验预测。物理工作有归属不等于全部依赖已恢复；不完整参数或关联保留 partial，不能按最近时间强配。结构来源 profiler 耗时不直接成为 CPU、设备、通信成本，dummy 权重也不提供数值或内容相关行为的正确性结论。
