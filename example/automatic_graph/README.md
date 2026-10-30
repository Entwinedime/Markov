# 自动取图与条件 Serving 示例

输入一个固定模型、配置集合和请求集合，流程自动选择代表来源、观察必要逻辑角色、建立参数化模板，再按目标状态动态展开。模型使用完整结构的 dummy 权重，节点耗时来自显式假设；输出用于检查执行结构，不构成真实性能预测或精度结论。

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

`MARKOV_IMAGE_PREFIX` 可选择其他已准备好的前缀，默认 `markov-next`。脚本在 modeling 容器内组织任务，通过宿主 Docker 创建本任务的原生容器；采集前会构建当前 C++ 核心和任务私有 SDK 观察库。物理设备只选择采集位置，TP 本地分片不要求现场拥有目标数量的卡；PP 采集使用实际 TP×PP 参与进程。

正式探索在任务准备阶段将挂载的Python采集源码安装到隔离原生容器；直接使用镜像入口则运行镜像内的包。阶段交付时重建`sglang-profile`，使镜像快照与源码一致。使用本机代理且构建网络不可达时，可通过已有的`MARKOV_BUILD_NETWORK=host`选择构建网络；运行镜像和依赖仍由同一Dockerfile制作。

配置可用 `"placement": {"devices": [4, 6]}` 指定 TP2 的两个目标设备，省略时按 rank 绑定 `0..TP×PP-1`；当前要求单机、设备互异，纯预测不检查现场卡数。它与采集用的 `deployment.physical_devices` 独立，结果的 topology 会列出实际绑定和参与组；设备标签不自行产生带宽或争用成本。

PP 配置可增加 `"pp": 3, "microbatch_requests": 2`；部署的 `physical_devices` 需要容纳该来源组的 TP×PP 参与者，placement 按 stage 内 TP rank 排列。当前 Ascend 实现不支持 PP2。每域的一次 PP 会话启动全部必要参与者，代表发布观察，其余角色使用独立绑定；目标使用同一控制器维护多微批次和唯一 KV 状态。

一级缓存入口示例为 [prefix_cache.json](prefix_cache.json)：`example/automatic_graph/run.sh --input example/automatic_graph/prefix_cache.json`。输入固定Qwen3-8B、TP1，选择原生控制及`prefix_cache=true`，两种容量/并发与两份共享内容请求集共四个目标。采集适配器在一份代表会话中分组观察缓存机制，用户不指定组批。结果的`resources`允许缓存页留存；终态守恒不代替中途引用或设备内容就绪验证。MoE还需提供对应EP拓扑的路由条件，缓存键同时比较token与逐位置专家条件；路由分歧只保留此前的共同前缀，模型输入token不变。

P/D 分离使用 [disaggregated.json](disaggregated.json)：`example/automatic_graph/run.sh --input example/automatic_graph/disaggregated.json`。确认其中两张采集卡可用；目标在两池内分别声明 `tp` 与容量，示例仍为双方 TP1、PP1。P 每次处理一个完整 prompt，D 预约后接收 KV，再发布首 token 和继续 Decode；placement 先列 P 的全部 rank，再列 D 的全部 rank。源页在传输确认后释放，待合并页在下一次分配需要时通过原生模板整理；结果 `resources` 分别报告各池容量、空闲页/槽位和缓存持有量。对称/非对称 TP、双方 PP4 和 MoE 双方 TP=EP2 已有有限开发核对；原生要求 Decode PP 等于 Prefill PP 或1，来源限制和独立验证边界继续保留。

MoE/EP 使用 [expert.json](expert.json)：`example/automatic_graph/run.sh --input example/automatic_graph/expert.json`。示例为完整 Qwen1.5 MoE、TP2/EP2，需两张实际采集卡；请求路由按层和逻辑 token 区间给出，padding 路由单独声明。它是给定路由的条件结构，仍执行 gate/top-k 等工作，不能从长度推断真实内容路由；缺少原型或来源覆盖为 partial 时如实报告。

示例预算是有限上限，不保证任意模型或配置都能在其中完成。初始化、统一预热、观察、导出、处理和失败均计入累计额度；额度不足时保留状态并停止，不隐式扩大预算或循环补采。

## 来源复用与结果含义

每次任务固定一个模型。dtype、TP、PP、EP、attention backend、page_size及chunk、mixed、prefix cache机制相同的配置共享来源；每个域保留实际逻辑 rank 的本地分片。可隔离 TP 先取得必要根角色，由实际模块和原生词表分片 API 规划其余代表，再为等价 peer 生成独立绑定。原生组仍启动协议所需的参与者，模型加载后比较模块布局和角色职责，仅选代表观察与导出；PP4、EP4 复用、TP2×PP3×EP2 及限定多 rank PD 组合已有开发核对。`inputs/role-plans` 和结果的 `role_source` 区分观察与派生。不同模型或域分别建立来源，不以相似 shape 自动替代所有职责，也不声明同库跨 TP 或跨页大小变换。

同域内根据请求、并发、KV 容量与 Prefill 预算重新组批、求值几何、重复成员子图和连接端口。原生实现变化由已声明的条件和局部原型选择；超过模型上下文、参数范围或原型覆盖时保留明确失败。正式入口提供 SGLang/Ascend，单卡辅助通信只提供结构接口，不提供真实跨卡时延。

一键探索默认使用轻量控制器，基线为 FCFS、分开的 Prefill/Decode、固定输出长度和冷逻辑 KV。先用下文命令构建control镜像，任务顶层设置 `"control": "native"` 可使用原生控制；已有模板也可通过下文的原生控制入口运行。原生代码决定组批及PP/PD推进，核心管理KV资源和模拟完成反馈，两种控制不保证产生相同批次。

共同实例配置可加 `"prefill_chunk_tokens": 128`，PD放在 `pd.prefill` 内，要求原生控制、正整数且页对齐。chunk机制单独建来源域，目标chunk长度在域内变换；中间块保存KV索引并继续下一块，最终块才生成输出。单worker、dense TP2、MoE TP2/EP2、dense TP1×PP3、P1/D1、P TP2/D TP4、P/D各TP2/EP2及P/D各TP1×PP3已有分块开发核对。

`"mixed_batches": true`用于共同实例，要求原生chunk控制及对应的mixed来源。当前官方FA环境下，Qwen3-8B的TP1、TP2和PP3已完成包含一级缓存的开发闭环；有限到达案例核对了就绪Decode的预算保留及后续Prefill完成。Qwen1.5 MoE的一个TP2/EP2补齐mixed批次被原生attention拒绝，该组合没有合格mixed来源；非mixed chunk/cache已有开发证据。以上不授予任意负载公平性或独立结构资格，overlap尚未开放。

请求输入使用可复现的合成内容：可用workload的`prefixes`（`id`、`parent`、`tokens`）及请求的`prefix_id`声明共享部分，其余prompt和生成后缀互不共享。采集保留原生采样工作，再注入与预测相同的输出token；这是条件执行结构，不是真实文本生成。内容字段本身不启用缓存；共同实例配置或`pd.prefill`显式设置`"prefix_cache": true`，并选择原生控制及同机制来源。一级缓存已接通限定开发范围；PD仅Prefill侧缓存，Decode仍通过KV交接。单worker可与chunk组合，其他机制组合仍须各自取得证据。

统一预热使用与目标序列分开的固定五请求，并按原生 ATB 上界预留 workspace；预热不计入目标服务时间，但计入采集费用。当前 runtime 规则针对 CANN 9.0/Ascend910B 的 dense 路径，workspace 预留限制为相等且不大于 128 的 head 维度。新的并行chunk/缓存组合、overlap、speculative 或其他内容相关路径需要各自的实现和证据。

给定耗时改变后，完成事件会改变后续组批。一次运行完成、条件结构可展开、独立原生对照相符和性能精度通过分别判断，不能相互代替。

## 输出与恢复

每次运行创建一个 UTC 时间目录并打印位置：

```text
data/<UTC时间>_automatic-graph/
  inputs/                       原任务、环境身份、角色规划及逐组合输入
  captures/<角色-尝试>/         会话摘要、操作来源及规范物理事实
  assets/<实现域>/templates.json  共享定义/布局/执行模式与独立角色绑定
  results/summary.md             人读入口、失败与累计费用
  results/<配置>/<请求集>.json   默认的精简组合结果及逐请求结果
  diagnostics/                  --details 时的完整组合结果
  logs/run.log                  子进程命令与失败原因
  state.json                    任务状态、资产位置及费用
  work/                         SDK、观察库与尚未发布的草稿
```

默认保留后续消费需要的规范来源与模板，随状态提交释放无引用历史。PD来源另保留每rank的`transport-rank-N.json`注册端点/地址，以及`rank-N.json`中实际复制的地址片段，用于对应原生rank、层、head和页；进程地址不进入模板或目标身份。首次运行加 `--details` 才保留完整图、逐批/逐 token 记录和原始 profiler/runtime 导出；完整组合结果改写到 diagnostics。全部域资产发布后释放私有 work，即使目标结构仍为 partial。数据和构建产物不提交 Git。

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
cmake -S src/collection/native/runtimes/ascend \
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
  --output data/YOUR_RUN/assets/templates.json
scripts/markov.sh simulate \
  --templates data/YOUR_RUN/assets/templates.json \
  --input example/automatic_graph/serving.json \
  --output data/YOUR_RUN/results/serving.json
```

直接调用脚本前须已有当前核心构建，可用 `scripts/build.sh` 制作。逻辑 TP 在一次 `templates` 中重复 `--source`，每项后接对应的 `--physical`；随后向 `simulate` 传入这一份来源域模板。共享 catalog 只保存一份相同结构，roles 保留各 rank 的独立入口和资源绑定；旧内部资产需从来源重建。只有语义来源时可省略 `--physical` 建立逻辑诊断库，不能据此执行完整物理 serving。

切换为无设备原生控制时，先从当前推理镜像构建控制镜像，再消费同样的模板与目标输入：

```bash
docker compose -p markov-next-redesign -f docker/compose/inference.yml build control
bash scripts/markov-control.sh \
  --templates data/YOUR_RUN/assets/templates.json \
  --input example/automatic_graph/serving.json \
  --output data/YOUR_RUN/results/native-serving.json
```

启动端需要Python≥3.10和Docker；它连接独立的原生控制与C++执行容器，不挂载设备、权重或目标观察。原生模型及拓扑配置取自模板；结果发布前等待全部参与进程成功退出。`--details`保留图和动作明细，`--timeout-seconds`限制本次运行时间。该入口检查已采模板下的闭环，不自动补采，也不代表独立结构对照通过。

`inspect` 报告声明的形状、条件和状态缺口；`produce` 发布公开图。模板分开自身工作定义、子调用组合布局、具体调用绑定与物理执行变体；层号、动态成员和目标实例不自动形成新定义。`simulate` 依据目标状态和完成事件持续展开，CPU 服务、设备工作、等待及输出端口各有边界。

`expand --templates ... --target ... --output ...` 用于单区域诊断。例如模型 Decode 的目标参数可包含 `M`（新增 token）、`B`（本批成员）、`R`（请求池容量）和 `max_sequence_tokens`。固定模型绑定来自资产；动态参数和子调用仍须满足模板合同。`expand`/`compose` 的目标可用 `source_rank` 选择来源角色，默认 0；`compose` 按具名端口连接多个区域，不另建调度器。

局部诊断核对给定输入条件下的模板行为，派生输出由模板计算；完整serving还需核对控制器怎样产生这些输入，以及跨调用的资源与完成关系。

## 辅助入口

已有探索任务可以继续对指定目标做原生结构对照：

```bash
example/automatic_graph/run.sh validate \
  --run data/YOUR_RUN --configuration larger --workload together \
  --capture captures/target-larger-together
```

配置和请求集ID来自该任务的输入。入口先用已发布模板计算默认/明细结果，再观察全部真实worker，逐rank比较；目标观察不参与建库。原生入口要求请求同时到达，所需物理卡由原任务deployment提供，预算继续使用该任务的累计额度，必须事先覆盖来源与目标运行。已有capture仅在模型、拓扑、请求、规定的池参数及路由一致时复用，已有案例报告拒绝覆盖；不自动重试。

案例汇总保存在`results/validation/<配置>/<请求集>/<capture名>.json`，逐rank明细及执行/路由证据在旁边同名目录，原生事实保存在指定capture目录。原生证据逐行核对workload专家ID与实际通信split、专家计数；批次人数取实际Prefill/Decode执行区域。预测证据检查节点完成、依赖就绪、资源独占、动作排空、交接边及输出序列。普通失败保留错误并继续其他rank；预算耗尽与中断单独记录，保留尚需处理的临时材料。完成对照后删除消费过的大图，结构差异或partial均不代表性能精度结论。

`matrix.py`维护当前S8调查的具名输入配方。已有模型来源元数据的准备目录可用`run.sh matrix --run data/YOUR_MATRIX`生成任务与边界输入；它只写输入，未运行模型，且拒绝改写已冻结清单。每个来源域的正式输入位于`runs/<域名>/inputs/task.json`，后续通过同一正式workflow启动，批量调查编排和汇总继续留在example。

审查完成后在清单中记录`frozen_revision`、两个执行镜像的ID并将`status`设为`frozen`，再执行：

```bash
example/automatic_graph/run.sh investigate --run data/YOUR_MATRIX
```

批量入口核对冻结代码与镜像，先运行全部正式来源/预测任务，再执行预选原生对照、边界输入和生命周期案例；不根据目标结果补模板。整轮与各任务分别保留时间、尝试及存储上限，原生启动前检查声明卡的实际进程，已有占用或无法确认空闲时不启动Engine。独立案例出错后继续无依赖案例，缺来源的案例记blocked；预算停止/中断保留历史与费用，原命令可继续未完成案例，完成的结果不覆盖。

`results/summary.md`为整轮入口，`results/outcomes.json`保留逐次结果，`runs/<域>/`仍采用正式目录结构，额外边界及生命周期证据在`cases/`。来源SDK/runtime原始导出消费后移到原capture的`evidence/`中，避免默认清理且不复制；规范事实保留采集时路径，需检查原始文件时使用该归档位置。完整临时图消费后删除，只保留精简证据。整轮结束不自动判为结构全部通过，也不自动生成窗口审查结论。

| 文件 | 用途 |
| --- | --- |
| [probe.py](probe.py)、[state.py](state.py)、[allocator.py](allocator.py) | 观察公共操作、页池与分配机制 |
| [collectives.py](collectives.py) | 观察原生 collective 及本地接口 |
| [physical.py](physical.py) | 从保留的 trace/runtime 事实形成物理归属清单 |
| [structure.py](structure.py) | 比较独立观察与预测的批次、工作、几何、状态及端口 |
| [operations.py](operations.py) | 共用操作、端口及数据依赖核对，独立于serving配对和调度 |
| [execution.py](execution.py) | 核对 API、事件、队列排空与 stream 完成前沿；共用依赖路径查询，保持完整图中的跨rank/flow路径 |
| [evidence.py](evidence.py) | 检查预测时间线、输出及原生实际路由与组批证据 |
| [request_state.py](request_state.py) | 核对共同动作顺序、请求输入范围和输出进度；另核对终态资源及逐rank索引绑定，不重演独占页池 |
| [resource_history.py](resource_history.py) | 核对所有者实际分配、共享绑定、传输、释放与终态，以及页归还时已提交模型/传输的设备消费者是否结束；不选择缓存策略 |
| [operands.py](operands.py) | 按当时资源绑定核对SGLang模型入口的KV位置、槽位和长度；检查同rank/layer的attention状态存储及跨动作设备路径 |
| [validation.py](validation.py)、[matrix.py](matrix.py)、[investigation.py](investigation.py) | 单案例对照、具名输入和冻结批量调查；复用正式workflow并与核心业务分开 |

这些入口读取公共结果和原生事实，不回流修改被验预测。物理工作有归属不等于全部依赖已恢复；不完整参数或关联保留 partial，不能按最近时间强配。结构来源 profiler 耗时不直接成为 CPU、设备、通信成本，dummy 权重也不提供数值或内容相关行为的正确性结论。

预测状态核对消费当前程序含`resource_history`的`--details`产物，以实际提交/完成序号处理同时间戳事件。请求检查统一核对到达、连续Prefill输入段、撤回重算及输出进度，首段可从已绑定的缓存前缀开始；中间PP结果不覆盖下一块已预约的范围。资源检查从实际变更核对分配快照、共享前缀、引用、淘汰、传输页对应及图完成后的回收，并与终态身份对照，不重演另一套分配/缓存策略。

资源历史只在details下生成，不记录完整token内容；按动作提交时的资源绑定与实际设备区间核对页归还晚于模型/传输消费者完成。另沿各设备队列的末端工作核对到释放动作完成端口的依赖路径，这项检查不依赖给定耗时，覆盖的前沿数记录为`release_device_frontiers`。报告的matched仅表示已声明工作和资源记录自洽，不证明设备消费者已全部采到、实际KV数值或原生目标等价。旧明细缺少资源历史时须从保留模板和输入重新预测，旧报告保持原结论，不维护绕过资源检查的兼容路径。

`model_operands`进一步核对模型入口的整型内容与当时实际页绑定。attention按接口声明的持久读写存储检查同rank/layer绑定稳定，并检查前次设备完成至后次设备入口存在路径；这项路径检查不依赖某组节点耗时。它不执行backend内部的block table计算，也不验证KV浮点数值或PD搬运的逐字节地址；这些边界不能被入口索引一致或整存储依赖掩盖。

执行核对还沿目标图每个输入值的实际producer，检查生产调用的CPU返回能否到达使用调用的入口，报告在`value_production`。该项保证值描述符先产生再使用，不按原生批次顺序替换目标生产者；设备内容就绪及共享存储读写保护属于另外的关系，不能仅凭描述符检查就授予这些资格。

单个flow的完成关系比较只证明所选操作范围内的边。等待若涉及起点之前或上一flow的工作，会分别报告外部完成边及未绑定前沿，保持partial；完整窗口及控制交接另行核对。范围内缺边、多余边或外部资源连接错误仍判为差异。

整窗口根据已配对flow内的调用建立操作的一一对应，并核对这些调用之间的数据依赖、公开端口及事件/队列/stream关系。核对始终使用原预测图及其依赖，批次和图文件中的操作排列另行报告。覆盖不完整时不能判为matched；已对应部分存在差异时仍报告differences，不能因其他工作未配对而跳过检查。未配对工作及其因果仍未验证，结构核对不授予原生调度策略等价资格。

原生适配器可以在实际控制调用边界声明`control_only`。`controller_scope`单列这些调用及操作数；只有没有跨范围数据依赖、设备/runtime动作和共同通信合同时，比较视图才分开这部分控制工作。声明不成立时保留操作并报告差异；未标记的工作不自动排除。原图、模型通信、准备/释放和等待关系保持原样，状态轮询与不同策略的原生开销不因此获得预测资格。

Stream按每个操作内部的有序工作取得唯一对应，重复API按同一owner内的发生序号绑定真实节点。跨owner的排列由`same_stream_order`单列；必要等待前沿、事件连接和队列排空仍按实际图核对。

局部flow的必要完成关系在完整目标图中检查可达路径，`transitive_completion_links`报告经其他节点完成的关系。范围外前沿必须属于原生实际等待的stream，涉及外部工作时仍为partial；错误stream边界和额外内部完成关系继续报告差异。
