# 建模开发文档

维护方式：这是 modeling 主线设计文档。更新时直接删改本文件内容，不写流水账、实验结果或阶段分析。
真实 run、验证结果和历史结论维护在 `docs/work_progress.md` 或 `docs/validation/`。

## 目标

Modeling 基于 profiling 事实构建 C++ TraceGraph，并在目标配置下通过子模块维护状态或修改 DAG。

默认流程：

```text
profile manifest / explicit trace
  -> scripts/trace/trace_merger.py
  -> C++ TraceGraph
  -> optional SimulationModule
  -> topological simulation
  -> prediction.json / optional summary / optional validation
```

默认主输出：

```json
{
  "predicted_e2e_ns": 0
}
```

`predicted_e2e_ns` 来自 DAG 拓扑仿真。对于 HiCache state-only backend，它不是 cache state 正确性的验收指标。

## 运行入口

Modeling 后端是 C++23 TraceGraph，构建和运行基线是独立的 `modeling` Docker service。该 service 基于干净
Ubuntu 24.04，只提供 C++23、CMake、Ninja、clang-format/clang-tidy、Python 标准运行环境和 modeling 脚本依赖；
不挂载 Ascend 设备，不依赖 CANN，不安装 SGLang / KTransformers runtime。宿主机只负责启动外层 wrapper 或执行无
modeling runtime 依赖的文本检查；不再支持直接在宿主机用 `scripts/internal/entrypoints/model.py` 执行 modeling run、host build
`trace_graph` 或旧 `build/bin` 产物。

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

不维护 fixture-backed smoke modeling 入口。Modeling 验证必须基于真实 profile manifest、显式 trace，或专项验证文档中记录的
可复现 profile/modeling run。

当前也不维护静态 `configs/modeling/` 文件。cache-state 主流程由 `scripts/internal/entrypoints/hicache_workflow.py` 从 profile suite 的
target server metadata 动态生成 Python runner config，写入 `<workflow_output>/artifacts/runner_configs/`；每个 prediction
输出目录下的 `cpp_model_config.json` 是 C++ TraceGraph backend narrow config。

faithful replay：

```bash
scripts/model.sh \
  --config <modeling-config.json> \
  --profile-manifest <run_dir>/profile_manifest.json \
  --output-dir <run_dir>/modeling/faithful_replay \
  --mode faithful_replay \
  --emit-validation
```

HiCache state validation prediction 通常由 workflow 生成 runner config 后调用：

```bash
scripts/model.sh \
  --config <workflow_output>/artifacts/runner_configs/target_<config_id>.json \
  --profile-manifest <run_dir>/profile_manifest.json \
  --output-dir <run_dir>/modeling/cache_state \
  --mode cache_state
```

该 runner config 必须包含 `cpp_trace_graph.backend_kind="validation"` 和
`outputs.emit_validation=true`。普通 business cache-state prediction 不生成 `model_summary.json` /
`validation.json`，也不使用 Debug backend。

常用覆盖项：

| CLI | 作用 |
| --- | --- |
| `--profile-manifest` | 覆盖 config 中的 manifest。 |
| `--output-dir` | 覆盖输出目录。 |
| `--mode` | 覆盖 modeling mode。 |
| `--emit-dag-chrome-trace` | 输出 DAG Chrome trace。 |
| `--emit-module-summary` | 输出 `model_summary.json`。 |
| `--emit-validation` | 输出 `validation.json`。 |

## 脚本分层

Modeling 相关脚本同样按 wrapper、entrypoint 和可复用包分层：

| 层级 | 路径 | 职责 |
| --- | --- | --- |
| 宿主机 wrapper | `scripts/model.sh` | 启动 modeling 容器并转发 CLI。 |
| 容器内入口 | `scripts/internal/entrypoints/model.py` | 解析 modeling CLI，调用 `markov_internal.modeling.runner`。 |
| C++ 输入准备 | `scripts/internal/markov_internal/modeling/trace_inputs.py` | 处理 profile manifest / raw trace 输入、调用 trace merger、检查 merge summary。 |
| C++ config / binary | `scripts/internal/markov_internal/modeling/cpp_config.py` | 生成窄 C++ model config、解析 target HiCache 参数，并按 backend 定位 Release 或 Debug/validation binary。 |
| validation payload | `scripts/internal/markov_internal/modeling/validation.py` | 组装 validation 输出、HiCache state validation 和 recommended config audit。 |
| workload helper | `scripts/internal/markov_internal/modeling/workload.py` | 读取 workload report / bench JSONL 中的实际运行窗口。 |

HiCache workflow 是 profiling 后的 validation 编排，不属于 C++ 后端主体：

| 路径 | 职责 |
| --- | --- |
| `scripts/internal/entrypoints/hicache_workflow.py` | 统一入口，解析 workflow CLI。 |
| `scripts/internal/markov_internal/hicache/workflow/core/` | workflow context 与 artifact policy。 |
| `scripts/internal/markov_internal/hicache/workflow/stages/` | 面向对象 stage runner、final-state stage 和 transition stage。 |
| `scripts/internal/markov_internal/hicache/workflow/output/` | TTY progress 与 stage/workflow summary 输出。 |
| `scripts/internal/markov_internal/hicache/workflow/config/` / `planning/` | workflow runner config 与 matrix plan 写出。 |
| `scripts/internal/markov_internal/hicache/matrix/runs/` | profile discovery、run metadata 和 prediction spec。 |
| `scripts/internal/markov_internal/hicache/matrix/predictions/` / `reports/` | prediction 输出路径、final-state matrix summary 和 workflow input quality report。 |
| `scripts/internal/entrypoints/hicache_transition.py` / `hicache/transition/` | 只读 transition oracle、model self-check、single/matrix compare、catalog 和 gate 输出。 |
| `scripts/internal/markov_internal/hicache/transition/artifacts/` / `replay/` / `validation/` | transition path/catalog 产物、model replay schema 和 exactness validation。 |
| `scripts/internal/markov_internal/hicache/oracle/snapshot/` / `diff/` / `evidence/` | oracle snapshot 读取、state delta/mismatch 和 capacity/coverage evidence。 |
| `scripts/internal/markov_internal/hicache/core/` | HiCache fact 解析、consumer 路由和 token dictionary/span helper。 |

transition oracle 只消费 `source_actual` / `timing_observation` evidence 和 `oracle_state` snapshot 来做验证标签；
它不生成 state-model fact，也不消费 runtime storage-control checkpoint。C++ state model 的输入仍只由 fact contract 和 router
决定。

HiCache workflow 的 Python 编排以 stage runner 为顶层结构：

```text
WorkflowRunContext
  -> WorkflowArtifactPolicy
  -> WorkflowProgressReporter
  -> workflow.stages.QualityStageRunner
  -> workflow.stages.FinalStateStageRunner
  -> workflow.stages.TransitionStageRunner
```

约束：

- `workflow/cli.py` 只负责解析 CLI、发现 runs、创建 context、按固定顺序调用 stage runner 和写 workflow summary；
- quality、final-state、transition 阶段共享 `WorkflowStageRunner` 生命周期，不在各自矩阵模块中直接打印进度；
- `matrix/quality.py`、`workflow/final_state.py`、`transition/matrix.py` 保留为可测试业务函数或 stage helper，不拥有终端输出策略；
- `WorkflowProgressReporter` 是 workflow 用户可见进度的唯一 owner；TTY 下刷新动态进度行，非 TTY 下只输出阶段 start/done summary；
- 新增阶段必须接入同一套 stage runner / progress / artifact policy，而不是复制一套阶段脚手架。

当前 workflow artifact 布局：

```text
<workflow_output>/
  workflow_summary.json
  stages/
    quality/summary.json
    final_state/self_summary.json
    final_state/cross_summary.json
    transition/summary.json
  artifacts/
    matrix_plan.json
    runner_configs/
    quality/
    transition_catalog/
  predictions/
    <input>/<source>__to__<target>/
```

使用约束：

- `workflow_summary.json` 和 `stages/*/summary.json` 是用户第一入口，只保留阶段级计数和分组摘要，不嵌入 per-run/per-cell rows；
- `artifacts/runner_configs/target_<config>.json` 是 Python runner config，供 `scripts/model.sh --config` 使用；
- `predictions/.../cpp_model_config.json` 是 C++ TraceGraph backend narrow config，供 `trace_graph --model-config` 使用；
- per-run audit、transition catalog、gate、prediction cell 产物都是复现/诊断 artifact，不应和用户第一入口混称；
- 默认 console 输出不逐 run/cell 打印 `result ok ...`；失败时只补充失败数量、少量 sample 和关键 artifact 路径。

HiCache Python helper 的当前职责分组：

| 分组 | 模块 | 职责 |
| --- | --- | --- |
| 共享核心 | `hicache/core/` | fact metadata、consumer 路由和 token dictionary/span helper。 |
| 输入合同 | `hicache/input_contract/` | canonical workload signature 和 cross-input report。 |
| 矩阵 | `hicache/matrix/` | 发现 profile runs、构造 prediction specs、workflow input gate 和 prediction row summary。 |
| oracle | `hicache/oracle/` | final-state oracle、capacity/config audit、predicted records、coverage、delta 和 mismatch provenance helper。 |
| transition | `hicache/transition/` | predicted transition schema/replay/self-check、target oracle、single/matrix compare、catalog、gate 和 taxonomy。 |
| workflow | `hicache/workflow/` | post-profile quality、final-state prediction、transition exactness、artifact policy、progress 和 workflow summary。 |

`hicache/` 根目录不再新增业务模块；新 helper 必须落到上述职责子包，避免重新依赖文件名前缀维持边界。

## 建模模式

| mode | 语义 |
| --- | --- |
| `faithful_replay` | 不加载子模块，不 patch DAG；消费完整真实执行 trace，验证 base DAG。 |
| `cache_state` | 加载状态子模块，维护内部状态，不修改 DAG。 |
| `cache_patch` | 子模块维护状态并通过 mutation API 修改 DAG；HiCache 尚未实现。 |

`replay` 只允许指 `mode=faithful_replay`。启用 HiCacheModule 的场景必须称为 `self-config prediction` 或
`cross-config prediction`。

## Trace 合并

`scripts/trace/trace_merger.py` 在 manifest 模式下合并：

- torch profiler trace；
- LD_PRELOAD trace；
- Python probe sidecar。

Trace merger 不根据 modeling mode 删除真实执行事件。`faithful_replay`、`cache_state` 和 `cache_patch`
应看到同一份 merged trace；差异只在是否加载子模块、是否产生 DAG mutation。

非执行类 HiCache 事件需要通过 `args.fact` 路由隔离：

| 字段 | 作用 |
| --- | --- |
| `fact.class` | 子模块事实分类。 |
| `fact.role` | class 内事实角色，供状态子模块二级路由。 |
| `fact.consumers` | 允许消费该事实的模型、质量审计或 validator 列表。 |

## TraceGraph 结构

C++ 后端位于 `src/modeling/trace_graph`：

| 目录 | 内容 |
| --- | --- |
| `include/markov/trace_graph/core` / `src/core` | `TraceEvent`、`DagGraph`、`DagBuilder` 和日志等基础结构。 |
| `include/markov/trace_graph/frontend` / `src/frontend` | 窄 model config 和 trace normalize。 |
| `include/markov/trace_graph/io` / `src/io` | Chrome trace 读取输出 adapter。 |
| `include/markov/trace_graph/simulation` / `src/simulation` | 拓扑仿真。 |
| `include/markov/trace_graph/modules` / `src/modules` | `SimulationModule`、业务模块、diagnostics 和 validation。 |
| `modules/hicache/model` | HiCache target-derived 状态机。 |
| `modules/hicache/runtime` | async operation、capacity、ref ledger、token directory 和 target-control clock。 |
| `modules/hicache/radix` | canonical token radix tree 和 split policy。 |
| `modules/hicache/storage` | storage directory 和 backend-readable 投影。 |
| `modules/hicache/diagnostics` | HiCache summary JSON 序列化和诊断输出。 |

构建目标：

```bash
scripts/run.sh modeling -- bash -lc 'cmake --build build/modeling/trace_graph --target trace_graph -j2'
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
| `trace_graph_cli_support` | CLI Debug/Release 边界；Release stub 不链接 diagnostics / validation。 |
| `trace_graph_diagnostics` | Debug-only module summary、HiCache summary JSON 和调试输出 adapter。 |
| `trace_graph_validation` | Debug-only C++ 轻量结构 validation；oracle 对比仍由 Python validation pipeline 负责。 |

业务层不得依赖 diagnostics / validation target；diagnostics / validation 可以消费业务层暴露的结构化结果。Release 构建不链接
diagnostics / validation，`--model-summary` 会明确要求 `TRACE_GRAPH_DEBUG=ON`。调试和验证裁剪只使用单一 `DEBUG` 宏，由
CMake 的 `TRACE_GRAPH_DEBUG` 或 Debug build 控制，宏不应散落在状态机主体中。

## SimulationModule 接口

所有 what-if 都必须规约为 C++ `SimulationModule`。Python 侧只做配置、trace merge、validation 编排。

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
| `HiCacheModule` | state-only；维护 cache state，不修改 DAG。 |

## HiCache 状态后端

HiCache backend 当前是 state-only `SimulationModule`：它消费 state-model facts 和显式 target config，维护 target cache state，
输出 final state、transition trace、policy decision trace 等结构化 summary；oracle validation 由 Python pipeline 消费这些输出完成。
它暂不修改 DAG。

主链路：

```text
HiCacheFact
  -> HiCacheFactRouter
  -> HiCacheTokenDirectory / role-specific token resolver
  -> HiCacheTargetPager
  -> scoped canonical HiCacheTokenRadixTree
  -> StorageDirectory / RefLedger / CapacityIndex / AsyncOperationTable / TargetControlClock
  -> HiCachePolicy
  -> DerivedStateView / structured summary
  -> diagnostics summary / Python validation
```

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
| `model/summary.hpp` | HiCache state model 的 Debug/validation 结构化执行结果；Release 下为空标记类型，不包含 JSON。 |
| `diagnostics/summary.hpp/.cpp` | HiCache summary JSON 序列化；不参与状态机决策。 |
| `hicache_module.hpp/.cpp` | `SimulationModule` registry glue；Debug 才持有结构化 summary，Release 只执行 state replay。 |

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
- wait-complete 完成后 apply ready pages，best-effort 在 cache-extend terminal boundary terminate，timeout 在 completed 或 timeout 边界 terminate/late；
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

HiCache 的最终目标不是只让 final state 对齐，而是预测 target config 下的 E2E、关键路径和主要 cache 开销变化。
后续链路按以下边界推进：

```text
target semantic chain:
  state-model probe facts
    -> target state
    -> state transitions
    -> target cache operation intents

source physical chain:
  torch / LD_PRELOAD / timing evidence
    -> source cache physical op groups
    -> source DAG node / edge attribution

DAG rewrite chain:
  source full DAG
    - source-only cache physical ops
    + target-only cache intents
    +/- resize ops that exist in both source and target
    -> predicted target DAG
    -> predicted E2E
```

长期阶段：

| 阶段 | 输入 | 输出 | 通过口径 |
| --- | --- | --- | --- |
| target state | state-model facts + target config + oracle label | final state / transition trace | final state 对齐，`state_model_fact_ready=true`。 |
| target intent | state-model facts + transition / policy / async / ref traces | cache operation intent stream | intent 可追溯到 state transition，source outcome 不混入。 |
| source physical attribution | torch / LD_PRELOAD / timing evidence + workload anchors | source cache-owned node / edge groups | physical op group 归因稳定且不重复占用 DAG node。 |
| cache-neutral baseline | source full DAG + source physical groups | cache-neutral DAG | source cache cost 能拆出去并装回去。 |
| source/target cache diff | source physical groups + target intents | delete / insert / replace / resize decisions | self-config diff 基本 identity，cross diff 可解释。 |
| DAG patch | cache-neutral DAG + target intents | predicted target DAG | 无 dangling edge、无 cycle，blocking intent 位于正确依赖边界。 |
| duration / E2E | source calibration + target intents + target oracle label | predicted E2E / critical path audit | E2E 误差、phase 误差和 cache op contribution 可解释。 |

`transition` 解释 state 怎么变；`intent` 解释 target 下应该有哪些物理 cache 操作。DAG patch 应消费 intent，
不能直接消费 raw page-level transition 或 final page set。

## Backend 选择

业务 modeling 默认使用 Release backend：

- 默认可执行文件是 `build/modeling/trace_graph-release/trace_graph`；
- 不链接 diagnostics / validation；
- 不支持 `--model-summary`；
- 不执行 module summary writer、C++ validation runner 或 HiCache debug summary JSON adapter；
- 不保存 HiCache transition/policy/ref/capacity/radix/async 行级 debug history。

HiCache validation workflow 必须使用 Debug/validation backend：

- workflow 生成的 runner config 写入 `cpp_trace_graph.backend_kind="validation"`；
- runner 只查找 `build/modeling/trace_graph-debug/trace_graph`，不回退到 Release；
- 缺少 Debug backend 时直接失败，并提示 Debug build 命令；
- `outputs.emit_validation=true` 代表执行 validation 路径，而不是只多写一个输出文件。

## 验证

validation 不是默认输出，只有 `--emit-validation` 或 config 中 `outputs.emit_validation=true` 时生成，并且必须使用
Debug/validation backend。

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

- HiCache state-to-DAG patch；
- async prefetch exact progress / partial completion 的完整 target model；
- SGLang `TreeNode.host_ref_counter`、host protection lifetime 和复杂 radix split/delete victim tie-break 的完整等价；
- write-back ack、background flush 和 `_evict_backuped()` / `writing_check(write_back=True)` 的真实异步批处理时序；
- transition exact oracle 和逐步 provenance 验收；
- scope-normalized comparison 之外的多 scope page identity 验证。

这些风险的当前验证状态维护在 `docs/validation/hicache_state_validation.md`；中长期缺口和阶段性妥协维护在
`docs/validation/hicache_state_model_limitations.md`。本文件不重复实验分析。
