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
scripts/profile.sh configs/experiments/hicache_state/profiling_hicache_state_mainline_one_matrix.json --experiment s1a_manual
```

dry-run 也优先使用同一个外层入口：

```bash
scripts/profile.sh configs/experiments/hicache_state/profiling_hicache_state_mainline_one_matrix.json --experiment s1a_manual --dry-run
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
python3 scripts/internal/profile_runner.py --config configs/experiments/hicache_state/profiling_hicache_state_mainline_one_matrix.json --experiment s1a_manual
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
| `profiling.python_probe.state_trace.enabled` | 仅 HiCache state validation 使用；开启后 runner 会设置 `TRACE_SIM_HICACHE_STATE_TRACE=1`，并给 HiCache targets 追加 validation-only `state_snapshot` 字段。 |
| `profiling.ld_preload.enabled` | 是否启用 LD_PRELOAD。 |
| `profiling.ld_preload.library` | LD_PRELOAD so 路径，通常是 `build/docker/sglang/lib/libhook.so`。 |
| `profiling.ld_preload.trace_output` | hook 输出文件基名；hook 会自动追加 `.rank<RANK>.pid<PID>.json`。 |
| `server` | 被测 server 启动命令和 ready URL。 |
| `bench` | workload driver 命令；runner 会移除 server 侧 probe 环境，避免误采 bench。 |

## Profiling suite 与输入矩阵

需要手动连续跑多组 profiling 并归档配置时，应使用 suite config。suite 的核心语义是：一个
config 固定一套采集配置，里面可以列出多个 server 配置和多个输入配置，再选择其中一组或多组
实验运行。

同一个 suite 内的 `profiling` 是共享采集配置。`matrix.servers[]`、`matrix.inputs[]` 和
`experiments[]` 不允许覆盖或 unset `profiling`；如果需要改采集渠道、probe target、torch
profiler 或 LD_PRELOAD 行为，应新建另一个 suite config。这样一次归档中的实验差异只来自
server 配置和输入配置，后续比较 profiling 结果时不会把采集变量混进来。

最简矩阵写法：

```json
{
  "name": "profiling_suite_name",
  "framework": "sglang",
  "profiling": {
    "enabled": true,
    "channels": ["python_probe", "ld_preload"]
  },
  "matrix": {
    "servers": [
      {
        "id": "s1a",
        "server": {
          "command": ["python3", "-m", "sglang.launch_server", "..."],
          "ready_url": "http://127.0.0.1:30000/get_model_info"
        },
        "env": {
          "SGLANG_HICACHE_FILE_BACKEND_STORAGE_DIR": "{run_dir}/hicache_storage"
        }
      },
      {
        "id": "s1b",
        "server": {
          "command": ["python3", "-m", "sglang.launch_server", "..."],
          "ready_url": "http://127.0.0.1:30000/get_model_info"
        }
      }
    ],
    "inputs": [
      {
        "id": "manual",
        "bench": {
          "command": ["python3", "scripts/bench/hicache_phased_workload.py", "..."]
        }
      },
      {
        "id": "bench",
        "bench": {
          "command": ["python3", "-m", "sglang.bench_serving", "..."]
        }
      }
    ]
  }
}
```

如果 `matrix` 下没有显式 `experiments`，runner 会按 `servers × inputs` 全量展开，实验
id 为 `<server_id>_<input_id>`，例如 `s1a_manual`、`s1a_bench`、`s1b_manual`、
`s1b_bench`。如果只希望在归档 config 中保留部分组合，可以显式写：

```json
{
  "experiments": [
    {"id": "s1a_manual", "server_ref": "s1a", "input_ref": "manual"},
    {"id": "s1b_bench", "server_ref": "s1b", "input_ref": "bench"}
  ]
}
```

手动运行前先列出展开后的实验：

```bash
scripts/profile.sh configs/experiments/<domain>/<suite>.json --list-experiments
```

只跑其中几个实验：

```bash
scripts/profile.sh configs/experiments/<domain>/<suite>.json --experiment s1a_manual --experiment s1b_bench
scripts/profile.sh configs/experiments/<domain>/<suite>.json --experiments s1a_manual,s1b_bench
```

suite 运行会在 suite 输出目录中写入：

| 文件 | 作用 |
| --- | --- |
| `suite_config.json` | 本次使用的原始 suite config，作为归档入口。 |
| `suite_selection.json` | 全部可选实验、命令行选择器和本次计划运行的实验。 |
| `suite_result.json` | 已完成 run 目录和失败项。 |

每个展开后的 experiment config 会自动补充 `metadata.suite_experiment_id`、
`metadata.suite_server_id` 和 `metadata.suite_input_id`。如果只选择子集运行，run id 仍保留
原矩阵序号，例如只跑第 1 和第 4 个实验时目录前缀仍是 `01_` 和 `04_`，方便之后把结果对回
完整归档矩阵。

显式 `server.command`、`bench.command`、`bench.args` 和 `env` 字符串支持两类占位符：

| 占位符 | 作用 |
| --- | --- |
| `{run_dir}`、`{trace_dir}`、`{bench_dir}`、`{log_dir}` | 当前展开实验的输出目录。 |
| `{metadata.foo}`、`{server.foo}`、`{bench.foo}`、`{env.foo}`、`{modeling.foo}` | 当前展开实验合并后的点分配置字段。 |

点分配置占位符用于让 input 维度引用 server 维度带来的参数。例如 manual workload 的
`--cache-write-policy` 可以写成 `{metadata.hicache_write_policy}`，这样 input 仍只维护一份，
S1A/S1B 展开后分别得到自己的 write policy。

当前已归档的 mainline-one profiling 入口是一个 suite matrix config：

```bash
scripts/profile.sh configs/experiments/hicache_state/profiling_hicache_state_mainline_one_matrix.json --list-experiments
scripts/profile.sh configs/experiments/hicache_state/profiling_hicache_state_mainline_one_matrix.json --experiment s1a_manual
scripts/profile.sh configs/experiments/hicache_state/profiling_hicache_state_mainline_one_matrix.json --experiments s1a_manual,s1b_manual
```

该 config 只保留两个 server 维度 `s1a` / `s1b` 和两个 input 维度 `manual` / `bench`。
它依赖 `target_page_identity_page64` / `target_page_identity_page128` 这类有限 page-size
字段，只能作为旧批次归档入口。下一次集中重采不得继续扩展该 page-identity 契约，应按本文
`HiCacheModule` 小节定义的新 token/range invariant contract 新建 suite。

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

Base DAG 验证优先使用 SGLang 自带 `sglang.bench_serving`，是为了先降低 workload 和
采集组合变量，不是为了定义 faithful replay 过滤规则。原则上，faithful replay 应消费完整真实执行
trace；如果某个实验采集了 HiCache 或 CPUInfer 真实执行事件，这些事件也应进入 merged trace 并参与
base DAG 构建。

清理后的 `configs` 目录不再保留单独的 base DAG 采集入口。后续如果需要重新跑 base DAG
验证，应新增明确命名的 profiling suite，并在同一变更中补齐文档和 fixture；不要引用已删除的
历史配置路径。bench 输出 `bench/bench.jsonl` 时，modeling runner 仍会用最后一行 `duration`
作为 faithful replay 的 actual E2E。

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

`--hicache-ratio` 可以按实验目标调整，但必须大于 `1.0`，并且需要在配置说明或验证记录中写明原因。
如果容量压力不足，优先增加 workload 的长上下文、压力请求数或使用显式 capacity 配置；不能用小于等于
`1.0` 的 ratio 构造实验。

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
| `TRACE_SIM_HICACHE_STATE_TRACE=1` | 只在 HiCache state validation 中启用，允许 `sglang.hicache` 采集 `state_snapshot`。 |

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

`page_hashes_concat:<prefix_tokens>,<tokens>,<page_size>[,<prior_hash>]` 也只由
`sglang.hicache` probe 支持，用于先拼接两段 token path，再按目标 page size 重新
计算完整 path 的 page identity。

`page_hashes_after_prefix:<prefix_tokens>,<tokens>,<page_size>[,<prior_hash>]` 只由
`sglang.hicache` probe 支持，用于 page size what-if 下的 prefetch。它用
`prefix_tokens` 在目标 page size 下重新计算 parent hash，但只输出 `<tokens>` 对应的
suffix pages，避免把已经命中的 prefix pages 混入 prefetch planned set。base run 的
`last_hash` 属于 base page size，不能直接作为 target page size 的 parent hash。

输出格式是 Chrome trace JSON：

```text
trace/python_probe/python_probe_trace.rank<RANK>.pid<PID>.json
```

每个事件的事实字段写在 `traceEvents[].args` 中。Python probe 输出需要显式区分输入路由：

| 事件类型 | `model_input` | `dag_input` | `state_model_input` | 说明 |
| --- | --- | --- | --- | --- |
| 真实执行事件 | true | true | false | 例如 CPUInfer submit/sync、真实 runtime anchor；faithful replay 必须能消费。 |
| HiCache invariant fact | true | false | true | 例如 token dictionary、lookup path、insert path、prefetch intent；只进入 HiCache state model，不作为默认性能 DAG 节点。 |
| timing / source actual | true 或 false | 按实验决定 | false | 例如 source movement、IO duration、actual prefetch/writeback；不能作为 target state 输入。 |
| 验证 / debug / oracle 事件 | false | false | false | 例如完整 cache state snapshot、模型预测差异、probe 内部状态、质量审计证据；只服务 validation/debug。 |

默认 modeling 输入应消费真实执行事件和显式 `state_model_input=true` 的子模块事实。`model_input=false`
的 Python probe 事件只能由显式 validation/debug 逻辑读取。

当前已归档的 HiCache state validation 配置入口：

```bash
scripts/profile.sh configs/experiments/hicache_state/profiling_hicache_state_mainline_one_matrix.json --list-experiments
scripts/profile.sh configs/experiments/hicache_state/profiling_hicache_state_mainline_one_matrix.json --experiment s1a_manual
```

该 suite 是旧 page-identity HiCache state 采集入口，只启用 `python_probe`，关闭 torch profiler 和
LD_PRELOAD。下一次集中重采应新建 token/range invariant suite；旧入口不能替代 faithful replay，
需要验证 base DAG 或 cache patch 时仍应使用完整真实执行 trace，并补充对应配置入口。

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
| `model_input` | 是否作为默认建模输入。新配置应优先使用 `dag_input` / `state_model_input` 表达更细粒度路由，`model_input` 只保留为合并入口的总开关。 |
| `dag_input` | 是否进入默认性能 DAG。非执行类 invariant fact、oracle 和 debug 事件必须为 false。 |
| `state_model_input` | 是否允许功能子模块作为状态模型输入消费。只有显式不变量事实可以为 true。 |
| `fact_class` | 子模块事实分类，例如 `invariant_state`、`timing_observation`、`source_actual`、`oracle_state`、`debug_quality`。 |

按需字段不应全局默认采集。对象明文和大 tensor 不应作为默认字段。HiCache 新采集契约允许采集
去重后的 token dictionary，因为它是跨 page size 重建 radix tree 的必要事实；普通事件不得重复携带完整 token 列表。

`scripts/internal/profile_quality.py` 是 profiling 后的质量审计入口。它读取
`profile_manifest.json`，检查：

- trace 文件是否包含 torch / LD_PRELOAD / python probe；
- Python probe target 是否命中；
- required 字段是否缺失；
- `workload_report.json` 声明的 HiCache 机制是否实际出现；
- HiCache invariant 事件是否具备 token dictionary / token span / seq_no 等必要事实；
- HiCache state trace 开启时是否采到了 validation-only `state_snapshot.capacity`。

controller start/enqueue 这类队列锚点允许 count-only；真正进入 `invariant_state` 的 HiCache
事件缺 token dictionary、token span、`cache_scope` 或 `seq_no` 时，`quality_ready=false`。
如果 `profiling.python_probe.state_trace.enabled=true` 但没有任何 capacity snapshot，
`quality_ready=false`，因为跨配置 capacity prediction 缺少有效 budget 证据。

## 子模块采集矩阵

不同 modeling 子模块只启用自己需要的事件。

| 子模块 | 需要事件 | 推荐渠道 | 关键字段 |
| --- | --- | --- | --- |
| `TraceGraph` | CPU op、runtime launch、device kernel、copy、sync/wait | `torch`, `ld_preload` | `device_id`, `stream_id`, `correlation_id`, `op_name`, `dur` |
| `NodeScaleModule` | 被缩放的 op/kernel/copy 节点 | `torch` | `op_name`, `node_kind`, `dur` |
| `BandwidthModule` | memcpy、host-device copy、device-device copy、storage read/write | `torch`, `ld_preload` | `bytes`, `src`, `dst`, `direction` |
| `ParallelStrategyModule` | rank 启动、collective、send/recv、barrier、shard 信息 | `torch`, `ld_preload`, `python_probe` | `rank`, `world_size`, `group_id`, `collective_kind`, `bytes` |
| `InterconnectModule` | CPU-NPU copy、NPU-NPU copy、collective、runtime sync | `torch`, `ld_preload` | `src_device`, `dst_device`, `bytes`, `link_kind` |
| `HiCacheModule` | request token path、radix lookup/insert、logical prefetch、capacity request、lock/ref scope、异步 IO 观测 | `python_probe`, `ld_preload`, `torch` | `request_id`, `operation_id`, `token_path_id`, `token_span`, `fact_class`, `tier_src`, `tier_dst`, `bytes` |

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

HiCache 新采集目标面向重构后的 C++ 后端，不要求兼容现有 page-identity state model。新的原则是：

- profiling 只采事实，不生成 target 行为；
- state model 只消费 `fact_class=invariant_state` 的事实；
- 后端用 token/range 事实按目标配置重建 page、radix node、lock/ref chain 和 evictable set；
- source run 实际 movement、policy decision 和 state snapshot 只能进入 `timing_observation`、`source_actual` 或 `oracle_state`；
- `page_identity` 不再是 state model 主输入，只能作为 validation/debug 或旧实验解释字段。

#### 事件分类契约

HiCache Python probe target 必须显式写入 `fact_class`。后端入口只用该字段做第一层分流，不能再靠
role 字符串推断某个事件是否是不变量。

| `fact_class` | `state_model_input` | `dag_input` | 语义 | 典型事件 |
| --- | --- | --- | --- | --- |
| `invariant_state` | true | false | target 配置无关的状态事实，是 HiCache state model 唯一主输入。 | request tokens、lookup path、insert path、prefetch intent、capacity request、lock scope delta |
| `timing_observation` | false | true 或 false | source run 的耗时、bytes、IO completion 观测。它可以训练或参数化 latency model，但不能直接决定 target resident state。 | storage read/write duration、L3->L2 transfer duration、writeback duration |
| `source_actual` | false | true 或 false | source run 实际发生的 movement 或 policy 结果。它可以用于 faithful replay、profile quality 或 oracle 对照，不能作为 target-state answer。 | load_back、actual prefetch enqueue、actual victim page、actual remove page |
| `oracle_state` | false | false | validation-only 状态答案。 | ordered state transition oracle、periodic/final state snapshot、actual locked/evictable set |
| `debug_quality` | false | false | probe 内部排查和质量审计。 | source node id、queue size、best_match_node_id、producer id |

`model_input=true` 只能表示事件进入 modeling 输入集合；是否进入性能 DAG 由 `dag_input` 决定，是否进入
HiCache state model 由 `state_model_input` 和 `fact_class` 决定。非执行类 `invariant_state`
事件不得被当作默认 DAG 节点。

#### Token / Range 主事实

新的 invariant profile 必须采集 token-level 事实。为了避免每个事件重复大列表，token 序列采用
dictionary + span 引用：

| 字段 | 必需性 | 说明 |
| --- | --- | --- |
| `token_path_id` | 必需 | 完整 token 序列的稳定内容 hash，例如 `sha256_u32le:<hex>`。 |
| `token_count` | 必需 | 该 token path 的 token 数。 |
| `token_ids` | dictionary 必需 | 只在 token dictionary 事件中出现，保存完整 token id 序列；普通事件只能引用 span。 |
| `token_span` | 普通事件必需 | `{path_id, begin, end}`，闭开区间。 |
| `prefix_span` | 按 role 必需 | prefix / matched path 对应的 span。 |
| `suffix_span` | 按 role 必需 | new input、inserted suffix 或 prefetch suffix 对应的 span。 |
| `full_path_span` | 按 role 必需 | 当前 cache key 的完整 logical path。 |
| `hash_algo` | 必需 | HiCache page hash 算法版本，例如 `sglang_radix_sha256_v1`。 |
| `cache_scope` | 必需 | rank / worker / cache object 作用域，避免多进程状态混合。 |
| `seq_no` | 必需 | 同一 `cache_scope` 内单调递增的逻辑顺序号，用于后端重放。 |

后端根据 `token_ids`、`hash_algo` 和 target `page_size` 自己生成 page identity。新增 page size
不需要重新 profile。`target_page_identity_page64`、`target_page_identity_page128` 这类有限枚举字段不再进入新契约。

token dictionary 事件本身是 `fact_class=invariant_state`、`state_model_input=true`、
`dag_input=false`。它是后端重建 target page 和 radix tree 的辅助事实，不是业务执行节点。

#### Invariant State 采集点

默认 HiCache state profile 只保留下列不变量采集点。采集对象可以来自现有 SGLang callable，也可以在
probe 中新增更贴近语义的 wrapper；命名以 role 语义为准，不要求沿用旧 target id。

| role | 建议采集对象 | 必需字段 | 后端用途 |
| --- | --- | --- | --- |
| `request_tokens` | request 进入 scheduler 或 cache lookup 前的请求对象 | `request_id`, `full_path_span`, `token_count`, `cache_scope`, `seq_no` | 建立 request 到 token path 的全局事实。 |
| `lookup_path` | `HiRadixCache.match_prefix` | `request_id`, `full_path_span`, `matched_token_len` 或 `matched_span`, `cache_scope`, `seq_no` | 在 target radix tree 上重放 lookup、touch 和 prefix hit。 |
| `insert_path` | `HiRadixCache.insert` | `request_id`, `full_path_span`, `value_token_count`, `prefix_len`, `cache_scope`, `seq_no` | 在 target radix tree 上执行 split / merge / insert，并生成 target pages。 |
| `prefetch_intent` | `HiRadixCache.prefetch_from_storage` | `request_id`, `prefix_span`, `suffix_span`, `policy_params`, `cache_scope`, `seq_no` | 后端按 target policy 和 target page size 生成 planned prefetch pages。 |
| `prefetch_check_point` | `HiRadixCache.check_prefetch_progress` | `request_id`, `check_kind`, `cache_scope`, `seq_no` | 给 target async prefetch scheduler 提供请求时间线上的等待/检查边界。 |
| `capacity_request` | `HiRadixCache.evict` 或容量检查入口 | `requested_tokens`, `requested_pages_source`, `reason`, `tier`, `cache_scope`, `seq_no` | 后端按 target capacity 和 evictable predicate 选择 victim。 |
| `lock_scope_delta` | `HiRadixCache.inc_lock_ref` / `dec_lock_ref` | `logical_path_span`, `delta`, `request_id` 或 `operation_id`, `cache_scope`, `seq_no` | 后端在 target radix tree 上沿 parent chain 更新 lock/ref。 |
| `cache_config_observed` | cache/controller 初始化或 snapshot 轻量事实 | `source_page_size`, `write_policy`, `prefetch_policy`, `thresholds`, `capacity_summary`, `cache_scope` | 记录 source 配置和质量审计；target 配置仍由 modeling config 显式提供。 |

`matched_token_len`、`prefix_len`、`requested_tokens` 这类长度事实是建模输入；source run 返回的
`best_match_node_id`、`last_host_node_id`、actual victim page 和 actual movement page 不是建模输入。

#### Runtime / Movement 观测

以下事件从默认 `invariant_state` profile 中移出。需要性能建模或 faithful replay 时可以单独启用，但必须标成
`timing_observation` 或 `source_actual`：

| role | `fact_class` | 用途 |
| --- | --- | --- |
| `l2_to_l1_load_observed` | `source_actual` 或 `timing_observation` | 真实 load_back 路径和耗时样本，不直接更新 target L1。 |
| `l1_to_l2_write_observed` | `source_actual` 或 `timing_observation` | 真实 backup/write-back 路径和耗时样本，不直接更新 target L2。 |
| `l2_to_l3_write_observed` | `source_actual` 或 `timing_observation` | 真实 storage write 路径和耗时样本。 |
| `l3_to_l2_transfer_observed` | `timing_observation` | 真实 prefetch/read completion 的 bytes/tokens/duration；target ready 状态由 async model 决定。 |
| `remove_page_observed` | `source_actual` | source radix/page removal oracle，不能作为 target removal。 |
| `prefetch_decision_observed` | `source_actual` | source scheduler decision，不能替代 target prefetch policy。 |

这些观测事件如果进入性能 DAG，`dag_input=true`；如果只作为 latency sidecar 或 validation 证据，
`dag_input=false`。无论哪种情况，`state_model_input=false`。

#### Oracle 与质量采集

为了验证新后端，应保留独立 oracle profile，但默认预测不得消费：

| oracle | `fact_class` | 说明 |
| --- | --- | --- |
| `state_snapshot` | `oracle_state` | 周期性或最终 cache state，包含 node、resident、dirty、backuped、evicted、lock/ref、capacity。 |
| `ordered_transition_log` | `oracle_state` | source run 中真实 state transition 的有序日志，用于定位 prediction mismatch。 |
| `evictable_snapshot` | `oracle_state` | 当前可驱逐集合、被保护集合和拒绝原因，用于验证 target evictable predicate。 |
| `async_operation_snapshot` | `oracle_state` | ongoing prefetch/writeback operation、progress、queue、completion，用于验证 async scheduler。 |
| `profile_quality_marker` | `debug_quality` | target 命中、字段缺失、token dictionary 引用完整性和顺序号连续性。 |

state snapshot 可以继续由 `state_trace.enabled` 控制，但它必须保持 `state_model_input=false`。
如果需要从 snapshot 派生最小 operation-level oracle，也只能进入 validation 路径，不能混入 `invariant_state`。

#### Radix Tree 重建目标

重构后的后端应从 `invariant_state` 事实重建 target HiCache radix tree：

1. 用 token dictionary 解析所有 `token_span`。
2. 按 `cache_scope` 和 `seq_no` 排序重放请求。
3. `lookup_path` 在 target radix tree 上查找最长 prefix，并更新 target touch/hit 状态。
4. `insert_path` 在 target radix tree 上执行 token-level split、merge 和 node 创建。
5. 用 target `page_size` 和 `hash_algo` 为 node/range 派生 page identity。
6. node 状态维护 `L1/L2/L3 resident`、`dirty`、`backuped`、`evicted`、`lock_ref`、`hit_count` 和 touch order。
7. `lock_scope_delta` 在 target tree 上找到 logical path 后沿 parent chain 更新，而不是消费 source node id。
8. `capacity_request` 按 target capacity、lock/ref、dirty/backuped 和 policy 选择 evictable victim。

因此，新 profile 不再采集 target page identity 矩阵，也不依赖 source movement 来补 target state。

#### Async Prefetch 采集边界

Prefetch 分成 intent、timing observation 和 oracle 三类，不能混用：

| 采集事实 | `fact_class` | 必需字段 | 建模语义 |
| --- | --- | --- | --- |
| `prefetch_intent` | `invariant_state` | `request_id`, `prefix_span`, `suffix_span`, `policy_params`, `seq_no` | target 后端据此决定是否计划、计划哪些 target pages。 |
| `prefetch_check_point` | `invariant_state` | `request_id`, `check_kind`, `seq_no` | 表示请求时间线上的检查/等待点，不携带 source ready 答案。 |
| `prefetch_io_observed` | `timing_observation` | `operation_id`, `token_span`, `completed_tokens`, `bytes`, `ts`, `dur` | latency / bandwidth 样本；不能直接标记 target page ready。 |
| `prefetch_actual_state` | `oracle_state` | actual planned/ready/late/suppressed、operation progress、queue state | validation only。 |

target ready/late/suppressed 必须由后端 async scheduler 结合 target policy、target page set、IO latency model 和
check point 推导。source run 的 actual ready pages 只能做 oracle。

#### Writeback Flush 采集边界

Writeback 也分成 target 可重建输入、耗时观测和 oracle：

| 采集事实 | `fact_class` | 必需字段 | 建模语义 |
| --- | --- | --- | --- |
| `writeback_trigger_input` | `invariant_state` | `reason`, `requested_tokens/pages`, `policy_params`, `cache_scope`, `seq_no` | 描述容量或 policy 触发条件，不包含 source victim page。 |
| `writeback_io_observed` | `timing_observation` | `operation_id`, `token_span`, `bytes`, `completed_tokens`, `ts`, `dur` | flush/write latency 样本。 |
| `writeback_actual_state` | `oracle_state` | actual dirty->backuped、actual flushed pages、completion order | validation only。 |

如果 SGLang 的 flush decision 依赖隐藏队列或后台线程状态，profiling 应采 policy predicate 和 queue
条件作为观测或 oracle，而不是把 source flushed page 当作 target answer。

#### 新配置收敛目标

下一次集中重采应使用新的 HiCache suite，而不是继续扩展当前 mainline-one page-identity 配置。新 suite
至少拆成三组 profile target：

| target group | 默认启用 | 内容 |
| --- | --- | --- |
| `hicache_invariant_state` | 是 | token dictionary、request tokens、lookup path、insert path、prefetch intent/check point、capacity request、lock scope delta、source config summary。 |
| `hicache_timing_observation` | 按性能建模实验启用 | load/write/storage/prefetch IO 的 bytes、tokens、duration、operation id。 |
| `hicache_oracle_debug` | 按 validation 实验启用 | state snapshot、ordered transition log、evictable snapshot、async operation snapshot、profile quality marker。 |

同一个 suite 内不得让 input/server 维度覆盖 target group。需要改变采集类别时新建 suite，避免把
采集契约差异混进实验变量。

#### 重采前验收

重采前必须先用 fixture 固化新契约，至少覆盖：

- token dictionary 去重和 span 引用完整性；
- lookup / insert 的 token path 到 target page hash 推导；
- insert split / merge 和 removed node oracle 对照；
- lock/ref parent chain 在 target tree 上的重放；
- capacity request 下 locked page 不可驱逐；
- write-back dirty eviction 与 background flush 的边界；
- prefetch intent、check point、IO observation 和 oracle 的分离；
- `fact_class` 缺失、错误分类或 `state_model_input` 污染时 quality fail。

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

这里的“干净 trace”不是指删除 HiCache、CPUInfer 或其他领域的真实执行事件，而是指默认性能输入中
不能混入非执行类 state snapshot、oracle、debug 和质量审计事件。真实执行事件属于 faithful replay 的
必要输入；验证类事件属于辅助输入。

HiCache token/range invariant fact 是建模输入但不是执行节点。新后端读取 `dag_input=false` 且
`state_model_input=true` 的事件时，必须保留为 auxiliary fact 或 sidecar fact，不得创建默认 DAG node。

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
