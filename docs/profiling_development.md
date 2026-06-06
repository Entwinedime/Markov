# Profiling 开发文档

维护方式：这是 profiling 主线文档。更新时直接删改本文件内容，不在这里写流水账。

## 目标

Profiling 只采集真实运行事实，为后续 trace graph 和 what-if 建模提供输入。采集阶段不判断目标配置下应该发生什么，也不做 policy 推断。

主流程：

```text
experiment config
  -> profile runner
  -> torch profiler / python_probe / ld_preload 分别采集
  -> trace 与 sidecar 路径写入 profile manifest
  -> modeling runner 读取 manifest 或显式输入
```

Profiling 需要回答：

- 运行中发生了哪些事件；
- 事件属于哪个进程、线程、rank、device、stream；
- 事件开始时间、持续时间和关联 id 是什么；
- request、operation、cache page、storage IO 等身份事实是什么；
- 某个建模子模块所需事实是否齐备。

Profiling 不回答：

- 目标配置下 cache / 并行 / 互联策略应该如何变化；
- DAG 应该如何改；
- 端到端预测时间是多少；
- 残差应该由哪个模型修复。

## 总体架构

Profiling 分成三个独立采集渠道：

| 渠道 | 控制方式 | 主要用途 | 输出位置 |
| --- | --- | --- | --- |
| `torch` | runner 调用框架 profiler 接口 | framework op、device kernel、copy、runtime correlation | `trace/torch/` |
| `python_probe` | runner 注入 `src/profiling/python_probe` 到 server 的 `PYTHONPATH` | request、scheduler、cache policy 输入、Python 对象状态 | `trace/python_probe/` |
| `ld_preload` | runner 注入 `LD_PRELOAD`，hook 实现保持 C++ 侧硬编码 | Python 看不到的 native runtime、同步点、IO 或框架 C++ 符号 | `trace/ld_preload/` |

`python_probe` 和 `ld_preload` 不再由同一份 target 配置统一控制：

- Python 侧由 `profiling.python_probe` 控制；
- LD_PRELOAD 侧由 `profiling.ld_preload` 控制；
- LD_PRELOAD 要拦截什么符号，由 `src/profiling/ld_preload` 中的 C++ wrapper 和构建 profile 决定。

这样做的原因是 LD_PRELOAD 不能可靠地由 Python 动态声明任意符号；每个 wrapper 都需要明确的函数签名、符号名、目标 so 和参数序列化逻辑。

## 运行入口

真实 SGLang / KTransformers profiling 必须使用外层容器入口启动：

```bash
scripts/profile.sh configs/experiments/profiling_minimal_sglang_hicache.json
```

dry-run 也优先使用同一个外层入口：

```bash
scripts/profile.sh configs/experiments/profiling_minimal_sglang_hicache.json --dry-run
```

`scripts/profile.sh` 负责选择 docker compose service、挂载仓库到
`/workspace/trace-sim`、进入框架容器、设置 Ascend 环境，再在容器内调用
`scripts/internal/profile_runner.py`。

`scripts/internal/profile_runner.py` 是容器内执行器，不是真实实验的宿主机入口。
它只允许在这些场景直接调用：

- 已经位于 `scripts/profile.sh` 启动的框架容器内；
- 单元测试 / fixture；
- 不启动 server 的配置展开或 dry-run 检查。

不要在宿主机上直接运行真实 SGLang profiling：

```bash
python3 scripts/internal/profile_runner.py --config configs/experiments/profiling_minimal_sglang_hicache.json
```

宿主机 Python 环境不保证安装 SGLang、torch、torch_npu 和 Ascend 运行依赖。直接调用会
绕过容器环境，典型失败是 `ModuleNotFoundError: No module named 'sglang'`。

## 实验配置

Profiling 配置只描述采集和运行，不嵌入 modeling 预测逻辑。

| 字段 | 作用 |
| --- | --- |
| `name` | 实验名称，用于 run dir 和 manifest。 |
| `framework` | 当前 runner 支持 `sglang`，用于选择启动和 profiler 控制方式。 |
| `run_root` / `run_id` | 控制输出目录；未配置时使用默认时间戳目录。 |
| `profiling.enabled` | 是否启用 profiling。 |
| `profiling.channels` | 启用哪些渠道，只接受 `torch`、`python_probe`、`ld_preload`。短名和旧别名不再兼容。 |
| `profiling.debug` | 是否打开采集层 debug。Debug 输出不能作为默认 modeling 输入。 |
| `profiling.torch` | torch profiler 的启动、停止和输出配置。 |
| `profiling.python_probe.probes` | Python probe 插件列表，通用 callable 使用 `generic_callable`，SGLang HiCache 使用 `sglang.hicache`。 |
| `profiling.python_probe.targets` | Python callable 插桩目标。 |
| `profiling.ld_preload.enabled` | 是否启用 LD_PRELOAD。 |
| `profiling.ld_preload.library` | LD_PRELOAD so 路径，通常是 `build/docker/sglang/lib/libhook.so`。 |
| `profiling.ld_preload.trace_output` | hook 输出文件基名；hook 会自动追加 `.rank<RANK>.pid<PID>.json`。 |
| `server` | 被测 server 启动命令和 ready URL。 |
| `bench` | workload driver 命令；runner 会移除 server 侧 probe 环境，避免误采 bench。 |

`profiling.torch` 的默认语义是覆盖完整 workload：runner 调用 `/start_profile`，运行
workload，workload 结束后再调用 `/stop_profile`。默认配置不应设置 `num_steps`。
只有需要按 profiler step 自动结束的非默认实验才设置 `profiling.torch.num_steps`；一旦设置
`num_steps`，runner 不再在 workload 结束后强制调用 `/stop_profile`，避免重复 stop
导致 server log 出现 “Profiling is not in progress”。

HiCache diagnostic workload 使用 `scripts/bench/hicache_phased_workload.py`。它会输出
`workload_report.json`，其中包含请求时间窗口、phase 统计和每个 phase 预期触发的
cache 机制。modeling runner 会自动用该文件过滤 trace，只验证 workload 主窗口。

Diagnostic workload 必须采用渐进压力策略：先保证完整跑完，再逐步提高压力触发更多
cache 机制。当前默认真实实验使用保守压力参数，避免 NPU attention kernel 在长上下文
压力段崩溃。`--max-errors=1` 用于在首个请求错误后停止 workload，保留清晰失败点，
避免 server 已退出后的大量 `Connection refused` 污染报告。

当前 Ascend NPU 环境下，`torch profiler + NPU graph + HiCache phased workload` 不是
稳定默认组合。已验证纯 SGLang HiCache、LD_PRELOAD-only 与 NPU graph 可以跑完；但
torch-only 与 NPU graph 会在 `prefetch_reuse_C` 阶段触发
`aclnnFusedInferAttentionScoreV3` 的 `507009`，随后 scheduler 进程
`Segmentation fault` / `Bus error`。关闭 SGLang runtime profiler 的
`experimental_config` 后仍复现。默认稳定采集路径应优先使用以下两类之一：

- 开启 NPU graph 时，不启用 torch profiler，只采 Python probe / LD_PRELOAD；
- 需要 torch trace 时，关闭 NPU graph，或先用更小 workload / 更窄 profiler activity
  做隔离验证。

Base DAG 验证优先使用 SGLang 自带 `sglang.bench_serving`，避免 HiCache workload 和
Python probe 干扰 DAG 本身。当前配置入口：

```bash
scripts/profile.sh configs/experiments/profiling_sglang_bench_serving_base_dag.json
```

该配置只启用 `torch` 和 `ld_preload`，server 关闭 graph，不启用 HiCache 参数。
bench 使用离线 `random-ids` dataset，输出 `bench/bench.jsonl`；modeling runner 会用最后一行
`duration` 作为 faithful replay 的 actual E2E。

当前 phase 设计：

| phase | 作用 | 预期机制 |
| --- | --- | --- |
| `seed_A` | 建立 A 前缀和首次插入。 | lookup、insert、write backup/storage |
| `reuse_A` | 复用 A 前缀，验证 prefix match / hit。 | lookup |
| `backup_wait_A` | 重复 A 请求，提高 selective write 触发概率。 | lookup、write backup/storage |
| `pressure_B` | 发送大量 B 前缀长请求制造 cache 压力。 | lookup、insert、evict |
| `reuse_A_after_pressure` | 压力后复用 A，验证 load_back。 | lookup、load_back |
| `prefetch_seed_C` | 建立 C 前缀和 storage 备份。 | lookup、insert、write backup/storage |
| `prefetch_reuse_C` | 复用 C，验证 prefetch 决策和 L3->L2 movement。 | prefetch decision/schedule/query/transfer |
| `dirty_eviction` | 仅 write_back 场景启用，验证 dirty eviction writeback。 | insert、evict、writeback |

`--hicache-ratio` 不作为 diagnostic 自由变量。官方配置固定使用当前基线值 `2.0`。
如果容量压力不足，先增加 workload 的长上下文和压力请求数；只有确认 SGLang 参数
约束后，才使用明确安全的 `--hicache-size`。

Python probe 示例：

```json
{
  "profiling": {
    "enabled": true,
    "channels": ["torch", "python_probe"],
    "torch": {
      "enabled": true,
      "output_dir": "trace/torch"
    },
    "python_probe": {
      "probes": ["generic_callable"],
      "targets": [
        {
          "id": "hicache.match_prefix",
          "module": "sglang.srt.mem_cache.hiradix_cache",
          "target": "HiRadixCache.match_prefix",
          "events": ["hicache_lookup_start", "hicache_lookup_end"],
          "fields": [
            {"name": "request_id", "source": "arg:req_id", "required": false},
            {"name": "page_size", "source": "self.page_size"},
            {"name": "status", "source": "const:observed"}
          ]
        }
      ]
    }
  }
}
```

LD_PRELOAD 示例：

```json
{
  "profiling": {
    "enabled": true,
    "channels": ["ld_preload"],
    "debug": true,
    "ld_preload": {
      "enabled": true,
      "library": "build/docker/sglang/lib/libhook.so",
      "trace_output": "trace/ld_preload/cpu_trace.json"
    }
  }
}
```

## Python Probe

当前 active Python probe 位于 `src/profiling/python_probe`，采用 `sitecustomize.py + import hook + probe plugin` 结构。

启动条件：

| 环境变量 | 作用 |
| --- | --- |
| `TRACE_SIM_PYTHON_PROBE=1` | 打开 Python probe bootstrap。 |
| `TRACE_SIM_PYTHON_PROBES` | 逗号分隔 probe 插件列表，默认 `generic_callable`；HiCache target 必须使用 `sglang.hicache`。 |
| `TRACE_SIM_PYTHON_PROBE_TARGETS` | JSON 数组，传给 callable probe。 |
| `TRACE_SIM_PYTHON_PROBE_OUTPUT` | Chrome trace 输出目录。 |
| `TRACE_SIM_PYTHON_PROBE_DEBUG=1` | 打开 probe debug 日志。 |

`generic_callable` target 字段：

| 字段 | 作用 |
| --- | --- |
| `id` | 插桩目标稳定 id，用于事件和错误定位。 |
| `module` | Python 导入模块，例如 `sglang.srt.mem_cache.hiradix_cache`。类方法建议显式写该字段。 |
| `target` | 模块内 callable 路径，例如 `HiRadixCache.match_prefix`；模块级函数也可直接写完整路径 `pkg.mod.fn`。 |
| `events` | 事件名数组；第一个用于 start，最后一个用于 end，异常事件使用最后一个事件名加 `:exception`。 |
| `fields` | 需要采集的字段列表。 |
| `enabled` | 是否启用，默认 `true`。 |

字段配置：

| 字段 | 作用 |
| --- | --- |
| `name` | 输出字段名。 |
| `source` | 取值位置。 |
| `required` | 必需字段标记；缺失时写入 `missing_required_fields`，不阻断业务。 |

当前支持的 `source`：

| source | 作用 |
| --- | --- |
| 空字符串 | 按字段名从绑定参数或 kwargs 查找。 |
| `arg:name` | 从函数参数读取 `name`。支持 `arg:params.req.rid` 这类嵌套属性路径。 |
| `arg:0` / `args.0` | 从位置参数读取指定下标。 |
| `kwarg:name` | 从 kwargs 读取 `name`。支持嵌套路径。 |
| `self.attr` | 从实例方法第一个参数读取对象属性。支持嵌套路径。 |
| `return` | 记录返回值摘要。 |
| `return.attr` | 从返回值读取属性。支持 `return.0`、`return.1.id` 等 tuple/list 下标。 |
| `len:<source>` | 先按内部 source 取值，再记录 `len(value)`；用于 token/page/tensor 长度。 |
| `list:<source>` | 先按内部 source 取值，再把 tensor、array、RadixKey 等容器收敛为短列表。 |
| `const:value` | 写入常量字符串。 |

`page_hashes:<tokens>,<page_size>[,<prior_hash>]` 只由 `sglang.hicache` probe 支持，
用于按 SGLang HiCache page hash 规则生成 page identity。通用 `generic_callable`
不包含 HiCache 特化 source。

输出格式是 Chrome trace JSON：

```text
trace/python_probe/python_probe_trace.rank<RANK>.pid<PID>.json
```

每个事件的事实字段写在 `traceEvents[].args` 中。默认 modeling 输入应只读取 `model_input=true` 的事件。

## LD_PRELOAD

LD_PRELOAD 目录为 `src/profiling/ld_preload`。它是独立的 C++ hook 框架，不接受 Python runner 生成的 target 列表。
该目录只维护实现，不维护独立 README；开发说明统一写在本文档。

实现边界：

- 每个采集点必须是 C++ 中写死的 wrapper；
- wrapper 必须声明准确函数签名、符号名和目标 so；
- `HOOK_PROFILE=sglang` 当前只复用 AscendCL runtime wrapper；
- 不使用 Python 配置动态生成 LD_PRELOAD target；
- 不默认拦截 `open/read/write` 这类通用 POSIX IO，因为它容易踩到 logger、解释器和 libc 初始化路径，并且缺少 request/page/hash 归属；
- HiCache storage page 事实优先由 Python probe 的 `HiCacheController` / `HiRadixCache` target 采集；
- 只有当 native backend 的稳定符号和签名明确后，才在 `targets/sglang_hooks.cpp` 增加专用 wrapper。

构建入口：

```bash
scripts/internal/hooks/build.sh sglang
scripts/internal/hooks/build.sh ktransformers
scripts/internal/hooks/build.sh ascendcl
scripts/internal/hooks/build.sh ld_preload
```

说明：

- `sglang`、`ktransformers`、`ascendcl` 构建对应硬编码 wrapper profile；
- `ld_preload` 构建模板 hook，用于验证框架本身能编译和被 `LD_PRELOAD` 加载；
- 新 native 采集点必须在 C++ 中新增 wrapper，写清楚符号名、目标 so、参数字段和 trace 名称；
- runner 只设置 `LD_PRELOAD` 和 `HOOK_TRACE_OUTPUT`，不再设置 `TRACE_SIM_NATIVE_HOOK_TARGETS`。

当前 `sglang` profile 的必采事件是 AscendCL runtime 时间锚点：

| wrapper | 用途 |
| --- | --- |
| `AscendCL@aclrtSynchronizeStream` | 捕获 stream 同步等待。 |
| `AscendCL@aclrtSynchronizeStreamWithTimeout` | 捕获带 timeout 的 stream 同步等待。 |
| `AscendCL@aclrtSynchronizeEvent` | 捕获 event 同步等待。 |
| `AscendCL@aclrtSynchronizeEventWithTimeout` | 捕获带 timeout 的 event 同步等待。 |
| `AscendCL@aclrtSynchronizeDevice` | 捕获 device 全局同步。 |
| `AscendCL@aclrtSynchronizeDeviceWithTimeout` | 捕获带 timeout 的 device 全局同步。 |
| `AscendCL@aclrtRecordEvent` | 捕获 event 记录锚点。 |
| `AscendCL@aclrtStreamWaitEvent` | 捕获 stream 等待 event 的依赖锚点。 |

hook 输出基名来自 `HOOK_TRACE_OUTPUT`。实际文件会追加 rank / pid 后缀：

```text
trace/ld_preload/cpu_trace.json.rank<RANK>.pid<PID>.json
```

运行时环境变量：

| 变量 | 作用 |
| --- | --- |
| `HOOK_TRACE_OUTPUT` | 输出文件基名；hook 会自动追加 `.rank<RANK>.pid<PID>.json`，用于多进程场景拆分 trace。 |
| `RANK_ID` / `HOROVOD_RANK` / `OMPI_COMM_WORLD_RANK` | hook 按这个顺序识别 rank；都不存在时使用 `unknown`。 |
| `HOOK_ENABLE_PAPI` | 可选打开 PAPI / PMU 计数采集；采集失败不能影响主 hook 事件输出。 |
| `HOOK_PAPI_EVENTS` | 逗号分隔 PAPI 事件名；未配置时使用 hook 实现中的默认事件集合。 |

wrapper 实现约定：

| 约定 | 作用 |
| --- | --- |
| `HOOKFW_DEFINE_TARGET` | 绑定 trace 名称、函数类型、mangled symbol 和目标 so。 |
| `HOOKFW_INVOKE` | 在 wrapper 中解析原函数、执行过滤规则、记录开始结束时间、参数和可选 PMU。 |
| `HOOKFW_SET_RULE` | 配置 caller-to-callee 过滤；无规则或 caller 列表为空时表示记录该 callee 的所有调用。 |
| `Function-Args` | native wrapper 序列化出的参数统一写入该 args 字段，供 trace merger 补充 torch 事件或追加 native-only 事件。 |

新增 LD_PRELOAD 采集点时必须先确认目标符号没有被完全 inline，且函数签名、mangled name、目标 so 与运行环境一致。

## 最小 Trace 事实契约

所有可建模事件至少应能提供下列公共字段。不同渠道可以把字段放在不同容器里，但 normalizer 进入 modeling 前要规整。

| 字段 | 作用 |
| --- | --- |
| `schema_version` | 事件 schema 版本。 |
| `domain` | 事件所属领域，例如 runtime、kernel、cache、communication。 |
| `event_kind` | 事件类型。 |
| `pid` / `tid` | 进程和线程身份。 |
| `rank` | 多进程并行身份。 |
| `ts` / `dur` | 开始时间和持续时间。 |
| `status` | success、failed、queued、completed 等状态。 |
| `model_input` | 是否作为默认建模输入。Debug 事件必须为 false 或缺省。 |

按需字段不应全局默认采集。对象明文、大 tensor、完整 token 列表不应作为默认字段；需要采 hash、长度、page id 或摘要。

`scripts/internal/profile_quality.py` 是 profiling 后的质量审计入口。它读取
`profile_manifest.json`，检查：

- trace 文件是否包含 torch / LD_PRELOAD / python probe；
- Python probe target 是否命中；
- required 字段是否缺失；
- `workload_report.json` 声明的 HiCache 机制是否实际出现；
- 会改变 cache resident/dirty/backuped 状态的事件是否具备 page identity。

controller start/enqueue 这类队列锚点允许 count-only；真正状态转移事件缺 page
identity 时，`quality_ready=false`。

## 子模块采集矩阵

不同 modeling 子模块只启用自己需要的事件。

| 子模块 | 需要事件 | 推荐渠道 | 关键字段 |
| --- | --- | --- | --- |
| `TraceGraph` | CPU op、runtime launch、device kernel、copy、sync/wait | `torch`, `ld_preload` | `device_id`, `stream_id`, `correlation_id`, `op_name`, `dur` |
| `NodeScaleModule` | 被缩放的 op/kernel/copy 节点 | `torch` | `op_name`, `node_kind`, `dur` |
| `BandwidthModule` | memcpy、host-device copy、device-device copy、storage read/write | `torch`, `ld_preload` | `bytes`, `src`, `dst`, `direction` |
| `ParallelStrategyModule` | rank 启动、collective、send/recv、barrier、shard 信息 | `torch`, `ld_preload`, `python_probe` | `rank`, `world_size`, `group_id`, `collective_kind`, `bytes` |
| `InterconnectModule` | CPU-NPU copy、NPU-NPU copy、collective、runtime sync | `torch`, `ld_preload` | `src_device`, `dst_device`, `bytes`, `link_kind` |
| `HiCacheModule` | cache lookup、insert、load、prefetch、write、evict、storage IO、request lifecycle | `python_probe`, `ld_preload`, `torch` | `request_id`, `operation_id`, `page_identity`, `page_size`, `tier_src`, `tier_dst`, `bytes` |

### TraceGraph

采集目标是构建基础 DAG。`torch` 提供 framework op、kernel、copy、stream 和 correlation 事实；`ld_preload` 可补充 runtime C++ 同步点或框架没有暴露的 native 边界。

必须采集 `pid`、`tid`、`rank`、`ts`、`dur`、`device_id`、`stream_id`、`correlation_id`、`event_kind`。这些字段用于生成 DAG node、CPU 顺序边、stream 顺序边和 runtime-device correlation 边。

### NodeScaleModule

采集目标是定位要缩放的已有 DAG 节点。只需要 `torch` 中的 op/kernel/copy 事件，重点字段是 `op_name`、`node_kind`、`dur`、`device_id`、`stream_id`。

### BandwidthModule

采集目标是按 bytes 和带宽重算数据搬运节点。`torch` 负责 device copy，`ld_preload` 负责 native IO 或 storage IO。关键字段是 `bytes`、`src`、`dst`、`direction`、`dur`。

### ParallelStrategyModule

采集目标是为 TP/DP/PP 等并行 what-if 提供 rank 和 collective 事实。`torch` 采 collective kernel 或 runtime launch，`ld_preload` 补 native 通信库边界，`python_probe` 可采框架调度中的 group、shard 和 request 归属。

### InterconnectModule

采集目标是定位 CPU-NPU、NPU-NPU 和跨 rank 通信。关键字段是设备身份、链路类型、bytes、collective 类型、原始 duration 和同步边。

### HiCacheModule

HiCache 是 profiling 当前最重要的子模块。它需要两类事实：

- cache 状态事实：当前操作涉及哪些 page，L1/L2/L3 发生了什么状态变化；
- DAG 映射事实：这些状态变化在原始 trace 中对应哪些 CPU op、storage IO、device copy、sync 或等待节点。

SGLang HiCache 相关调用链可以按下列路径理解：

```text
request enters scheduler
  -> prefix / radix lookup
  -> decide hit pages and missing pages
  -> load missing pages from host/storage/cache tier
  -> optional prefetch schedule
  -> decode / extend uses KV
  -> insert or update generated KV pages
  -> write through / write back / eviction / release
```

建议 Python probe 采集点：

| 采集点 | 渠道 | 需要字段 | 为什么采 |
| --- | --- | --- | --- |
| request 进入 scheduler | `python_probe` | `request_id`, `batch_id`, `seq_len`, `prefix_len`, `rank` | 把 cache 行为归属到请求和 batch。 |
| radix / prefix lookup | `python_probe` | `request_id`, `operation_id`, `page_size`, `page_identity`, `matched_pages`, `hit_pages` | 维护 cache 状态和验证 prefix match。 |
| cache load | `python_probe`, `ld_preload` | `operation_id`, `tier_src`, `tier_dst`, `num_pages`, `bytes`, `blocking` | 决定 DAG 中要保留、删除或新增的 load 节点。 |
| prefetch schedule | `python_probe` | `operation_id`, `planned_pages`, `policy`, `deadline_us`, `window_us` | 建模 ready / late / suppressed pages。 |
| prefetch completion | `python_probe`, `ld_preload` | `operation_id`, `completed_pages`, `ts`, `dur`, `bytes` | 判断 best-effort 和 timeout 下哪些 page 可用。 |
| cache insert / update | `python_probe` | `operation_id`, `page_identity`, `num_pages`, `dirty`, `backuped` | 维护 resident、dirty、backuped 状态。 |
| write / backup | `python_probe`, `ld_preload` | `operation_id`, `tier_src`, `tier_dst`, `page_identity`, `bytes`, `blocking` | 区分 write-through、write-back eviction 和 background write。 |
| eviction / release | `python_probe` | `operation_id`, `evicted_pages`, `dirty_pages`, `capacity_reason` | 验证容量模型并决定是否触发 writeback。 |
| storage read/write | `python_probe`，明确 native 符号后可补 `ld_preload` | `operation_id`, `request_id`, `hash_pages`, `completed_tokens`, `ts`, `dur` | 把 L3 查询、读取和写入绑定到 request/page 事实。 |

HiCache 字段采集说明：

| 字段 | 如何采集 | 为什么采 |
| --- | --- | --- |
| `request_id` | Python scheduler/request 对象参数或属性 | 建立 request-level 因果链。 |
| `operation_id` | HiCache operation 对象、scope id 或 probe 生成的稳定 id | 连接 lookup、load、prefetch、write、evict。 |
| `rank` | 环境变量或框架 rank 对象 | 区分多 rank cache 状态。 |
| `page_size` | cache 对象属性，例如 `self.page_size` | 计算 scenario page 对齐和 radix split。 |
| `page_identity` | page id、block index、prefix hash 或 path+offset 摘要 | 维护 resident/hit/miss/dirty/backuped。 |
| `matched_pages` / `hit_pages` | radix lookup 返回值摘要 | 验证 prefix match 和命中状态。 |
| `num_pages` / `bytes` | 参数、返回对象或 native IO 参数 | 计算 DAG 节点 duration 和 bandwidth。 |
| `tier_src` / `tier_dst` | cache operation 参数或常量 | 判断 L3->L2、L2->L1、L1->L2、L2->L3 等移动方向。 |
| `blocking` | 调用点语义或同步等待事件 | 区分 critical path 与后台搬运。 |
| `policy` | 源配置或当前对象字段 | 记录源运行使用的 policy 输入，不在 probe 中推断目标 policy。 |
| `deadline_us` / `window_us` | prefetch scheduler 参数 | 判断 timeout / best-effort 的证据。 |
| `dirty` / `backuped` | cache page metadata | 验证 write-back/write-through 状态。 |
| `capacity_reason` | evict 调用上下文或容量字段 | 解释 eviction 是否由容量触发。 |

HiCache 默认不采：

- 完整 token 列表；
- page key 明文；
- 大 tensor；
- 目标 scenario 的 policy decision；
- 目标 scenario 的 replay 行为。

这些内容属于 modeling，不属于 profiling。

当前 SGLang HiCache Python probe target set：

| target id | 函数 | 采集目的 |
| --- | --- | --- |
| `scheduler.prefetch_kvcache` | `Scheduler._prefetch_kvcache` | 记录 request 级 prefetch 决策入口。 |
| `hiradix.match_prefix` | `HiRadixCache.match_prefix` | 记录 radix lookup、device hit、host hit 和匹配节点。 |
| `hiradix.prefetch_from_storage` | `HiRadixCache.prefetch_from_storage` | 记录 L3->L2 prefetch 计划。 |
| `hiradix.check_prefetch_progress` | `HiRadixCache.check_prefetch_progress` | 记录 prefetch ready / ongoing 判断。 |
| `hiradix.pop_prefetch_loaded_tokens` | `HiRadixCache.pop_prefetch_loaded_tokens` | 记录本 request 实际从 L3 加载的 token 数。 |
| `hiradix.init_load_back` | `HiRadixCache.init_load_back` | 记录 L2->L1 load_back 入口和返回节点。 |
| `hiradix.load_back` | `HiRadixCache.load_back` | 记录实际 host->device load_back。 |
| `hiradix.insert` | `HiRadixCache.insert` | 记录 device cache insert / radix 更新。 |
| `hiradix.write_backup` | `HiRadixCache.write_backup` | 记录 L1->L2 backup / write_back。 |
| `hiradix.write_backup_storage` | `HiRadixCache.write_backup_storage` | 记录 L2->L3 storage write 计划。 |
| `hiradix.evict` | `HiRadixCache.evict` | 记录 device eviction 触发和结果。 |
| `controller.load` | `HiCacheController.load` | 记录 L2->L1 load enqueue。 |
| `controller.start_loading` | `HiCacheController.start_loading` | 记录 L2->L1 load stream 启动。 |
| `controller.write` | `HiCacheController.write` | 记录 L1->L2 write enqueue。 |
| `controller.start_writing` | `HiCacheController.start_writing` | 记录 L1->L2 write stream 启动。 |
| `controller.prefetch` | `HiCacheController.prefetch` | 记录 L3 prefetch operation 创建。 |
| `controller.storage_hit_query` | `HiCacheController._storage_hit_query` | 记录 storage hit page 和 token 证据。 |
| `controller.page_transfer` | `HiCacheController._page_transfer` | 记录 L3->L2 page transfer 执行。 |
| `controller.write_storage` | `HiCacheController.write_storage` | 记录 L2->L3 write operation 创建。 |
| `controller.page_backup` | `HiCacheController._page_backup` | 记录 L2->L3 page backup 执行。 |

## 输出

默认 profile manifest 记录：

| 字段 | 作用 |
| --- | --- |
| `profiling_ready` | 本轮采集是否完成或 dry-run 成功。 |
| `profiling.channels_enabled` | 实际启用的采集渠道。 |
| `profiling.python_probes_enabled` | Python probe 插件列表。 |
| `profiling.python_targets` | Python probe target 原始配置。 |
| `trace.torch_trace_dir` | torch trace 目录。 |
| `trace.ld_preload_trace_dir` | LD_PRELOAD trace 目录。 |
| `sidecar.python_probe_dir` | Python probe trace 目录。 |

默认 modeling 输入只能消费干净 trace 和 sidecar。Debug 日志、probe patch 状态、质量统计和临时诊断必须由显式 debug 开关输出，并与默认输入分离。

## 质量审计

真实 profiling 完成后先跑采集质量审计：

```text
python3 scripts/internal/profile_quality.py --manifest <run_dir>/profile_manifest.json
```

默认输出：

```text
<run_dir>/profile_quality.json
```

审计内容：

| 字段 | 作用 |
| --- | --- |
| `trace_files` | 三类 trace 文件数量，确认 manifest 是否指向真实产物。 |
| `configured_target_count` / `observed_target_count` | Python probe target 配置数和实际命中数。 |
| `missing_targets` | 完全没有命中的 target。 |
| `targets_with_missing_required_fields` | 出现必需字段缺失的 target。 |
| `exception_targets` | 出现 exception phase 的 target。 |
| `targets.*.field_presence` | 每个 target 的字段覆盖情况。 |
| `quality_ready` | 是否满足进入 modeling 的最低采集质量。 |
