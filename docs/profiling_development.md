# Profiling 开发与使用

真实采集统一从 `scripts/profile.sh` 进入，配置决定使用 SGLang 还是 KTransformers 容器。
采集结束后，将 `profile_manifest.json` 交给建模流程；不需要手工寻找和拼接 trace 文件。

## 1. 共同流程

```text
experiment JSON
  -> framework adapter
  -> framework container
  -> server + workload + enabled channels
  -> manifest + traces + formal window
```

常用命令：

```bash
scripts/profile.sh --help
scripts/profile.sh <experiment.json> --dry-run
scripts/profile.sh <experiment.json>
```

`--server/--input/--experiment` 用于选择少量 cell；`--channels` 覆盖采集 channel；SGLang forced replay 使用
`--forced-token-bundle` 选择矩阵回放计划，普通单次回放也可在 workload 命令中直接指定 `--forced-token-plan`。
profiling cell 严格串行，避免设备、host 和 storage 状态跨 cell 干扰。
字符串命令的引号必须完整；解析错误直接报告，不按空命令或缺失 server 参数继续。参数数组形式可避免 shell 引号歧义。
模型引用在准备运行时选定，workload 环境和临时模型配置覆盖共用它：顶层 `model_path` 优先，
其次 `server.model_path`，否则取命令中最后一个 `--model-path` / `--model_path` 值（也支持等号形式）。
模型 ID 和相对路径原样传给 workload；只有实际覆盖本地 config.json 时才解析文件路径。
未声明的配置名、workload 名和模型引用不会从父环境继承旧的 `TRACE_SIM_PROFILE_*` 值。

普通采集不需要矩阵。需要将同一 base 的多个 workload 归组、进行 forced-token 回放或独立评分时，
在配置中明确 `metadata.config_id`（配置名称）和 `metadata.workload_id`（工作负载名称）。
同组 workload 共用 config_id，各自的 workload_id 与 token 计划中的名称一致。
矩阵生成的 suite_server_id/suite_input_id 仍优先作为矩阵轴标签；不要同时指定相互冲突的普通标签。
标签用于选择输入和展示，不参与成本拟合，也不能证明两份输入的实际配置相同。
`--server/--input` 也接受普通实验的 config_id/workload_id，与组补采、token bundle、
workload 环境和 manifest 读取共用同一标签选择规则，不必为选择实验另建矩阵。

### 配置、执行与失败处理

宿主机按配置选择容器，以参数数组传递仓库内配置和 bundle 路径；容器按模块启动，保留框架原有 Python 搜索路径。
选择实验只用命令行：支持 `--选项 值`、`--选项=值`、单数/复数别名、重复选择和逗号列表，不支持缩写；
多次 `--channels` 合并。`TRACE_SIM_PROFILE_CONTAINER_NAME` 只用于定位本次容器和预算超时清理。

suite 先展开并预检查全部选中实验，再串行执行。
单次运行和 suite 都在创建运行对象时完成输入预检查，不需要调用方另行开启。
矩阵按公共配置、server、input、实验覆盖的顺序合并，
每层覆盖后应用 `$unset`；选择子集不改变原编号。命令与环境变量先展开运行目录，再解析配置字段，未知字段报错。
三种通道的选项、probe 目录、尾部等待及刷新参数在准备时确定，执行和 manifest 共用，不能由旧环境变量暗中改写。
服务器按本次配置设置 `TRACE_SIM_PYTHON_PROBE*`，workload 子进程清除这些注入；无关搜索路径和通用 `LD_PRELOAD` 继承规则不变。

Torch 与 hook 默认输出到 run 内 `trace/torch`、`trace/ld_preload`；自定义相对路径以 run 为基准，绝对路径原样使用。
hook 按指定前缀的 `.rank*.pid*.json` 收录。自定义目录应专用于本次采集；启动重试不清理外部或自定义目录。
中断时回收已启动的子进程，不重试启动；临时 HiCache 存储清理失败也算失败，清理范围不得包含 run 根目录或外部路径。
显式 `model_config_overrides` 仅在本次运行生效，退出时恢复原始字节；恢复失败时报错，并保留 `_config_backup` 供人工恢复。

`suite_result.json` 在启动前声明各 manifest 位置，结束时记录已尝试、成功、失败和未执行数量。
`continue_on_error` 只允许普通错误后继续，有失败最终仍返回非零，不吞掉中断。SIGKILL 或写盘故障可能留下 running 状态，
因此声明路径、文件存在或旧状态都不等于采集完成。dry-run 不启动服务、不清理存储；它和失败运行都不生成 capture bundle。
base 与共享补采只读取声明的产物；缺少用量证据的失败仍按预留请求/token 计费，账本不把缺测当零。

manifest 声明的文件缺失、标记 `exists: false` 或 JSON 损坏时，正式预检查和成本提取报错，不能靠扫描其他文件补全。
流式 trace 只允许补齐缺失的数组/对象结尾，不删除残缺事件；历史诊断可单独记录坏文件继续检查，但不能据此认定正式输入可用。
forced-token 合同与正式窗口见第 4、7 节；有效性由真实请求和报告证明，不另维护一套重复的就绪报告。

### 长 trace 导出

Ascend 长 trace 的导出峰值内存可能远高于采集阶段。可在实验 `env` 设置
`SGLANG_NPU_PROFILER_SERIAL_EXPORT=1`，只将采集停止后的 NPU 导出回调按 rank 串行执行；
不改变正式请求、采样窗口或模型参数。该开关适用于当前 scheduler profiler，默认关闭。
长 trace 实验同时设置 `profiling.torch.strict_stop=true`，并按串行导出总时长配置 `stop_timeout_sec`；
09-14 的 TP=2/4 泛化实验使用 14400 秒上限，不代表预期每次需要四小时。导出不与重型 DAG 建模并发。
默认宽松停止模式可能把 API 超时保存为 `profile_stop_response.json` 的 warning，因此 manifest completed
及 trace 文件存在均不能替代文件完整性和下游设备/phase 语义检查。不完整导出优先从保留的原始数据离线恢复，不能当作成本误差拟合。

## 2. Framework 能力

| 能力 | SGLang | KTransformers |
| --- | --- | --- |
| 独立 image/service | `sglang` | `ktransformers` |
| LD_PRELOAD trace | 支持 | 支持，使用专用 hook |
| torch trace | 支持 | 当前 smoke 默认关闭 |
| Python semantic probe | HiCache 使用 | 当前不使用 |
| framework-neutral DAG | 支持 | 支持 |
| HiCache | 支持 | 不适用 |

两个框架共享 manifest/DAG，不共享不存在的 runtime module。KTransformers submodule、installer、image、compose service、hook 和 dispatch
都属于正式保留能力。

## 3. SGLang 5×3 实验

当前评分面板由 5 个 HiCache config 和 3 个 workload 组成。配置位于：

```text
configs/workloads/hicache_manual/configs.json
configs/workloads/hicache_manual/w1_device_spill_qualification_ring.json
configs/workloads/hicache_manual/w2_writeback_dual_tier_cascade.json
configs/workloads/hicache_manual/w3_prefetch_admission_ladder_and_survival.json
configs/experiments/hicache_manual_workload/profiling_full_dag_replay.json
```

完整采集得到 15 个真实 profile cell；每个 base 到其他 4 个 config 有 12 个 cross prediction，5 个 base 合计 60。这里的 5、3、12、60
只属于当前实验，不是 predictor 的硬编码接口。

开发时应按语义挑关键 cell：

```bash
scripts/profile.sh \
  configs/experiments/hicache_manual_workload/profiling_full_dag_replay.json \
  --server <base-config> \
  --input <workload> \
  --forced-token-bundle <bundle> \
  --dry-run
```

## 4. Forced-token capture 与 replay

forced-token 合同固定真实 request 顺序、输入 tokens 和输出 tokens，使不同配置看到相同 workload。
它是可复现实验的选项，不是普通预测必须手工拼装的一套流程。
预检查、实际 workload 执行和轻量回放共用参数解析，支持 `--参数 值` 与 `--参数=值`，重复参数以最后一次为准；
使用完整参数名，不使用缩写。capture 仅在全部请求及正式窗口成功后写出计划，已有计划仍禁止覆盖。
组流程的 base 自动补采也使用同一参数解析，不再要求参数必须以分开的两个字符串书写。
生成 capture/replay 配置时修改最后一次出现的参数；省略的默认配置路径在需要替换为本次输入时显式追加。
CPU 校正的轻量回放不替换 workload 参数，直接保留 base 命令和已使用的 token-plan 路径。
组流程中受预算管理的轻量回放每次只启动一次服务；普通 profiling 的配置化启动重试不受此限制。
正式阶段含重复请求时，计时边界对应展开后的第一条和最后一条请求。
模板加载统一校验字段类型，执行计划与宿主机预算共用逐项展开逻辑，不再先构造重复步骤的中间列表。
展开层只读已校验模板；tokenizer 输出长度、缺失的分支标记及空正式阶段仍在展开时检查。
普通请求、token 采集和回放共用请求参数，回放只额外指定已校验的输出 token；响应 token 在 HTTP 边界解析一次。
suite 保留输入 bundle 和成功 capture bundle 的索引，计划路径来自 workload 报告；不另存完整响应或重复 token 校验结果。
单次回放可直接指定已有 plan，不要求 suite 标签或额外 bundle；预检查仍核对模板 workload ID 和 token 计划。
使用 `{forced_token_plan}` 占位符时必须提供 bundle；同时指定 bundle 和显式 plan 会报错，不隐式覆盖用户输入。

先捕获：

```bash
scripts/profile.sh configs/experiments/hicache_manual_workload/no_profile_capture.json
```

再 profile：

```bash
scripts/profile.sh \
  configs/experiments/hicache_manual_workload/profiling_full_dag_replay.json \
  --forced-token-bundle <capture-suite>/forced_token_bundle.json
```

capture 的真实输出必须达到请求的 `max_new_tokens`，replay 会重新核对 workload ID、request order 和 token arrays。HTTP 成功但输出被容量
裁短不能作为合法 bundle。

有状态 capture 还要通过 startup gate、barrier 和 checkpoint。token-only capture 不提供建模 trace；随后的 replay 才是 base DAG 数据。
workload 只保留请求、重复请求、barrier 和 checkpoint；旧显式 wait 分支已删除，它原本也无法通过两阶段布局校验。
checkpoint 用于与服务端核对 token 路径的摘要仍保留，按前缀增量计算并在各 rank 间复用；这不是产物版本控制。
报告中缺失的 forced-token 检查计数不再补零，损坏报告也不视为“未开启检查”。

采集汇总及 CPU 校准按 manifest 声明读取 workload 报告和 trace，不按目录名称猜测；
capture bundle 中的计划路径来自对应报告。普通 bench-serving 的 JSONL 输出也由 manifest 的
`bench.bench_serving_files` 登记，但其中的总耗时没有 trace 时钟起点，不能据此构造 `[0, duration]` 裁剪窗口。
普通 DAG 构建在没有时间边界时读取完整 trace；需要正式窗口的建模/校准须提供带时间戳的 workload 报告。
未登记的旁置文件不自动参与建模；被选用的声明文件丢失则报错。
已有正式窗口但边界不完整的报告不能退回整份请求包络，更不能当作完整 E2E 真值。

## 5. 共享校准的采集语义

`prepare-hicache` 先复用 base 和已有独立测量，仅对剩余成本缺口选择共享计算或缓存压力实验。具体候选及限制统一维护在
[建模流程](modeling_development.md#5-一个-base-组)，这里不另维护一套端点和重复规则。

生成的 suite 与 profile 均写入本组目录；每次实际运行同时记录 token 和 trace，不依赖上一轮 token bundle 来切换回放。
失败、超时、无效 trace、请求和 tokens 都进入累计账本。不读取 target profile、评分或某个 cell 的误差来选择实验。
它是成本参数采集，不是另一个 workload 评分阶段；只通过 HTTP 不能证明 trace 通道、状态检查或成本辨识完整。

## 6. Trace channels

SGLang HiCache 使用：

- `torch`：device kernel 与 framework execution；
- `ld_preload`：CPU/syscall/I/O timing；
- `python_probe`：cache state、effect 和 phase 语义事实。

SGLang 的同步 hook 覆盖 ACL 与下层 runtime 入口，同线程、同 stream 的嵌套调用只记录外层一次。
可选环境变量 `HOOK_SYNC_THREAD_CPU=1` 在同步事件的 `args.thread_cpu_ns` 中记录线程 CPU 时间，默认关闭。

SGLang LD profile 的 `HOOK_MEMCPY2D_TIMING=1` 可单独记录 `aclrtMemcpy2dAsync` 的几何、方向、stream及wall/线程CPU时间，默认关闭。不读取张量内容或snapshot；线程CPU仍包含wrapper入口及参数格式化，不包含最终trace写出。它用于核对提交调用中的CPU工作和等待，不能直接把wall−CPU当可删除成本。实际symbol绑定和调用覆盖需在运行镜像验证后，才能用于校准。
它包含同步内部轮询和少量 wrapper 工作，不等于设备等待时长；时钟读取失败时字段缺失，不记作零。
该诊断目前通过独立构建验证，使用时需明确指定包含这一实现的 hook 库，不能假定已有部署库已更新。

SGLang 的独立阶段计时使用 `SGLANG_STEP_TIMING_DIR`。同时设置 `SGLANG_SCHEDULER_TIMING=1` 时，额外记录
`scheduler.run_batch`、`scheduler.process_batch_result`、有请求时的 `scheduler.get_next_batch_to_run`
以及带请求 ID 的 `scheduler.process_input_requests`
的墙钟、线程 CPU 与请求/rank 身份；默认不记录这些范围。批次选择即使返回 `None` 也保留活跃重试，
但 waiting/running/last batch/chunked 均无请求时不记录；只复制去重的请求 ID，不读取请求内容。
它们与 forward 步骤有嵌套关系，不能直接相加；JSON 写出不在本层包络内，但内层计时器写出仍会计入外层。
不采 tensor 内容或快照；run/result 的空批次不记录，该诊断仍不覆盖请求接收等全部调度工作。
输入处理支持单请求及 batch 容器，只复制请求 ID；空轮询及无请求 ID 的控制消息不记录。
输入处理与其内部 HiCache 方法可能嵌套，计算独占成本时需要扣除已测子范围，不能重复分配预算。

诊断开关 `SGLANG_STEP_TIMING_EMISSION=1` 可在下一条同线程记录的 `previous_emission` 中携带
上次 JSON 序列化、加锁与写出的墙钟边界及线程 CPU 时间，默认关闭，不增加额外记录行。
这能识别父方法/轮询间隔内的部分观测工作，但最后一次写出可能没有后继记录；计时入口、
构造 row 和采样元数据等工作仍未完整覆盖。不能据此声称消除了全部 profiler 开销，
也不能把锁等待墙钟当作线程 CPU。扣减时须按原始线程和区间去重，避免嵌套父子重复计算。

设置同一输出目录并开启 `SGLANG_HICACHE_HOST_TIMING=1` 可额外记录四个 host 方法：

`HiRadixCache.write_backup/load_back`、`HiCacheController.start_loading`、`PrefillAdder.add_one_req`。
默认关闭时保留原函数；开启时仅记录墙钟、线程CPU、pid/tid及是否正常返回，不读取参数内容。
这些记录本身没有请求身份，分析时必须按同线程完整包含关系关联外层，歧义或跨界需单列。
嵌套方法不能直接相加；外层probe包装自身开销不一定包含在方法计时中，不能把这些记录视为全部采集开销。

独立等待校准还可选开 `SGLANG_PREFETCH_TIMING=1`（同时需要 `SGLANG_STEP_TIMING_DIR`）。它记录预取 progress/check、控制器 terminate_prefetch（stop）、HiCache 事件检查及 `_all_reduce_attn_groups` 的嵌套墙钟/线程CPU计时。调度部分还记录写/载入完成检查、存储队列处理、请求接收及其广播方法，以保留完成确认与资源释放的边界。广播方法在DP/CP配置下可能包含多次通信，不能默认当成一次collective。progress 仅附请求ID、进入时operation是否存在/已发起及返回布尔值，不采snapshot或遍历tensor。stop 标出设置终止标记并读取完成量的方法范围，不代表后台线程已退出。通信需作为子调用单独分析，不能把整段墙钟都当CPU工作；timer写出发生在自身范围外，仍可能进入父范围。此开关默认关闭，关闭时返回原函数，不增加包装层。它是诊断观测，不会自动修改模型成本。

Python probe 默认不采集 cache snapshot。早期 snapshot 会遍历并序列化大对象，显著增加 CPU gap；当前正式 target catalog 已删除这条默认
路径。workload checkpoint 只是边界处的轻量状态门禁，不是 snapshot。

可选 `profiling.python_probe.diagnostics: "timing"` 记录 I/O thread CPU/wait 和缺页计数，只用于诊断，
不能与默认轻量 profile 混作同条件成本样本，也不能把未覆盖时间自动归为可建模 CPU gap。
采集侧仅保留 `off` 与 `timing`：已删除开发期逐调用 cProfile 明细及 `full` 档，
不再在 probe 内维护函数级性能分析。历史 trace 不修改；modeling 的 `--diagnostics full` 不受影响。

`timing` 还会对 storage enqueue 探针记录 `python_probe.emission_work`：参数绑定、字段提取与原事件写出三段的墙钟区间、线程 CPU 纳秒。它由 target 的 `capture_emission_timing` 策略控制，默认关闭；属于 `runtime_diagnostic`，不增加 DAG 执行节点。三段仍不包含诊断记录自身写出、wrapper 全部工作等开销，不能当作完整 probe 成本或直接从 E2E 真值扣除。

显式线程诊断读取 schedstat 时，每次读取后立即关闭文件，不保留线程局部裸描述符。
这避免工作线程退出后的句柄泄漏，但会增加诊断模式的文件打开/关闭开销；默认关闭时不执行这些操作，
新旧诊断采集不能据此视为完全等开销。schedstat 不可读时仍标为 unavailable，不填零。

storage enqueue 的归属使用操作编号，传输数量使用 `token_count`，不再重复遍历和写出完整 token 字典；状态建模所需的 workload identity 路径仍保留。自耗时记录用 `observed_target_id` 标明被测探针，不冒充带 `fact` 的建模事件。

声明 `hicache_dag_patch` consumer 时，请求响应、CPU通信、逐层等待及首次编译/加载是必要观测，`diagnostics=off`也保留。
它们与语义probe一样由consumer选择，不需要另一个用户开关。只做state/input-contract且off时不安装这些DAG观测。
server显式接收consumer列表；manifest的 `profiling.python_runtime_probes` 记录选择的模块，实际事件覆盖仍由建模端核对。
旧off资产没有这些事件，不会因当前选择规则改变而自动可用。比较诊断开销时需使用保留同一组必要观测的新采集。

import hook 复用模块名与固定 probe 目标的匹配结果，避免每次 Python import 都扫描全部目标；
它不缓存“已安装成功”，部分初始化、后续导入和模块替换仍会重新检查。这个优化不删除事件，也不自动修正历史 trace 的耗时。
通用 callable probe 也只检查当前函数的包装标记，不按模块名永久记账；同名模块或函数被替换后仍可安装。
bootstrap 直接加载 callable 实现，HiCache 字段使用固定的提取器集合，不再经由可变插件注册表和转发入口。
该集合在安装包装器时加载，避免把首次导入放进被测调用；token/helper 与 callable 的导入顺序不影响字段解析。
CPU 通信、HiCache 时序、分配、请求响应及 Triton 准备探针共用安装逻辑，方法的元信息与包装标记在安装处维护。
上下文、语义事实和时序观测仍使用各自的标记，允许在同一方法上共存；缺失的类或方法留到后续导入再检查。
HiCache 中只记录入口元数据的同步调用共用计时与异常透传；需要读取返回值、恢复上下文或特殊计时边界的包装保持独立。
字段路径中的 Python 属性只读取一次，避免 getter 的额外开销或副作用改变采集值。
callable 的函数签名在安装时解析；同次调用的每个开始/结束阶段只绑定一次实参，多个 target 共用参数映射，
但事件上下文各自独立，不共享 request 快照缓存或 fact 元数据。单 target 不额外复制映射。
emission 的 binding 区间只涵盖该事件实际承担的绑定和上下文工作，不重复计入前一个事件已完成的绑定。
字段提取统一使用绑定结果、位置参数和返回值，
不逐层透传已合并的原始关键字参数，也不在发事件时重新解析函数签名。
评估采集扰动须使用同配置、同 forced tokens 的 timing/off/无 profiler 对照，不能用不同 target 的耗时差估计开销。

Triton的编译准备（`runtime.triton.prepare`）和首次句柄装载（`runtime.triton.load`）由DAG consumer选择；
没有声明该consumer时，显式 timing 也可安装它们作诊断。仅关闭 diagnostics 不能关闭完整建模所需的准备观测。
准备区间包括磁盘缓存查找，异步模式下可能只测到编译提交；装载区间包括 launcher 创建和 binary 装载，均不能当成设备执行时间。
prepare 的 `execution_mode` 区分同步、异步与未知；`path` 仅在实际进入 IR 生成时标为 `compiled`，
同步返回对应内核且确认 IR 入口已观测、没有生成 IR 时标为 `disk_cache`。异步提交标为 `async_submit`，
其他情况为 `unknown`；不按耗时阈值分类，旧记录缺少字段也不能反推成编译样本。
记录保存在 Python probe trace 的 `runtime_diagnostic` 类别中，供准备成本归因与建模使用，不作为 HiCache fact。
source 已有准备时间仍由原 CPU gap 承载，消费这些观测时不能再重复加入一份成本。
不采 tensor 内容、snapshot 或缓存摘要；重复的已装载内核不发事件。不改缓存与预热策略，冷启动和已有缓存的结果必须分开解释。

DAG consumer 或显式 timing 会记录终止请求的 `runtime.response.*` 边界：scheduler_send、tokenizer_dispatch、serialize、http_body_sent。
只记录请求 ID 和区间，不读取 token 数组或序列化正文；无 socket 的非发送 rank 不记录发送成功。
http_body_sent 表示非流式 SGLang JSON 响应的最后 ASGI body send 返回，不等于客户端已经收到；
最终 E2E 真值仍来自 bench。它们同属 runtime_diagnostic，不作为额外 DAG 工作重复计时；类别名称不表示它们只是可选诊断。
对唯一落在原始 CPU gap 内的 scheduler_send，建图会增加 begin/end 两个零耗时连接点，将原 gap 分段保留。
重叠、越界、分支或已有时间改写导致无法绑定时不猜测；这些点不能独立决定 E2E，HTTP/客户端完成仍需另行连接和验证。

同一诊断档位还记录 `runtime.request.socket_received` 和 `runtime.request.dispatch_ready` 两个请求入口时刻。
前者是 socket 轮询及反序列化函数返回后的时刻，不是网络包到达时间；后者是跨 rank 广播、输入处理结束并返回请求后的时刻。
只对带请求 ID 的非空结果记录，不读取 token 或 snapshot，空轮询不写事件。对唯一落在未改写 CPU gap 内的时刻，
建图插入一个零耗时连接点，原 gap 只分段、不增加成本。当前 C2/W2 补采已验证收发观测完整及插点时间守恒，
串行正式请求已接入客户端依赖链；跨 rank 等待、探针开销及完整预测精度仍需验证，不能把两个时刻之差直接当成纯 CPU 成本。

`runtime.request.tokenizer_submit` 记录 tokenizer 的单个/批请求提交函数区间；异步 ZMQ 的提交函数返回不等于接收端已收到。
`runtime.request.receive` 记录带请求 ID 的整次 scheduler 接收调用区间，其中包含广播和等待。建图只将其开始转为零耗时点，
结束复用 dispatch_ready，不重复加入区间耗时。空轮询只读取一个开始时间，不写事件；请求 ID 列表作为语义身份完整保留，不按 32 项截断。

同一选择规则记录 CPU Gloo 的 `runtime.cpu_collective`：broadcast/all_reduce 的进程组名、成员、全局 rank、
调用前后组内序号、tensor 元素数/类型，以及 root 或规约操作。只读元信息，不读取 tensor 内容、不增加同步。
该序号是进程本地的底层计数；`monitored_barrier` 等操作可使各 rank 的计数不同，不能直接当作跨 rank 通信身份。
另以 `collective_index` 记录每个进程组内从零开始的观测调用顺序，各公开入口与兼容包装共享计数；
`observation_start_sequence` 保留开始观测时的底层序号。失败或合并提交也占用调用序号，不能当成已完成通信。
跨 rank 对应仍须核对观测覆盖、成员与操作参数；这些字段本身不保证迟启探针或并发调用可以正确配对。
`async_op=true` 时区间仅到提交返回，不能当作通信完成；合并提交或失败时序号可能没有推进，不能强行一一配对。
未请求DAG consumer且诊断off时不安装，设备通信及其他 backend 原样调用。建图仅保留观测，不新增执行节点或重复收费；
通用 CPU 通信依赖与真实模型采集开销尚未完成验证，不能用这些字段声称完整通信建模已完成。

通信事件还可携带实际框架调用的 `role`、外层 `scheduler_phase` 和预取请求的 `request_id`。
这些字段通过同步调用上下文附到原事件：区分写入/回载完成检查、存储队列检查、预取状态与完成确认，
以及 Decode 前的检查。不增加事件、tensor 读取或 snapshot；异常和嵌套返回后恢复原上下文，后台线程不继承主线程角色。
旧 trace 没有这些字段时不能从操作次数补造身份。后台有 Python 通信记录但没有 Torch worker 时，应报告采集可见性缺口，
不能将通信视为零耗时；角色探针也不会主动改变后台 profiler 的启用状态。
NPU 兼容层可能再次包装 distributed 函数；同一调用栈内、同组同操作的探针只记录外层一次，
避免一条真实通信产生两条相同序号的观测。不同操作或进程组不因此跳过，异常后恢复调用上下文。

HiCache 控制观测沿用上述 consumer/诊断选择规则。以下事件统一以 `runtime.hicache.` 为前缀，
保存调用起止和 returned/raised 状态；原返回值和异常照常传递，失败不伪造完成量。
探针只读已有标量、容器长度及 tensor 形状，不读取 tensor 内容、文件 key 或 snapshot，不增加锁或设备同步。
建图保留这些时间包络，不额外生成一份执行成本。

| 事件 | 观测内容 |
| --- | --- |
| `load_completion` / `write_completion` | ACK 队列、在途操作数量的前后值，以及是否为阻塞 write-back |
| `prefetch_check` / `prefetch_stop` | `can_terminate_prefetch` 的已有布尔返回值、`terminate_prefetch` 的已有完成 token 数与请求身份 |
| `prefetch_query` | 本地命中 token 数、page size、operation ID 和同步组数量 |
| `host_release` | 释放入队的元素数、page size，及前台完成/取消请求的已有上下文 |
| `storage_drain` | 已经跨 rank MIN 的三个回收数量上限和 TP 大小；单卡可以没有 MIN，无上限保留 `None` |
| `prefetch_enqueue` | 提交包络、operation ID、请求 token 数和已分配 token 数 |
| `prefetch_read` / `prefetch_publish` | `HiCacheFile.batch_get` 的页数；`PrefetchOperation.increment` 的请求身份、本次 token 数、是否接受更新及完成计数 |

消费这些观测时，需区分以下边界：

- MIN 返回后仍可能有确认和引用释放工作；源队列计数不是目标状态，不能直接照搬。
- check 入口不是共享状态读取的精确时刻；停止和读取完成量发生在 stop 包络内。
  后台查询也可能早于 enqueue 返回，因此提交返回不是精确入队时刻。
- 释放归属需要同线程顺序、operation 身份或明确的请求上下文，不能按最近时间猜请求。
- read 按同 pid/tid 和包含关系关联 `storage_read_service_observed`，外层已含读取和 host 写入，不能重复计费。
  两次 publish 之间还有 Python、探针和调度开销，不等于纯拷贝；成功样本也不能替代取消分支的测量。

旧 trace 缺少边界或后台 Torch lane 时保留缺口，不补造零成本、完成量或停止时刻。
缺少 tokenizer 提交等 HTTP 必要观测时，事务检查通过也不代表 HTTP 可预测，不能用整图末端替代客户端完成时间。

同一选择规则记录 `runtime.hicache.layer_waits`：每个 forward batch 的请求、阶段、consumer index、层数，
以及实际 HiCache 逐层等待调用的起止时间。调用区间先缓存在内存，batch 结束时统一写出，完整保留列表；
不逐次写 JSON、不增加设备同步、不采集 snapshot。未启用 consumer 时也保留快速返回的调用区间，失败调用标记为 raised。
建图只保存观测；HiCache图变换阶段据此调整目标配置的等待，不把包络再加成一份成本。该探针的真实模型采集开销仍需测量。
NPU 的逐层区间使用 Torch 原始计数器（`wait_clock=npu_syscnt`），不与 Python 墙钟直接比较。
采集结束后，manifest 的每份 Torch trace 保存自己的 `host_clock` 换算；建图读入时将区间转换为
`profiler_ns` 纳秒边界。没有 NPU 计数器时显式记录 `unix_ns`，不初始化设备；batch 外层仍为墙钟区间。
旧采集没有原始计数器，保持原样，不通过固定偏移修补。换算只解释时间来源，不是额外成本或模型系数。
采集直接按模块启动，不使用名为 `profile.py` 的转发脚本，避免遮蔽标准库 `profile`。
旧采集中 consumer 未启用时的空列表仅表示探针未记录调用，不表示框架没有调用；不能据此推断新增等待的位置或零成本。

同一选择规则还记录`runtime.hicache.decode_allocation`：在`alloc_for_decode`入口读取request id、
已有的kv_committed_len/kv_allocated_len、decode_batch_idx、token_per_req和缓存对象身份，返回时记录状态与区间。
不读取tensor、分配结果或snapshot，不查询设备事件；定义及scheduler导入别名只包装一次。
该入口早于forward和请求长度递增，不能用forward marker替代。建图仅保留观测，不重复增加CPU成本。
审计迭代数时需考虑overlap调度先启动下一轮再处理上轮结束结果，输出token数不一定等于decode轮数加一。

HiCache probe target 声明位于：

```text
configs/profiling/hicache_probe_targets.json
```

新增 target 时只采集下游实际消费的字段；大对象转换按需执行；事实通过 owner/consumer routing 隔离。token/page 内容身份用于跨 trace 的
同一逻辑输入匹配，不用于 artifact 版本或冻结管理。

## 7. Formal window

每个 workload report 提供语义开始和结束。manifest 的 `bench.workload_report_files` 声明报告路径，
建模入口读取报告并向 DAG builder 传递窗口；这不等于已把报告中的逐请求链接入 DAG。
显式 `input.workload_report` 优先；否则只读取 manifest 声明的报告。多份报告需显式选择，缺失声明不扫描运行目录补全。

- 窗口内执行事件进入 DAG；
- 窗口前的 token dictionary 只解释窗口内路径；
- 与窗口内 async operation 精确匹配的窗口后 ACK/release 只闭合生命周期；
- 前后 context fact 不创建 duration node，也不计入 E2E。

formal window 不能按 config、cell、事件名白名单或固定微秒边界硬编码。固定校准可在 formal window 前使用 barrier/checkpoint 建立状态，正式
测量窗口内只放请求。

## 8. Profile 产物

```text
<cell>/
  profile_manifest.json
  config.json
  server_cmd.txt
  bench_cmd.txt
  bench/<workload>/workload_report.json
  trace/torch/...
  trace/ld_preload/...
  trace/python_probe/...
```

manifest 明确 framework、启用 channel、trace 文件、workload 状态和 formal window；不携带 schema version、image digest、工作树冻结或逐文件
checksum。modeling 不回头扫描 suite 目录猜输入。

成功 profile 至少满足：

- manifest、workload 和启用的 trace channels 完整；
- forced replay 的 request/tokens 精确匹配；
- HiCache lifecycle 能闭合；
- 配置与实际 server command 一致；
- snapshot-free 默认路径没有隐式开启重诊断。

## 9. KTransformers

当前公共 smoke 配置：

```text
configs/experiments/ktransformers/profiling_dag_smoke.json
```

```bash
scripts/profile.sh configs/experiments/ktransformers/profiling_dag_smoke.json --dry-run
scripts/model.sh build-dag --profile-manifest <ktransformers-manifest> --output-dir <dag-output>
```

真实运行要求 image、两张可用 NPU、模型/GGUF 资产和专用 hook 全部存在。当前 fixture 只证明 dispatch、LD_PRELOAD manifest 与共享 DAG 接线；
缺模型资产时不能称为真实性能 profile。

构建：

```bash
scripts/build.sh sglang
scripts/build.sh ktransformers
scripts/internal/hooks/build.sh sglang
scripts/internal/hooks/build.sh ktransformers
```

各 hook 使用独立构建目录。若 CMake 报缓存中的源码路径不匹配，先确认并移走对应旧构建目录再重建；
脚本不再为历史路径迁移自动删除目录，也不会忽略配置失败继续构建。

## 10. 常见问题

服务器内文件预取服务可用 `SGLANG_HICACHE_IO_TIMING=1` 配合 `SGLANG_STEP_TIMING_DIR` 记录低频 `hicache.io.storage_read` 批次。默认关闭，字段包括请求标识、请求页数、页大小、操作前后completed_tokens及通用wall/线程CPU时钟，不读取tensor或snapshot，不改变同步。另在服务计时外记录当前线程实际允许的cpu_affinity，避免仅根据环境变量推断绑定。completed_tokens差值表示发布进度，不等同于逐页copy计数。计时结束后写日志，仍不能称完全无扰动；关闭其他采集后可作固定输入的轻量服务对照。注意同一目录开关也启用已有forward步骤计时，不能把这种模式称为“仅I/O计时”。

DMA 校准使用 `scripts/model.sh calibrate-hicache runtime-dma`，只输出设备传输成本，
按固定顺序预热、采样，并用各 rank 的设备算子时钟估计成本；当前张量实现只支持两字节 KV 元素。
必须传入 `--base-report`，由平台或独立物理报告提供模型几何、TP 设备和 NUMA 放置；
不再使用特定模型的默认层数、头数或默认设备，也不重复接收这些部署参数。
采样后同时保留 DMA 原始报告，并将新测得的缺失服务合入 calibration_report.json；不覆盖其他服务。
`--device-capacity-tokens` 可按 base 实际缓存池容量固定设备布局（不含额外 padding 页），
避免最大传输量变化时连带改变拷贝步长；报告的 `transfer_layouts` 保留实际 shape 和 stride。
缺省仍按最大操作分配，这不等同于真实缓存池布局。`--profiler-level` 保留设备事件覆盖排查能力，正式校准默认用 0。
开发期的 `--host-only`、`--host-timing`、`--group-warmup`、`--reverse-order` 已删除，
不再生成独立主机时钟样本。旧诊断数据保留，但不作为正式 CPU 成本输入；CPU 开销校正使用同源完整/轻量 profile 配对。
文件预取的独立采样只记录实际框架调用的分段时钟，不在采样 worker 内另开 Torch profiler；
需要分析实际负载的 trace 时使用正式 profiling 入口。分段计时仍有观测开销，不能当作完全无扰动测量。

`--dry-run` 成功但没有 trace：这是正常的；它只展开配置和命令。

Python probe 出现大 gap：先确认使用当前 snapshot-free catalog，再做同配置的 timing/off 对照。
可识别的部分同步等待已改为依赖；剩余未知时间仍保留，不能因为尚未归属就直接删除。当前覆盖与限制见工作进展。

设备缓存释放的观测分两层：原有 `device_release_backup/regular` 标记整段调用，新增 `runtime.hicache.allocator_free` 只在这两种调用内部记录 paged/token allocator 的 free 区间。字段包括所属释放分支、token 数、页大小、是否立即回收及 need_sort；只读取 tensor.numel 和 Python 标量，不读取索引内容、不做 snapshot 或设备同步。释放之外的 allocator.free 不输出事件，异常仍原样抛出。

这个内层区间用于后续分离共有的内存池回收与分支特有的缓存元数据操作；C++ 将它保留为旁路观测，不另建可执行节点。旧 trace 不会自动获得这个边界，现有整段模板路径仍保留；区间拆分与成本消费尚待接入。轻量记录也有开销，新采数据仍须走同源 CPU 开销校正，不能把 probe 的成本当成目标操作成本。

Forced replay 被拒绝：检查 bundle 在 workspace 内，且 workload ID、request order、输入与输出 token arrays 完全一致。

KTransformers 真实运行失败：分别核对 image、NPU、hook、config dispatch 和模型/GGUF，不把 dry-run 当成真实运行。
