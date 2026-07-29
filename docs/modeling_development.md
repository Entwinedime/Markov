# 建模开发文档

维护方式：这是 modeling 主线设计文档。更新时直接删改本文件内容，不写流水账、实验结果或阶段分析。
真实 run、验证结果和历史结论维护在 `docs/work_progress.md` 或 `docs/validation/`。

## 目标

Modeling 基于 profiling 事实构建 C++ TraceGraph，并在目标配置下通过子模块维护状态或修改 DAG。

默认流程：

```text
profile manifest
  -> C++ manifest trace input
  -> in-memory torch / LD_PRELOAD / Python probe event merge
  -> C++ TraceGraph
  -> optional SimulationModule
  -> topological simulation
  -> prediction.json / optional summary / optional validation
```

默认主输出：

```json
{
  "predicted_e2e_us": 0
}
```

`predicted_e2e_us` 来自 materialized active DAG 拓扑仿真，单位与 Chrome trace 一致，为微秒。启用 calibrated HiCache DAG patch
时，该图已经包含 target-derived I/O/control mutation；但 prefill 仍为 `deferred`，raw cross-config real E2E 也不是当前
direct I/O/control v1 的正确性门禁。

## 运行入口

Modeling 后端是 C++23 TraceGraph，构建和运行基线是独立的 `modeling` Docker service。该 service 基于干净
Ubuntu 24.04，只提供 C++23、CMake、Ninja、clang-format/clang-tidy、Python 标准运行环境和 modeling 脚本依赖；
不挂载 Ascend 设备，不依赖 CANN，不安装 SGLang / KTransformers runtime。宿主机负责外层编排，包括直接运行
`modeling_workflow.py` 和启动低层 container wrapper；不再支持直接在宿主机用 `scripts/internal/entrypoints/model.py`
执行 modeling run、host build `trace_graph` 或旧 `build/bin` 产物。

构建 modeling 环境：

```bash
scripts/build.sh modeling
```

进入 modeling 环境：

```bash
scripts/run.sh modeling
```

在 modeling 容器内构建 TraceGraph：

```bash
cmake -S src/modeling/trace_graph -B build/modeling/trace_graph -G Ninja
cmake --build build/modeling/trace_graph --target trace_graph -j2
```

宿主机也可以通过一次性命令执行同一检查：

```bash
scripts/run.sh modeling -- bash -lc \
  'cmake -S src/modeling/trace_graph -B build/modeling/trace_graph -G Ninja && cmake --build build/modeling/trace_graph --target trace_graph -j2'
```

不维护 fixture-backed smoke modeling 入口。Modeling 验证必须基于真实 profile manifest，或专项验证文档中记录的
可复现 profile/modeling run。

当前也不维护静态 `configs/modeling/` 文件。post-profile modeling 主流程从仓库根目录直接运行
`python3 scripts/internal/entrypoints/modeling_workflow.py`，由 unified workflow 统一编排。该 workflow 根据选中的
validation object 从 profile suite 的 target server metadata 动态生成 Python runner config，写入
`<workflow_output>/model_runs/<model_run_id>/runner_config.json`；每个 model run 输出目录下的 `cpp_model_config.json`
是 C++ TraceGraph backend narrow config。

低层单次 modeling execution 只能消费 workflow 生成的自包含 runner config：

```bash
scripts/model.sh \
  --config <workflow_output>/model_runs/<model_run_id>/runner_config.json
```

`runner_config.json` 自包含 `input.profile_manifest`、`output_dir`、`mode`、`cpp_trace_graph`、`outputs`、oracle path 和
`cpp_model_config`。`model.py` 只接受 `--config`，不再用 CLI 覆盖同一状态。validation requirement 会自动选择
`cpp_trace_graph.backend_kind="validation"` 并打开所需 artifact；普通 business prediction 使用 Release backend。

## 脚本分层

Modeling 相关脚本同样按 wrapper、entrypoint 和可复用包分层：

| 层级 | 路径 | 职责 |
| --- | --- | --- |
| 低层 wrapper | `scripts/model.sh` | 启动 modeling 容器并转发唯一的 `--config`；workflow 用户不以它作为主入口。 |
| 容器内入口 | `scripts/internal/entrypoints/model.py` | 只解析自包含 `--config`，调用 `markov_internal.modeling.runner`。 |
| 单次 run contract | `scripts/internal/markov_internal/modeling/run_config.py` | 校验 runner config、路径、backend 和输出选择。 |
| C++ command | `scripts/internal/markov_internal/modeling/backend.py` | 把 normalized run config 转成窄 C++ CLI 并执行。 |
| trace channel | `scripts/internal/markov_internal/modeling/trace_channels.py` / `trace_inputs.py` | 统一 channel 名称；后者只为 validation 记录 C++ 实际消费的原始文件。 |
| C++ config / binary | `scripts/internal/markov_internal/modeling/cpp_config.py` | 生成窄 C++ model config、解析 target HiCache 参数，并按 backend 定位 Release 或 Debug/validation binary。 |
| HiCache backend adapter | `scripts/internal/markov_internal/modeling_workflow/validations/hicache/backend/` | 在 workflow composition root 中把 C++ summary 转成 predicted state trace，并组装 faithful replay / HiCache validation 和 recommended config audit。 |
| workload helper | `scripts/internal/markov_internal/modeling/workload.py` | 读取 workload report / bench JSONL 中的实际运行窗口。 |
| workflow 入口 | `scripts/internal/entrypoints/modeling_workflow.py` | profiling 后统一 modeling workflow 入口；用户直接运行该 Python entrypoint，选择 validation object，不选择底层 C++ debug flag。 |
| workflow 包 | `scripts/internal/markov_internal/modeling_workflow/` | preflight、plan、model-runs、validations、artifact layout、runner adapter 和 workflow summary。 |

Unified modeling workflow 是 profiling 后的 validation / analysis 编排，不属于 C++ 后端主体。它的固定阶段是：

```text
preflight -> plan artifact -> model-runs -> validations -> workflow-summary
```

`plan artifact` 是瞬时内存规划和 `model_run_plan.json` 写出，不单独打印无实际工作的 console stage。

validation object 是 C++ output 的读取视角，不是独立 workflow：

| 路径 | 职责 |
| --- | --- |
| `validations/base_dag/` | 请求 faithful replay + base DAG diagnostics，解释 `dag_quality.json` / `dag_analysis.json`。 |
| `validations/hicache/dag_mapping.py` | 复用 base DAG faithful replay run，解释 HiCache anchor coverage / operation visibility。 |
| `validations/hicache/final_state.py` | 请求或复用 cache-state run，保留 raw final-state exactness，并用独立 closure 报告区分 exact、async readiness limitation、schedule sensitivity 和未闭合错误。 |
| `validations/hicache/transition/` | 复用 cache-state run 和 validation artifact，保留 raw transition exactness，并把 readiness、snapshot observability、cross schedule 和其它错误分栏。 |
| `validations/final_dag/` | 读取 active patched graph、attribution/rewrite/boundary/post-apply artifact 和 target shape oracle；raw shape 与 patch-local acceptance 分开报告。 |
| `scripts/internal/markov_internal/modeling_workflow/progress.py` | workflow 共享 TTY progress reporter。 |

transition oracle 只消费 `source_actual` / `timing_observation` evidence 和 `oracle_state` snapshot 来做验证标签；
它不生成 state-model fact，也不消费 runtime storage-control checkpoint。C++ state model 的输入仍只由 fact contract 和 router
决定。

Unified modeling workflow 的 Python 编排以 validation object 和 model-run planner 为顶层结构：

```text
WorkflowContext
  -> WorkflowArtifactLayout
  -> WorkflowProgressReporter
  -> selected ValidationRequest
  -> PreflightRunner
  -> ModelRunSpec planner
  -> runner adapter / model executor
  -> validation artifact analyzers
```

约束：

- `modeling_workflow/cli.py` 只负责解析 CLI 并进入 `WorkflowRunner`；run discovery、preflight、plan、execute、validation 和 summary 由 workflow 对象分层负责；
- validation object 只声明 preflight checks、model-run requests、output requirements 和 artifact 解释逻辑，不直接拼 `scripts/model.sh` 命令；
- runner adapter 负责把 semantic output requirement 翻译成当前 `model.py` runner config / output flag；
- `WorkflowProgressReporter` 是 workflow 用户可见进度的唯一 owner；TTY 下刷新动态进度行，非 TTY 下只输出阶段 start/done summary；
- selected validation 未请求的 quality check、C++ debug output 和 Python artifact 读取路径都不能执行。

当前 unified workflow artifact 布局：

```text
<workflow_output>/
  workflow_summary.json
  preflight_summary.json
  model_runs/
    <model_run_id>/
      runner_config.json
      execution.json
      prediction.json
      run_summary.json
      cpp_model_config.json
      model_summary.json
      validation.json
  artifacts/
    model_run_plan.json
    preflight/
    model_runs_summary.json
    validations/<validation_name>/
      summary.json
      <model_run_id>.json
```

使用约束：

- `workflow_summary.json`、`preflight_summary.json`、`artifacts/model_runs_summary.json` 和
  `artifacts/validations/<validation_name>/summary.json` 是用户第一入口，只保留阶段级计数和分组摘要，不嵌入 per-run/per-cell rows；
- `model_runs/<model_run_id>/runner_config.json` 是 Python runner config，供 `scripts/model.sh --config` 使用；
- `model_runs/<model_run_id>/execution.json` 记录该 cell 的执行、复用、跳过或失败状态；
- `model_runs/<model_run_id>/cpp_model_config.json` 是 C++ TraceGraph backend narrow config，供 `trace_graph --model-config` 使用；
- `run_summary.json` 使用 `markov.trace_graph.run_summary.v3`，只保留 graph 业务计数、微秒单位的 E2E、module business result；
  validation build 额外包含 `stage_timings_ms`。不重复记录 config/path/thread 字段，这些信息已经存在于 runner config；
- per-run audit、transition catalog、gate、model run cell 产物都是复现/诊断 artifact，不应和用户第一入口混称；
- 默认 console 输出不逐 run/cell 打印 `result ok ...`；失败时只补充失败数量、少量 sample 和关键 artifact 路径。

HiCache Python helper 的当前职责分组：

| 分组 | 模块 | 职责 |
| --- | --- | --- |
| 共享核心 | `modeling_workflow/validations/hicache/core/` | fact metadata、consumer 路由和 token dictionary/span 辅助工具。 |
| 输入合同 | `modeling_workflow/validations/hicache/input_contract/` | canonical workload signature、path contract 和 source/target report。 |
| preflight | `modeling_workflow/validations/hicache/preflight/` | HiCache workflow input gate、state fact coverage、forced-token、sequence 诊断和 strict diagnostic coverage。 |
| oracle | `modeling_workflow/validations/hicache/oracle/` | final-state oracle、capacity/config audit、predicted records、coverage、delta 和 mismatch provenance。 |
| transition | `modeling_workflow/validations/hicache/transition/` | predicted transition schema/replay/self-check、target oracle、single/prediction-set compare、catalog、gate 和 taxonomy。 |
| workflow | `modeling_workflow/` | post-profile preflight、model-run planning/execution、validation object、artifact layout、progress 和 workflow summary。 |

不再维护独立 `markov_internal/hicache/` 包；HiCache validation helper 必须落到 unified workflow 的 HiCache validation 子包，
避免重新依赖文件名前缀维持边界。

## 建模模式

| mode | 语义 |
| --- | --- |
| `faithful_replay` | 不加载子模块，不 patch DAG；消费完整真实执行 trace，验证 base DAG。 |
| `cache_state` | 执行 target-derived HiCache state replay，生成完整 effect decision ledger；请求 final DAG 时继续完成 source attribution、resource planning 和一次原子 DAG patch。 |

`replay` 只允许指 `mode=faithful_replay`。启用 HiCacheModule 的场景必须称为 `self-config prediction` 或
`cross-config prediction`。

## Trace 输入

C++ TraceGraph 直接读取 profile manifest 中的 trace channel，并在进程内完成合流：

- torch profiler trace；
- LD_PRELOAD trace；
- Python probe sidecar。

Python modeling runner 不调用 trace 合并脚本，也不写 `merged_trace` 中间产物。C++ `trace_manifest_input` 只负责 manifest
选路、logical input 并行读取和 channel 文件配对；`trace_channel_join` 在同一进程内负责 LD/profiler timestamp search、wrapper args
注入、standalone HiCache/cache_io/python_probe 事件附加和稳定排序。不存在独立 merger 进程或 sequential 兼容路径。
`faithful_replay` 和 `cache_state` 看到同一份 manifest source 合同；差异只在 channel selector 和启用的模块。

`--threads` 是 logical input 读取与 DAG build 的总预算。多个 logical input 并行构图时，每个 `DagBuilder` 得到
`max(1, threads / logical_concurrency)` 个内部线程，避免外层并发与单图并发相乘。`--file-threads` 独立控制单文件 scanner；
workflow 分别通过 `--trace-threads` 和 `--trace-file-threads` 设置这两个值。

Chrome trace reader 只立即解析顶层 event 字段，`args` 保留原始 JSON，并在模型查询具体 key 时懒加载。需要遍历全部
args 的模块必须显式走 materialized view，不能在 reader 阶段全量展开。

非执行类 HiCache 事件需要通过 `args.fact` 路由隔离：

| 字段 | 作用 |
| --- | --- |
| `fact.class` | 子模块事实分类。 |
| `fact.role` | class 内事实角色，供状态子模块二级路由。 |
| `fact.consumers` | 允许消费该事实的模型、质量审计或 validator 列表。 |

### Formal trace window 与 semantic fact

建模 runner 优先从 workload report 的 `formal_begin_ms` / `formal_end_ms` 构造窗口；旧 report 没有这对字段时才使用请求
包络。窗口起止必须同时存在，runner 将其换算为 `trace_window_start_us` / `trace_window_end_us` 写入 runner config，backend
再传给 C++ 的 `--trace-window-start-us` / `--trace-window-end-us`。C++ 只接受成对边界。

正式窗口只决定 active DAG 的可执行事件。窗口前的 token dictionary 定义和窗口后、与窗口内 async operation identity 严格匹配的
ACK / capacity release 分别保存为只读 context side table：前者只帮助解析窗口内 path，后者只证明 lifecycle closure。两者都
不创建 DAG node、不贡献 duration/E2E，且不能携带 target state 答案。

`args.fact` 标记的非执行 HiCache 事件同样只进入 `DagGraph::hicache_fact_events`；state replay、source index 和 attribution
从 fact side table 读取它们，但 active CPU lane 不会为这些事实建立可执行节点或顺序边。patch 只能把 semantic fact 映射到已证明的
真实 source execution anchor；缺少 anchor 或真实 consumer/wait boundary 时必须归类为 `unobservable` 或 `reject`，不得把
fact node 当作可执行载体补图。

## TraceGraph 结构

C++ 后端位于 `src/modeling/trace_graph`：

| 目录 | 内容 |
| --- | --- |
| `include/markov/trace_graph/core` / `src/core` | `TraceEvent`、`DagGraph`、`DagBuilder` 和日志等基础结构。 |
| `include/markov/trace_graph/frontend` / `src/frontend` | 窄 model config 和 trace normalize。 |
| `include/markov/trace_graph/io` / `src/io` | Chrome trace 读取输出 adapter。 |
| `src/cli` | 私有 CLI options、module pipeline、workflow、run summary 与 Debug output adapter；`main.cpp` 只做进程 glue。 |
| `include/markov/trace_graph/simulation` / `src/simulation` | 拓扑仿真。 |
| `include/markov/trace_graph/modules` / `src/modules` | `SimulationModule`、业务模块、diagnostics 和 validation。 |
| `modules/hicache/model` | HiCache target-derived 状态机。 |
| `modules/hicache/runtime` | async operation、capacity、ref ledger、token directory 和 target-control clock。 |
| `modules/hicache/radix` | canonical token radix tree 和 split policy。 |
| `modules/hicache/storage` | storage directory 和 backend-readable 投影。 |
| `modules/hicache/patch` | source DAG index、effect attribution、I/O resource、rewrite transaction、boundary 和 materialization validation。 |
| `modules/hicache/diagnostics` | HiCache summary JSON 序列化和诊断输出。 |
| `modules/dag_analysis` | Debug-only DAG faithful replay、结构画像、anchor coverage 和 operation visibility artifact。 |

构建目标：

```bash
scripts/run.sh modeling -- bash -lc 'cmake --build build/modeling/trace_graph-release --target trace_graph -j2'
```

旧 C++ TraceGraph 对照目录已移除，不参与 active build，也不提供旧 include path 或 namespace 兼容层。
active public include 根固定为 `include/markov/trace_graph/...`，命名空间固定为
`markov::trace_graph::...`。

构建 target 按职责分层：

| target | 职责 |
| --- | --- |
| `trace_graph_core` | DAG、trace event、builder 和 logger。 |
| `trace_graph_frontend` | config 解析和 trace normalize。 |
| `trace_graph_io` | Chrome trace I/O。 |
| `trace_graph_simulation` | 拓扑仿真。 |
| `trace_graph_hicache` | HiCache fact、policy、runtime、radix、storage 和 state model。 |
| `trace_graph_modules` | 业务 `SimulationModule` 包装层。 |
| `trace_graph_cli_support` | 文件输出和 CLI Debug/Release 链接边界；Release 不编译 Debug output adapter，也不链接 diagnostics。 |
| `trace_graph_diagnostics` | Debug-only module summary、HiCache summary JSON、DAG analysis 和调试输出 adapter。 |

窄 C++ model config 只用 `node_scale` / `hicache` 对象自身的 `enabled` 字段表达启用状态，不再同时维护 `modules[]` 注册表。
业务层不得依赖 diagnostics / validation target；diagnostics / validation 可以消费业务层暴露的结构化结果。Release 构建不链接
diagnostics / validation，`--model-summary` 和 `--dag-analysis-output-dir` 会明确要求 `TRACE_GRAPH_DEBUG=ON`。调试和验证裁剪只使用单一
`DEBUG` 宏，由 CMake 的 `TRACE_GRAPH_DEBUG` 或 Debug build 控制，宏不应散落在状态机主体中。

## SimulationModule 接口

所有 what-if 都必须规约为 C++ `SimulationModule`。Python 侧只做配置生成、运行编排和 validation。

子模块职责：

- 读取 normalized DAG / trace event；
- 解析自身事实；
- 维护内部状态；
- 必要时通过统一 mutation API 修改 DAG；
- 输出结构化 summary/debug 数据；JSON summary 由 diagnostics writer 生成。

当前 active 子模块：

| 模块 | 状态 |
| --- | --- |
| `NodeScaleModule` | smoke / 节点耗时缩放。 |
| `HiCacheModule` | 执行 state replay 并导出 stable-keyed effect decision ledger；自身不修改 DAG。 |
| `HiCacheDagPatchModule` | 只读消费 state result 和 source full DAG，完成 attribution、resource planning、rewrite/boundary gate，并通过统一 mutation API 原子应用完整 cell transaction。 |

## HiCache 状态后端

`HiCacheModule` 只消费四类 canonical state-model facts 和显式 target config，维护 target cache state，并输出 Release 可用的 effect decision ledger；
Debug/validation build 额外输出 final state、transition trace、policy decision trace 等结构化 summary，供 Python oracle validation 使用。
`HiCacheDagPatchModule` 通过只读共享 result 接在 state replay 后。启用 DAG patch 时，它从 source full DAG 建立一次语义索引，
逐 effect 证明 source carrier、consumer 和 ownership，生成完整 shadow transaction，通过 prospective topology 与 boundary gate 后
一次 apply。任一 supported effect unresolved、ownership conflict、未校准 non-empty cost 或拓扑失败都会阻止整个 cell 的 production mutation。

主链路：

```text
HiCacheFact
  -> HiCacheFactRouter
  -> HiCacheTokenDirectory / role-specific token resolver
  -> HiCacheTargetPager
  -> scoped canonical HiCacheTokenRadixTree
  -> StorageDirectory / RefLedger / CapacityIndex / AsyncOperationTable / TargetControlClock
  -> HiCachePolicy
  -> DerivedStateView / HiCacheEffectDecisionLedger
  -> SourceDagIndex / SourceAttribution / IoResourcePlan
  -> ShadowRewriteTransaction / BoundaryValidation
  -> one atomic DagMutationPlan / AppliedValidation
  -> topological simulation
  -> Debug diagnostics summary / Python validation
```

Release 业务结果包含 replay 完成状态、每个 stable opportunity 的显式 target state、schedule sensitivity、compact attribution/rewrite
计数、mutation journal 和 active graph 计数；policy、transition、capacity、ref 和 per-effect 证据行只存在于 Debug/validation build。
effect decision 记录 request/scope、方向、candidate/effective segment、byte 数、target boundary、target effect state 和 patchability，
不读取 target actual timing、E2E 或 validation answer。`not_required` 必须由 opportunity 上的显式 decision 表达，不能由 operation
缺席推断。Prefill 不属于 direct I/O/control v1，ledger 与 patch result 固定报告 `prefill_effect_status=deferred`。

Patch 不回写 state result。Source attribution 使用完整 source DAG，但只把具有强 identity、明确 ingress/egress/consumer 且 ownership
唯一的 atom 交给 rewrite。`DagMutationPlan` 支持 duration replacement、node/edge tombstone、独立 synthetic node、add/redirect edge、
prospective topology validation 和 mutation journal；post-apply validator核对 plan/journal/materialization、family dependency、resource lane
和 active topology。Simulator、DAG writer、run summary 与 DAG diagnostics 始终读取同一个 materialized active graph。

### HiCache I/O 资源合同

Workflow 只提供一个可选的业务模型输入：

```text
--hicache-io-model <explicit_io_model.json>
```

文件 schema 固定为 `markov.hicache.io_model.v1`，必须显式包含：

```json
{
  "schema": "markov.hicache.io_model.v1",
  "model_id": "model identity",
  "calibration_status": "calibrated",
  "kv_bytes_per_token_per_rank": 1024,
  "device_host_bandwidth_bytes_per_sec": 1000000000,
  "host_storage_bandwidth_bytes_per_sec": 500000000,
  "provenance": {
    "kv_geometry": "explicit model metadata",
    "device_host_bandwidth": "external measurement",
    "host_storage_bandwidth": "external measurement"
  }
}
```

示例数值只用于展示 schema 和单位，不是默认值或标定值。其中只有 device-host 与 host-storage bandwidth 是 cost 参数；
`kv_bytes_per_token_per_rank` 是模型几何。Workflow 使用 target
`page_size` 计算 `kv_bytes_per_page`，并把 canonical JSON digest、模型身份、provenance、page bytes 与两个 bandwidth 写入每个
cache-state runner config 和 C++ narrow config。一个 workflow invocation 只能使用同一份 I/O model；target profile config
不能按 cell 覆盖 `kv_bytes_per_page` 或 `io_cost`。未提供模型时仍可执行 state/transition workflow，但 resource plan 必须输出
missing byte projection blocker 和两个 bandwidth parameter-presence 字段，不能使用默认值。

`calibration_status=contract_only` 只允许构造 decision、cost、attribution 和 shadow transaction，不允许 non-empty production apply；
`calibration_status=calibrated` 必须来自独立 measurement provenance，才允许带 synthetic I/O 的 transaction materialize。Target workload
trace、target observed duration 和 E2E 不得用于反推 bandwidth。

I/O duration 只按下式计算：

```text
duration_us = ceil(effective_byte_count * 1_000_000 / bandwidth_bytes_per_sec)
```

resource model 在每个 logical scope 内维护 `host_storage_lane`、`device_to_host_lane` 和 `host_to_device_lane`。同 scope 同 lane 的 operation 按
target logical enqueue boundary 与稳定 effect id 排序，输出相邻 dependency；不同 lane 不增加依赖。capacity dependency gate
不占 I/O lane 且 duration 为零。Resource planning 本身只生成 read-only plan；synthetic node 和依赖只由完整 rewrite transaction 创建。

### 输入边界

后端输入分流规则：

```text
consume fact iff "hicache_state_model" in fact.consumers
    && fact.class/fact.role is accepted by HiCacheFactRouter
    && phase matches the role contract
```

其它 HiCache 事件计入 `skipped_non_state_model_events`，不能更新 target state。token dictionary 也只从 completed
state-model path fact 水合；`source_actual`、`timing_observation`、`oracle_state` 和
debug/provenance 字段只能用于质量审计、validation label 或 transition 归因，不能回写为 target state mutation。

当前正常 state model fact：

| fact | 语义 |
| --- | --- |
| `workload_identity/cache_lookup_input` | `match_prefix` cache lookup key；用于按 target radix lookup / touch 和 opportunistic host-visible loadback。 |
| `workload_identity/cache_extend_input` | `prepare_for_extend` start-phase batch 输入；模型按 batch accepted fill path 统一计算 extend allocation pressure 和 request refs。 |
| `workload_identity/cache_lifecycle_commit` | finished/unfinished lifecycle commit；fact 必须显式携带当前 committed/fill path，模型基于该 path 插入 radix 并释放 request KV lifecycle。 |
| `workload_identity/prefetch_candidate_anchor` | scheduler prefetch candidate path；模型按 target policy 重新判断 planned pages、storage hit prefix、host reservation 和 anchor ref。 |

match-prefix concrete path、lookup result、source insert/capacity/lock/maintenance、storage/controller result 和 async completion
只作为 `source_actual` / `timing_observation` evidence。`drain_storage_control_queues()` 不声明 profiling checkpoint；
host/storage release 的 target 时机由模型内 target-derived 近似负责。unknown state-model fact 必须进入 quality / summary
error，不能静默消费。

cross-config rule diagnosis 必须先通过 hard workload identity contract：只比较 `workload_identity` facts，逐 role 对比 count
和 request-normalized canonical fact multiset。raw `request_id` 是 run-local correlation id，不是跨配置 workload identity。

### 组件边界

当前 C++ HiCache backend 文件：

| 文件 | 作用 |
| --- | --- |
| `fact.hpp/.cpp` | 识别 HiCache event、收集 token dictionary、解析 span 和事实字段。 |
| `router.hpp/.cpp` | role enum、输入门禁和 required field 检查。 |
| `runtime/token_store.hpp/.cpp` | `HiCacheTokenDirectory`、event-local path snapshot、request timeline 和 role-specific resolver；不保存 source page identity 作为状态输入。 |
| `runtime/target_pager.hpp/.cpp` | 按 target page size 投影完整 page hash、page id 和 page path。 |
| `radix/token_radix_tree.hpp/.cpp` | 每个 `cache_scope` 一棵 canonical compressed radix tree；device/host/storage/ref 都是 node residency/ref 字段，不再维护 device tree 与 host tree 两套事实源。 |
| `storage/storage_directory.hpp/.cpp` | target storage namespace；区分 materialized page record 与 backend-readable hash record，支持连续 storage hit prefix 查询。 |
| `runtime/ref_ledger.hpp/.cpp` | request / writeback / loadback / storage / prefetch owner 级 ref 账本，负责同步 tree 上的 lock ref 和 host ref。 |
| `runtime/capacity_index.hpp/.cpp` | mutation-driven device/host leaf index、capacity snapshot、victim choice 和 audit trace。 |
| `runtime/async_state.hpp/.cpp` | prefetch、writeback、loadback、storage operation table 和 lifecycle transition。 |
| `runtime/target_control_clock.hpp/.cpp` | target-side control boundary 与内部 operation id，避免把 source timestamp 当作 target 调度事实。 |
| `radix/node_split_policy.hpp/.cpp` | radix split 时 residency/ref/hit count/page projection 的结构化迁移策略。 |
| `policy.hpp/.cpp` | 显式 target config 解析、SGLang-derived default 和 policy decision trace。 |
| `runtime/state_index.hpp/.cpp` | `DerivedStateView`，从 tree / storage / async 派生 validation-facing state sets。 |
| `model/state.hpp/.cpp` | `HiCacheState` 聚合状态、scope 管理、digest、公共 apply/finalize 入口。 |
| `model/request_model.cpp` | cache lookup、batch cache extend、lifecycle commit 和 insert/loadback 相关 transition。 |
| `model/prefetch_model.cpp` | prefetch candidate、cache-extend terminal boundary、ready/apply/cancel 和 host reservation。 |
| `model/host_storage_model.cpp` | host cleanup、host allocation 和 capacity eviction。 |
| `model/writeback_model.cpp` | write-through / write-back、backup ACK、dirty clear 和 ref hold/release。 |
| `model/finalizer.cpp` | finalize 时 pending operation 收束。 |
| `model/effect_decision.hpp/.cpp` | 为每个 stable opportunity 导出显式 target decision、segment、schedule sensitivity、byte projection 和合同缺口。 |
| `patch/io_resource_model.hpp/.cpp` | 只用 effective bytes 与两个显式 bandwidth 生成 duration、三条 lane 和稳定 lane dependency；不读取 DAG 或 target timing。 |
| `patch/source_dag_index.hpp/.cpp` | 一次扫描 active source DAG，索引 fact、request、operation、邻接和 input contract。 |
| `patch/attribution.hpp/.cpp` | 逐 effect 证明 source presence、carrier、owned duration、boundary 和 consumer。 |
| `patch/rewrite_transaction.hpp/.cpp` | 分类 insert/remove/replace/no-op/partial，解决 ownership，并构造完整 prospective mutation plan。 |
| `patch/boundary_validator.hpp/.cpp` | apply 前检查 source cost、target cost、ingress/egress 和 consumer dependency。 |
| `patch/applied_validator.hpp/.cpp` | apply 后核对 journal、materialization、family/lane dependency 和 topology。 |
| `model/result.hpp` | state replay 的 Release result 与 Debug summary 边界。 |
| `model/summary.hpp` | HiCache state model 的 Debug/validation 结构化执行结果；Release 下为空标记类型，不包含 JSON。 |
| `diagnostics/summary.hpp/.cpp` | HiCache summary JSON 序列化；不参与状态机决策。 |
| `hicache_module.hpp/.cpp` | state replay 的 `SimulationModule` glue；Release 暴露 effect decisions，Debug 额外持有 summary。 |
| `dag_patch_module.hpp/.cpp` | 集中编排 resource、index、attribution、rewrite、validation 和一次原子 mutation apply。 |
| `core/dag_mutation.hpp/.cpp` | 通用 mutation plan、journal、tombstone、redirect、去重和 topology validation。 |

### 目标 Page 投影

后端不消费 `page_identity` / `target_page_identity_page<page_size>` 作为主输入。page 由 token path 重建：

```text
for each full target page:
  page_hash = sha256(parent_hash_bytes + token_u32le...)
  page_id = cache_scope + "|" + page_hash
```

规则：

- target `page_size` 优先来自 modeling config；
- 没有 target page size 时才回落 source page size；
- 只生成完整 page，不生成 tail page；
- `cache_scope` 参与内部 page id，validation 可用 `oracle_page_key_mode=strip_scope` 与 raw oracle hash 对齐；
- page 级集合只能从 canonical node、operation lifecycle 和 storage directory 派生，不能作为独立事实源。

### 目标状态

summary 输出当前 validation 使用的集合，但集合来源必须是 canonical tree / storage / async projection：

| 集合 | 来源 |
| --- | --- |
| `l1_resident_pages` | node device residency projection。 |
| `l2_resident_pages` | `host.present && host.visible` projection。 |
| `l3_resident_pages` | storage-readable projection；是否包含 backend-only readable hash 由 derived view mode 明确选择。 |
| `dirty_pages` | node dirty projection。 |
| `backuped_pages` | host copy projection；不把 storage readable 直接当成 backuped。 |
| `evicted_pages` | target eviction lifecycle projection。 |
| `locked_pages` | lock ref 非零的 node/page projection。 |
| `pending_writeback_pages` | async operation table 中尚未完成的 writeback projection。 |
| `prefetch_planned_pages` | prefetch operation planned path projection。 |
| `prefetch_ready_pages` | modeled async queue 已 ready 但可能尚未全部 host-visible 的 page projection。 |
| `prefetch_late_pages` | target policy 判定 timeout/late 的 page projection。 |
| `prefetch_suppressed_pages` | storage miss / revoke / finalization / timeout 下被 target policy 放弃的 page projection。 |
| `page_hit_counts` | policy-visible page hit count projection，仅作诊断 metadata。 |

### 策略与资源语义

request / allocator：

- `cache_extend_input` 使用 batch accepted fill path 构造 `CacheExtendBatchIntent`，统一计算 batch-level extend pressure；
- eviction gate 对齐 SGLang allocator：用 `DeviceAllocatorLedger.available_pages()` 判断是否需要 eviction，不从 radix occupancy 反推；
- eviction budget 使用完整 allocation request；实际 active request reservation 使用本次真正分配/占用的 page；
- `cache_lifecycle_commit` 在 finished / unfinished 上插入 committed path，并释放 duplicate / tail / overallocated KV 到 allocator ledger。

token directory：

- `HiCacheTokenDirectory` 保存 event-local token path snapshot 和 request timeline；`request_id` 只表示请求身份，不表示静态 token path；
- path 消费必须走 role-specific resolver：lookup/extend/lifecycle/prefetch 分别只消费对应 fact-local path；
- lifecycle 缺少 committed path 时必须记录 missing 诊断并跳过 mutation，不能静默复用 extend path；
- `prefetch_candidate_anchor` path 只作为 prefetch candidate，不更新 request committed timeline；
- directory 只接收 completed state-model path fact；diagnostic/source path 不能为 state model 水合 token；
- lifecycle resolver 可以读取 earlier committed snapshot 做 duplicate/tail 计算，但本次 lifecycle mutation 的目标 path 必须来自当前 fact。

host / storage / prefetch：

- host cleanup victim 是 host radix leaf，必须 host-visible、evicted、无 lock/host ref protection，且没有 host-present backup child；
- host cleanup budget 来自本次 allocation request：prefetch 对齐 `evict_host(prefetch_length)`，write backup 对齐
  `evict_host(len(node.value))`；
- storage hit query 只保留连续命中前缀；storage-readable 不等于 host-visible；
- prefetch operation 保存 planned path、hit prefix、requested host pages、reserved host pages 和 anchor ref；
- prefetch policy 共用一个 boundary solver：best-effort固定source cache-extend，wait-complete取I/O completion并仅在更晚时加gate，
  timeout取I/O completion与configured deadline的较早者；只有boundary前完成的完整页进入host radix和PrefetchIo effect；
- revoke / timeout incomplete 的 host reservation 进入 pending release 近似，不立即从 host budget 中消失；同 request 的
  `cache_extend_input` side effect 完成后做 request-local release drain。

write policy：

- `write_through`、`write_through_selective` 和 `write_back` 共享 device insert、host backup、storage readable、capacity cleanup helper；
- `write_through_selective` 的 hit-count threshold 由 target policy 决定；
- write-through backup ACK 前会持有普通 lock ref；当前按 target control fact 近似 drain，并在 finalize 收敛尾部
  pending ACK，真实 async ACK / rank 同步时序仍记录为 validation 限制；
- write-back ACK 时序当前折叠为同步 completion，结果语义统一落到 host backup / storage readable / dirty clear；
- source writeback ACK、storage hit result、node remove result 和 async wall-clock completion 不能作为 state model input。

### 摘要

summary 输出位置：

```text
model_summary.json.modules[0].hicache
```

关键字段：

| 字段 | 说明 |
| --- | --- |
| `input_hicache_events` | 识别到的 HiCache events。 |
| `processed_hicache_events` | 实际消费的 state-model end events。 |
| `skipped_non_state_model_events` | 跳过的 source_actual / timing / oracle / debug events。 |
| `processed_events_by_role` | 各 role 消费计数。 |
| `missing_state_model_facts` | 缺失或未知 state-model 输入。 |
| `token_path_diagnostics` | role-specific path resolution、lifecycle missing/stale、timeline 和 direct-fact 使用情况。 |
| `final_state` | `DerivedStateView` 派生的模型最终 state sets 和 counts。 |
| `storage_directory_inclusive_state` | 包含 backend-readable hash 的 storage-inclusive projection。 |
| `transition_trace` | request / operation / page 级模型状态转移。 |
| `async_lifecycle_trace` | prefetch / writeback / loadback / storage operation lifecycle。 |
| `policy_decision_trace` | policy、allocator、capacity、loadback 和 cleanup 决策账本。 |
| `capacity_mutation_trace` / `capacity_victim_choices` | capacity index 增量更新和 victim 选择证据。 |
| `ref_mutation_trace` / `ref_audit` | owner 级 ref acquire/release 和 tree ref 一致性审计。 |

### 状态到 DAG / E2E 路线

当前完成对象是 HiCache direct I/O/control DAG patch v1。它不只检查 final state，还把 source DAG 中已有的 HiCache
载体与 target decision 做显式 diff，并在同一 active graph 上完成删除、插入、替换、依赖重连和拓扑仿真：

```text
target semantic chain:
  four canonical workload facts + target config + calibrated I/O model
    -> target state
    -> stable opportunities
    -> complete target effect decisions

source physical chain:
  torch / LD_PRELOAD / timing evidence
    -> one-pass source DAG index
    -> source presence/carrier/boundary/consumer attribution

DAG rewrite chain:
  source attribution + target decisions
    -> no-op / insert / remove / replace / partial-replace decision
    -> one cell-level mutation transaction
    -> prospective topology and boundary validation
    -> one atomic apply
    -> post-apply materialization validation
    -> simulated target DAG
```

职责和验收边界：

| 阶段 | 输入 | 输出 | 通过口径 |
| --- | --- | --- | --- |
| target state | 四类 state-model facts + target config | final state、transition、effect decisions | raw exactness不被改写；已知 async readiness limitation必须有final-DAG证据并独立分栏，unrelated/not-ready必须为0。Source actual与target oracle不进入replay。 |
| target cost | effective bytes + 两个 bandwidth | duration 与 scope-local lane | 公式可追溯；没有经验常数或 target observed duration。 |
| source attribution | source full DAG + source actual/timing probes | source carrier、owned cost、boundary、consumer | 每个 atom ownership 唯一；unobservable/ambiguous 显式阻断。 |
| source/target diff | attribution + target decisions | rewrite transaction | supported opportunity 全覆盖；不存在用 operation absence 代替 `not_required`。 |
| DAG patch | transaction + source graph | materialized active graph | cell-level atomic；boundary、journal、family/lane dependency 与 topology 一致。 |
| patch-local validation | target actual validation probes + prediction artifact | self/cross shape comparison | schedule-invariant exact；arrival-sensitive row 必须有 target-self alternate evidence。 |
| simulation | materialized active graph | simulated E2E / critical path | 只作预测输出；raw cross-config real E2E 不参与 v1 pass/fail。 |

H2D loadback carrier 必须限制在当前 decision opportunity window 内查找；只按 PID + radix node ID 扫描整条 trace 会在跨请求复用
node ID 时把历史调用误判为多个当前 carrier。一个 opportunity 内没有 carrier 仍为 `unobservable`，出现多个真实 carrier 仍为
`ambiguous`，不能通过放宽 safety gate 绕过。

2026-07-13 的 75-cell `75/75 applied` 结果属于旧 source-DAG 语义下的历史 artifact，不能作为当前二进制的 patch acceptance
声明。当前主线先以 2026-07-29 的同输入 15-cell `faithful_replay` 验证 Base-DAG：全部 cell 的相对误差均不超过 5%。当前
HiCache patch 仍必须逐 cell 满足真实 execution anchor 与 consumer/wait anchor；最新安全阻断结果不能被表述成已 applied prediction。
可复现实验路径、数值和适用边界维护在 `docs/validation/hicache_state_validation.md`。

`transition` 解释 state 怎么变；effect decision 解释 target 下每个稳定 opportunity 应该发生什么。DAG patch 消费 decision ledger，
不能从 raw page-level transition、final page set 或 target actual operation 反推 rewrite。当前 supported effect 为 loadback、prefetch
I/O、prefetch visibility、commit D2H、commit H2S 和 commit capacity gate；prefill 明确 deferred。

## Backend 选择

业务 modeling 默认使用 Release backend：

- 默认可执行文件是 `build/modeling/trace_graph-release/trace_graph`；
- 不链接 diagnostics / validation；
- 不支持 `--model-summary`；
- 不执行 module summary writer、C++ validation runner 或 HiCache debug summary JSON adapter；
- 不保存 HiCache transition/policy/ref/capacity/radix/async 行级 debug history。

需要 validation/debug artifact 的 unified modeling workflow 必须使用 validation backend：

- workflow 生成的 runner config 写入 `cpp_trace_graph.backend_kind="validation"`；
- runner 只查找 `build/modeling/trace_graph-validation/trace_graph`，不回退到 Release 或 Debug；
- validation backend 使用优化构建，例如 `Release + TRACE_GRAPH_DEBUG=ON`，保留 diagnostics / validation 代码但避免
  `-O0` Debug 二进制处理大 trace；
- 缺少 validation backend 时直接失败，并提示 validation build 命令；
- `outputs.emit_validation=true` 代表执行 validation 路径，而不是只多写一个输出文件。
- `outputs.emit_dag_analysis=true` 同样要求 validation backend，并执行 DAG analysis artifact 构造路径。

## DAG Analysis Validation

`base_dag` / `hicache_dag_mapping` 是 profiling 后的 per-run validation object，用来理解真实 DAG，而不是执行 cache patch。
它的粒度是真实 `input_id / config_id / profile_manifest`，不是 source-to-target prediction cell。两者共享同一批
`mode=faithful_replay` model run：`base_dag` 解释 faithful replay / DAG quality，`hicache_dag_mapping` 解释 HiCache
anchor coverage / operation visibility。

运行入口示例：

```bash
python3 scripts/internal/entrypoints/modeling_workflow.py \
  --profile-run-dir <suite> \
  --output-dir <suite>/modeling/modeling_workflow_dag \
  --validations base_dag,hicache_dag_mapping \
  --inputs <input_id> \
  --configs <config_id>
```

阶段产物：

```text
workflow_summary.json
preflight_summary.json
artifacts/preflight/full_dag_trace_channels/<input>/<config>/profile_artifact_audit.json
artifacts/model_runs_summary.json
artifacts/validations/base_dag/<model_run_id>.json
artifacts/validations/base_dag/summary.json
artifacts/validations/hicache_dag_mapping/<model_run_id>.json
artifacts/validations/hicache_dag_mapping/summary.json
model_runs/<model_run_id>/dag_quality.json
model_runs/<model_run_id>/dag_analysis.json
model_runs/<model_run_id>/dag_anchor_coverage.json
model_runs/<model_run_id>/dag_operation_visibility.json
```

`dag_quality.json` 记录 faithful replay sanity 与 DAG build 计数；输入 channel coverage 只在 preflight 的
`profile_artifact_audit.json` 中维护，不再由 Python 回写 C++ artifact。`dag_analysis.json` 记录 node / edge / lane /
critical path 摘要；`dag_anchor_coverage.json` 只审计当前 workload identity facts 的 DAG anchor；`dag_operation_visibility.json`
把候选 HiCache operation 标记为 visible / partially visible / invisible。这里的 `patchable_candidate` 只表示阶段二可继续评估，
不代表当前业务路径已经支持 patch。

`base_dag.ready`只表示full-DAG输入和结构artifact可用于后续建模：C++命令成功、`dag_quality.json` / `dag_analysis.json`
存在、node/edge非空且DAG analysis没有结构blocker。`faithful_replay.ready`、relative error和
`dag_replay_error_too_high`继续原样保留在row与summary中，但属于raw E2E diagnostic，不参与`base_dag.ready`。原因是当前
closed-loop benchmark没有固定跨配置request arrival timeline，scheduler idle/polling差异不能作为HiCache patch或base-DAG结构
正确性的hard gate。

后续 scheduler、storage、runtime 或 communication 子模块不应新增独立 DAG workflow，而应在 unified modeling workflow
下新增 validation object，并声明自己需要的 C++ output requirement。

## 验证

validation 不是默认输出。用户只选择 workflow validation object；runner adapter 由 semantic output requirement 自动生成
`outputs.emit_validation=true` 或 `outputs.emit_dag_analysis=true`，并选择 validation backend。低层 `model.py` 不暴露 emit CLI。

HiCache state validation 必须同时看：

- `validation_ready`；
- `validation_errors`；
- `hicache_state.state_model_fact_ready`；
- `hicache_state.missing_state_model_facts`；
- `hicache_state.final_state_match` / `raw_final_state_match`；
- normalized `sets_diff_by_tier`。

只要 `state_model_fact_ready` 为 false，即使 final state 偶然对齐，也不能宣称 state-model prediction 通过。
当前有效验证口径、结果和剩余风险维护在 `docs/validation/hicache_state_validation.md`。

## 未覆盖设计范围

下列内容不应在 development 文档中用实验结论替代设计：

- HiCache 命中变化造成的 prefill token 数和 LLM 计算图变化；
- async prefetch exact progress / partial completion 的完整 target model；
- SGLang `TreeNode.host_ref_counter`、host protection lifetime 和复杂 radix split/delete victim tie-break 的完整等价；
- write-back ack、background flush 和 `_evict_backuped()` / `writing_check(write_back=True)` 的真实异步批处理时序；
- scope-normalized comparison 之外的多 scope page identity 验证。
- host-wide/device-wide 多 request 竞争和跨平台 CPU/NPU operation cost；
- 修复 closed-loop benchmark 后的固定 arrival schedule raw E2E exactness。

这些风险的当前验证状态维护在 `docs/validation/hicache_state_validation.md`；中长期缺口和阶段性妥协维护在
`docs/validation/hicache_state_model_limitations.md`。本文件不重复实验分析。
