# Profiling 开发与使用

真实采集统一从 `scripts/profile.sh` 进入，由配置选择 SGLang 或 KTransformers 容器。
采集结果通过 `profile_manifest.json` 交给建模，不需要手工寻找或拼接 trace。
本文讲采集合同；预测和校准编排见[建模说明](modeling_development.md)，当前证据见[工作进展](work_progress.md)。

## 1. 共同流程

```text
experiment JSON → 框架容器 → server + workload + channels
                → manifest + traces + workload 正式窗口
```

```bash
scripts/profile.sh --help
scripts/profile.sh <experiment.json> --dry-run
scripts/profile.sh <experiment.json>
```

普通采集不要求矩阵。suite 先展开、预检查全部选中实验，再严格串行执行，
避免 NPU、host 和 storage 状态跨 cell 干扰。配置与参数来自文件和命令行，不读取 stdin。
配置展开及普通模板的 workload 身份解析归 `profiling/suite.py`；base 准备和独立性能采集共用，
不在各自工作流维护另一套解析。已有源与 CPU 配对的预测路径不加载采集规划，只有缺输入时才进入采集。
dry-run 检查配置、命令及 workload 格式并写出计划，不启动服务、不清理存储、不生成 token capture bundle，
也不证明运行时缓存状态或设备语义正确。

| 参数 | 用途 |
| --- | --- |
| `--server/--input/--experiment` | 选择 suite cell 或普通配置标签 |
| `--channels` | 收窄已配置通道，用于明确标注的采集对照 |
| `--forced-token-bundle` | 给各 workload 选择同一受控 token 计划 |
| `--list-experiments` | 只列出展开的实验 |

选择支持单数/复数别名、重复参数、逗号列表及等号形式，不支持选项缩写；多次 channels 合并。
普通分组在 `metadata.config_id/workload_id` 声明标签；矩阵的 suite_server_id/suite_input_id 优先。
标签用于选择和展示，不参与拟合，也不证明实际输入相同。

### 配置与运行生命周期

矩阵按公共配置、server、input、实验覆盖顺序合并，每层应用 `$unset`，选择子集不改变原编号。
命令与环境先展开运行目录，再解析配置；未知占位字段和损坏引号报错。
显式参数数组可以避免 shell 引号歧义，不把外部参数拼成 shell 命令。

模型引用依次取顶层 `model_path`、`server.model_path` 或命令中最后一次
`--model-path/--model_path`（含等号形式）。模型 ID、相对路径原样传给 workload；
仅本地 config.json 覆盖需要解析文件路径。未声明的模型和标签不继承父环境旧值。

每个 ProfileRun 在创建时完成预检查。模板负载校验 token 范围与准备/正式阶段布局，
状态断言仍由实际运行检查。服务器只注入本次声明的探针；workload 清除服务器专用的 probe 和 hook 注入，
保留无关 Python 搜索路径与框架环境。

服务启动重试只发生在 workload 前，保留失败日志，仅清理本次启动的 run-local 产物；
自定义或外部 trace 目录不自动删除。中断会回收已启动进程，不重试启动。
显式 `model_config_overrides` 退出时恢复原始字节；恢复失败报错，并保留 `_config_backup`。
临时 HiCache 存储仅在服务器退出后按明确范围清理，清理失败也算失败，不删除 run 根目录或 trace。

suite 在启动前声明 manifest 位置，结束时记录尝试、成功、失败及未执行数。
`continue_on_error` 可在普通失败后继续，但有失败最终仍返回非零；不吞掉中断。
SIGKILL 或写盘故障可能留下 running，旧状态、路径存在或 manifest completed 都不是有效采集的充分证据。

## 2. Framework 能力

| 能力 | SGLang | KTransformers |
| --- | --- | --- |
| 独立 image/service | sglang | ktransformers |
| LD_PRELOAD | 专用 hook | 专用 hook |
| Torch 与 Python 语义探针 | 支持 | 当前采集入口不支持 |
| manifest、源 DAG 与仿真 | 共用 | 共用 |
| HiCache | 支持 | 不适用 |

不为框架不存在的模块伪造能力。KTransformers 的 submodule、installer、image、compose、hook 和 dispatch 保留；
共享建模核心不反向依赖其源码树。

```bash
scripts/build.sh sglang
scripts/build.sh ktransformers
scripts/internal/hooks/build.sh sglang
scripts/internal/hooks/build.sh ktransformers
```

hook 各用独立构建目录。CMake 缓存源码路径不匹配时，先确认并移走该旧目录再重建；
脚本不自动删除历史目录，也不忽略配置失败。
KTransformers 公开配置为 `configs/experiments/ktransformers/profiling_dag_smoke.json`：

```bash
scripts/profile.sh configs/experiments/ktransformers/profiling_dag_smoke.json --dry-run
scripts/model.sh build-dag --profile-manifest <ktransformers-manifest> --output-dir <dag-output>
```

真实推理还需 image、可用设备、模型/GGUF 和对应 hook。
dry-run 及已有共享构图证据不是新模型的真实性能采集，当前保留范围见工作进展。

## 3. SGLang 5×3 实验

原对照由五个 HiCache 配置和三个 workload 组成，不是 predictor 的硬编码合同。

```text
configs/workloads/hicache_manual/configs.json
configs/workloads/hicache_manual/w1_device_spill_qualification_ring.json
configs/workloads/hicache_manual/w2_writeback_dual_tier_cascade.json
configs/workloads/hicache_manual/w3_prefetch_admission_ladder_and_survival.json
configs/experiments/hicache_manual_workload/profiling_full_dag_replay.json
```

15 份真实 profile 支撑每个 base 的 12 cross、五个 base 共 60 cross。
开发按执行语义挑关键 cell，不因每次改动都重采全矩阵：

```bash
scripts/profile.sh configs/experiments/hicache_manual_workload/profiling_full_dag_replay.json \
  --server <base-config> --input <workload> --forced-token-bundle <bundle> --dry-run
```

## 4. Forced-token capture 与 replay

受控 token 是复现实验选项，不是普通预测必须人工串联的第二套流程。
它固定真实请求顺序、输入和输出 token，使不同配置看到相同 workload。
组的首次准备可从普通模板命令自动安排记录与回放，不要求手工补 replay 参数；
下述命令用于单独采集与复用，具体选择由[组准备入口](modeling_development.md#首次准备)组织。

```bash
scripts/profile.sh configs/experiments/hicache_manual_workload/no_profile_capture.json
scripts/profile.sh configs/experiments/hicache_manual_workload/profiling_full_dag_replay.json \
  --forced-token-bundle <capture-suite>/forced_token_bundle.json
```

capture 只在全部请求及正式窗口成功后写计划，不覆盖已有计划。
真实输出必须达到 max_new_tokens；HTTP 成功但被容量裁短不是合法 bundle。
replay 核对 workload、request order 和完整 token 数组。单次 workload 可直接指定
`--forced-token-plan`，不要求 suite 或额外 bundle；占位符 `{forced_token_plan}` 才需要 bundle，
bundle 与显式 plan 同时提供时报错。

模板的请求、重复请求、barrier、checkpoint 共用展开和预算逻辑。
正式边界对应展开后的第一、最后正式请求。响应 token 在 HTTP 边界解析一次；
capture bundle 的计划路径来自 manifest 所声明的 workload 报告，不存重复响应和校验副本。
自定义普通请求可使用 prompt_token_ids，不要求人工 anchor/tail 或状态 checkpoint。

有状态模板默认 `--checkpoint-mode verify`。
新配置可显式用 observe 保留 barrier、启动配置核对和缓存观测，只不强求旧状态断言；
报告 passed 为 null，不把运行成功算作断言通过。`--require-hicache-state` 仍是显式状态约束。
checkpoint 的 token 路径摘要用于服务端语义核对，不是产物版本控制，不采 cache snapshot。

ShareGPT 本地语料通过同一 workload 入口转化：

```text
--sharegpt-dataset <json> --sharegpt-policy <policy.json>
--tokenizer-path <model> --output-dir <inputs>
```

按实际 tokenizer 选样、不裁文本。语料与 token 数组留在 data，选样规则在 configs；
预热、正式、独立校准及确认会话分别声明。预热不计正式窗口，但缓存不会隐式清空。
旧 trace 不因新增语料或探针自动获得缺失字段。

## 5. 共享校准的采集语义

base 提供源结构与未变成本，变化工作使用离线独立成本包。
`prepare-hicache` 只有遇到实际成本缺口、显式开启且有累计预算时，才选择整组共享实验；
不自动把当前 base 入库拟合，不读取 target trace、评分或逐格误差。

suite、token 和 profile 均写入当次组目录；失败、超时、无效输出和重试都计入累计账本。
缺少实际用量的失败按预留请求/token 计费，不记为零。HTTP 成功不证明 trace、状态或成本可辨识。
候选、预算和恢复见[建模说明](modeling_development.md#6-准备流程与失败处理)，这里不再维护另一套补采规则。

同源 CPU 轻量配对保留 base 的实际命令与 token 计划，仅改变采集方式。
开启已有轻量 I/O 计时后，一趟回放也可记录预取服务，不额外加请求；
使用前仍需核对实际 I/O 工作量。受预算管理的轻量回放每次只启动一次服务，不使用普通启动重试。

物理采样不运行 target workload，页宽域由采样声明，不从 target 列表倒推。
DMA 的 `runtime-dma` 入口需要 base-report 提供模型几何、设备和 NUMA 信息，
当前只支持两字节 KV；原始 DMA 样本保留，报告合入原始测量来源，不拟合或保存服务系数。
存储报告保留采样域、所选工作点及测量条件，预取保留阶段计时；原服务的来源继续保留，
补采成功只表示完成测量。成本参数及辨识缺口由独立数据库的离线训练处理，不由采集报告决定。
固定 device-capacity-tokens 可保持真实缓存池的 shape/stride；缺省按最大操作分配，不等价于实际布局。
文件预取 sampler 记录实际框架分段时钟，不再在 worker 内启动 Torch profiler。
空闲同步及各类计时原语的适用范围见成本文档，采样成功不表示整组成本已齐备。

## 6. Trace channels 与轻量探针

SGLang 的 Torch 记录设备及框架工作，LD_PRELOAD 记录 CPU/API/I/O 时序，
Python probe 补充执行语义。后者不是额外一份可叠加的执行成本。

Python probe 默认 snapshot-free，只读现有 Python 标量、容器长度与 tensor 元数据，
不读取 tensor 内容、不复制缓存、不增加设备同步。DAG consumer 即使 diagnostics=off，
也需要请求响应、CPU 通信、层等待、分配及 phase 边界；只做 state/input-contract 时不安装这些 DAG 观测。
manifest 的 python_runtime_probes 记录选择，实际覆盖仍由建模检查。

缓存查询同时记录缓存树与控制器的对象关系，不需要 snapshot 或实际写回/回载来建立身份。
建模只接受这类直接关系或明确的调用/请求对应，不按最近时间或“只有一棵树”猜测。

### 正式观测分别说明什么

| 观测族 | 用途与主要边界 |
| --- | --- |
| HiCache fact | workload 身份、源结果和时序分开路由；源命中或 true/false 不替目标选择分支 |
| load/write completion、write_ack | ACK 与在途队列、备份和发布；设备完成、缓存可见及引用释放分开 |
| prefetch query/check/stop/read/publish | operation、请求、实际读取与已接受更新；stop 设标志不等于后台线程退出 |
| storage drain、host/device release | MIN 后的数量上限、页和释放分支；无 MIN 上限为 None，不用源计数代表目标队列 |
| extend/decode allocation、allocator | 外层请求/长度与内层队列、dtype、页几何及实际分支分开，不读分配索引 |
| layer_waits | 每 batch 缓冲逐层区间，结束统一写出；未启用 consumer 的空列表不证明没有调用 |
| attention、attention_metadata | 分别描述逐层计算和每轮页表；记录 FIA/MLA/graph 等条件，不代表特殊分支都已支持 |
| dense、Embedding、normalization | shape、stride、dtype、实际模型角色；父包络与内部算子不重复收费 |
| rotary、Q/K reshape、qkv_norm_rope | 旋转布局、共享视图与真实融合路径；共享表归属当前起始层，不按最后登记层猜测 |
| input_buffers、logits、sampling | eager 复制、末行选择、输出整理与贪心采样各自归属；未知处理不当作普通分支 |
| device_collective、cpu_collective | 参与者、载荷、组内次序及外层角色；序号相同本身不保证跨 rank 对应 |
| Triton prepare/load/kernel | 冷准备、句柄装载与真实提交；warmup 排除，父区间不可与内部准备相加 |
| request/response | 入口、广播后就绪、序列化、最后 ASGI body send；最终真值仍由客户端 bench 提供 |

缓存写入保留 K/V 的 shape 与 stride；同元素数的连续张量和切片不共用描述。
descriptor_id 只是同一采集内的引用，不是版本或摘要证明。
Qwen3 角色由实际模块登记，其他模型可提供测量几何，但不会自动获得 Qwen3 的目标布局规则。
普通 NPU Linear、SwiGLU、RMSNorm 和已观测融合入口不冒充量化、CPU、ROCm 或未知实现。

Attention/metadata/dense 共用 `thread_timing.invoke_runtime_call`：
先记录结束墙钟与线程 CPU，再提取返回元数据、写事件；异常原样传递并记录 raised。
其他探针有不同边界时保留独立实现，不为统一代码改变测量口径。
当前这轮计时重构尚未重新实采，不能宣称探针扰动已经重新验证。

### 所有者、返回值和通信

HiCache 同步控制观测保留 returned/raised，不伪造完成量。
后台查询可早于 enqueue 返回，check 入口不是共享状态读取的精确时刻；
不能用包络端点直接替代入队、状态读取或后台退出。
read 与 service 通过同 pid/tid 的唯一包含关系关联，外层已经包含读取与写入，不能重复计费。
两次 publish 之间还可能有 Python、probe 和调度工作，不等于纯拷贝。

释放通过 operation 身份、同线程顺序或明确请求上下文归属，不按最近时间猜请求。
`allocator_free` 仅在真实设备释放分支内记录，父调用与内层范围不相加；
旧 trace 没有边界时保留限制，不补造零成本。长度列表不截断，也不从 batch 总量反推缺失前缀。

Gloo 与 NPU 通信分别保留组名、成员、rank 和观测次序；不同名称的组不自动合并。
失败及合并提交也占序号，async_op 只测到提交返回，不当作通信完成。
进程底层序号会受到未观测 barrier 等影响；跨 rank 对应仍需完整覆盖与参数核对。
同栈、同组、同操作的兼容重复包装只记录外层一次，嵌套及异常后恢复上下文；
不同操作不跳过，后台线程不继承主线程角色。缺后台设备 lane 不能记作零服务。

Triton prepare 区分 sync/async/unknown；只有观察到真实 IR 入口才标 compiled，
同步返回匹配内核且确认 IR 包装生效、未进入 IR 才标 disk_cache。
异步是提交区间，不按耗时推断路径。TRITON_ENABLE_TASKQUEUE 使用实际 launcher 开关语义：
关闭队列是直接提交并等待，不是同一异步结构换一个耗时。

请求入口/响应探针不读 token 正文。socket_received 是轮询与反序列化返回，不是网络包到达；
dispatch_ready 在广播和输入处理之后；tokenizer_submit 返回不是送达 ACK；
http_body_sent 是最后 ASGI send 返回，不是客户端已收齐。
建图可在唯一、未改写的 CPU gap 中插零耗时连接点并分段保留原 gap，
歧义、跨界或缺 HTTP 必要观测时不猜连接，也不用整图末端冒充客户端完成。

### 诊断开关

默认关闭的诊断不能与轻量采集混作同条件成本样本，也不能自动当作可删除开销。

| 开关 | 观测与限制 |
| --- | --- |
| `profiling.python_probe.diagnostics: timing` | thread CPU、等待、缺页及 schedstat；不可读字段为 unavailable，wall−CPU 不是纯阻塞 |
| `HOOK_SYNC_THREAD_CPU=1` | 同步 hook 的线程 CPU，含轮询及 wrapper 工作，不等于设备等待 |
| `HOOK_EMISSION_TIMING=1` | 后继同线程事件携带上次日志写出的区间/CPU；末次可能缺测，不覆盖全部 hook |
| `HOOK_MEMCPY2D_TIMING=1` | 几何、方向、stream 和调用时钟；入口格式化仍有开销，需对应部署库 |
| `SGLANG_STEP_TIMING_DIR` | 已有 forward 步骤计时；不能称为零扰动或“仅 I/O” |
| `SGLANG_SCHEDULER_TIMING=1` | 活跃调度、结果处理、接收与批次选择，父子区间需去重 |
| `SGLANG_HICACHE_HOST_TIMING=1` | write_backup/load_back/start_loading/add_one_req；无直接请求身份需精确关联 |
| `SGLANG_PREFETCH_TIMING=1` | query/check/stop、完成检查、存储处理及 CPU 通信包络 |
| `SGLANG_HICACHE_IO_TIMING=1` | 文件预取的 read 服务、发布计数与 CPU affinity；发布不等于逐页 copy |
| `SGLANG_STEP_TIMING_EMISSION=1` | 后继同线程记录携带上次序列化/写出，仍未覆盖全部计时入口 |
| probe catalog 的 `capture_emission_timing` | 绑定、提取和事件写出；不含诊断自身和全部 wrapper |

探针诊断只保留 off/timing，开发期 cProfile 与 full 档已删除；
modeling 的 diagnostics full 是另一层报告开关，不受影响。
schedstat 每次读取及时关闭文件，诊断会增加打开/关闭开销；不把未知计数填零。
同线程、同 stream 的嵌套 ACL/runtime 同步 hook 只记录外层一次。
任何开销扣减须按原始线程和区间去重，只用有依据的 CPU 成分，不直接扣日志锁等待墙钟。
新版 hook 或探针不会自动更新已有部署库与旧 trace，构建成功也不证明真实覆盖。

## 7. Formal window 与时钟

workload report 提供语义起止，manifest 的 bench.workload_report_files 声明路径。
显式 input.workload_report 优先；多份报告需明确选择，缺声明不扫描运行目录猜测。
bench-serving JSONL 的总时长没有 trace 时钟起点，不能构造 [0,duration] 裁剪窗口。
无窗口的普通 build-dag 可读整份 trace；需要正式窗口的预测/校准必须有带时间戳的报告。
已经声明但不完整的窗口不能退回整个请求包络。

窗口内执行进入 DAG；窗口前 token 字典只解释路径，前序事实只初始化状态。
与已准入 async 操作精确对应的尾部 ACK/release 闭合生命周期，不独立延长 E2E。
不按配置、cell、事件白名单或固定微秒范围硬编码窗口。
固定校准可在准备阶段用 barrier/checkpoint 建状态，正式窗口中只放请求。

NPU layer_waits 使用 Torch syscnt，不直接与 Python time.time_ns 比较。
manifest 每份 Torch trace 的 host_clock 分别保留 vendor 的 origin_tick/origin_ns/ns_per_tick，
以及 framework 的 unix_origin_ns/unix_reference 墙钟锚点。
建图在通道合并和裁剪之前换算，原始 trace 不变；syscnt 等待区间用同一参考转换。
没有参考的旧 manifest 保留 profiler_ns 解释，不算已对齐。
只能用同次原始时钟证据补锚点，不能按 cell 身份或误差拟合偏移。
锚点换算不消除采样误差、漂移或时钟矛盾；后续仍核对包含关系与依赖。

长 trace 可显式启用 `SGLANG_NPU_PROFILER_SERIAL_EXPORT=1`，只串行停止后的 rank 导出，
不改变请求或参数。搭配 torch.strict_stop 和足够的 stop_timeout_sec；
导出不与重型 DAG 建模并发。宽松停止可能把 API 超时只写为 warning，
manifest completed 和文件存在不能替代完整性检查。
不完整导出优先从保留原始数据离线恢复，不能当作成本误差拟合。

## 8. 产物与开发位置

```text
<cell>/
  profile_manifest.json
  config.json
  server_cmd.txt / bench_cmd.txt
  bench/<workload>/workload_report.json
  trace/{torch,ld_preload,python_probe}/...
```

Torch/hook 默认在 run 内；自定义相对路径以 run 为基准，绝对路径原样使用。
hook 收录指定前缀的 .rank*.pid*.json。自定义目录专用于当次采集，外部目录不自动清理。
manifest 只声明实际配置、文件、采集状态与窗口，不存内部版本、digest、冻结副本或 checksum。

正式输入须确认：启用通道完整、真实 workload 成功、受控 token 一致，
以及消费者所需语义与生命周期齐备。声明文件缺失、exists=false 或 JSON 损坏时报错，
不扫描其他文件补全。流式 trace 只允许补齐数组/对象结尾，不删除残缺事件；
历史诊断可以记录坏文件继续，但不能据此准入正式建模。

| 位置 | 修改职责 |
| --- | --- |
| `scripts/profile.sh`、profiling/runner | 薄入口、容器与 suite 调度 |
| profiling/executor、runtime、environments | 单次生命周期、命令、目录与环境 |
| `src/profiling/config.py`、manifest.py、profiler_clock.py | 配置规整、交接文件与时钟证据 |
| Python probe bootstrap/patching/schema | 安装、重复包装与事实路由 |
| probes/values 与 probes/hicache | 参数/返回值读取与 HiCache 语义字段，不负责安装或写事件 |
| probes/ 与 hicache_probe_targets.json | 实际框架边界、必要元数据和调用时钟 |
| LD_PRELOAD targets/framework | 原生 hook 与轻量计时 |

增加观测前确认下游真正消费的字段。安装时解析签名、同次开始/结束各绑定一次，
多个 target 共享参数映射但不共享事实上下文；属性读取一次，避免 getter 改变采集语义。
模块名匹配可缓存，安装成功不永久缓存：后续导入、部分初始化与函数替换仍检查。
通用 callable 与固定 HiCache 提取器共用安装，不再维护可变插件注册表和转发层。
catalog 选择与运行时安装共用 `schema.parse_target`，字段、条件和 fact 规则只维护一份；
字段或条件格式错误在采集预检查时报错，不必等服务器启动。安装输入按模块和 callable 一次分组，
仍保留同一函数上的 target 顺序及每次导入后的重查。
`probes/values.extract_value` 处理参数、返回值、路径和组合表达式，HiCache 前缀由固定分派处理。
token 提取器不再依赖 callable 安装器；摘要 list 与完整 token 字典仍分别取值，缺失不填零。
probe 导入不得初始化设备或启动运行；新增包装不得读 tensor、snapshot 或额外同步。
真实覆盖、额外扰动及支持范围须通过正式采集验证，不能以字段存在或构建通过代替。
