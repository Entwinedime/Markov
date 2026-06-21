# Modeling 开发文档

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
modeling runtime 依赖的文本检查；不再支持直接在宿主机运行 `scripts/internal/model_runner.py`、host build
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
cmake -S . -B build/modeling -G Ninja
cmake --build build/modeling --target trace_graph -j2
```

宿主机也可以通过一次性命令执行同一检查：

```bash
scripts/run.sh modeling -- bash -lc \
  'cmake -S . -B build/modeling -G Ninja && cmake --build build/modeling --target trace_graph -j2'
```

不维护 fixture-backed smoke modeling 入口。Modeling 验证必须基于真实 profile manifest、显式 trace，或专项验证文档中记录的
可复现 profile/modeling run。

faithful replay：

```bash
scripts/model.sh \
  --config <modeling-config.json> \
  --profile-manifest <run_dir>/profile_manifest.json \
  --output-dir <run_dir>/modeling/faithful_replay \
  --mode faithful_replay \
  --emit-validation \
  --emit-module-summary
```

HiCache state prediction：

```bash
scripts/model.sh \
  --config <hicache-state-modeling-config.json> \
  --profile-manifest <run_dir>/profile_manifest.json \
  --output-dir <run_dir>/modeling/cache_state \
  --mode cache_state \
  --emit-module-summary \
  --emit-validation
```

常用覆盖项：

| CLI | 作用 |
| --- | --- |
| `--profile-manifest` | 覆盖 config 中的 manifest。 |
| `--output-dir` | 覆盖输出目录。 |
| `--mode` | 覆盖 modeling mode。 |
| `--emit-dag-chrome-trace` | 输出 DAG Chrome trace。 |
| `--emit-module-summary` | 输出 `model_summary.json`。 |
| `--emit-validation` | 输出 `validation.json`。 |

## Modeling Mode

| mode | 语义 |
| --- | --- |
| `faithful_replay` | 不加载子模块，不 patch DAG；消费完整真实执行 trace，验证 base DAG。 |
| `cache_state` | 加载状态子模块，维护内部状态，不修改 DAG。 |
| `cache_patch` | 子模块维护状态并通过 mutation API 修改 DAG；HiCache 尚未实现。 |

`replay` 只允许指 `mode=faithful_replay`。启用 HiCacheModule 的场景必须称为 `self-config prediction` 或
`cross-config prediction`。

## Trace Merger

`scripts/trace/trace_merger.py` 在 manifest 模式下合并：

- torch profiler trace；
- LD_PRELOAD trace；
- Python probe sidecar。

Trace merger 不根据 modeling mode 删除真实执行事件。`faithful_replay`、`cache_state` 和 `cache_patch`
应看到同一份 merged trace；差异只在是否加载子模块、是否产生 DAG mutation。

非执行类事件需要通过字段路由隔离：

| 字段 | 作用 |
| --- | --- |
| `model_input` | 是否进入 modeling 输入集合。 |
| `dag_input` | 是否作为默认性能 DAG 节点。 |
| `fact_class` | 子模块事实分类。 |
| `event_role` | atomic fact role，供状态子模块二级路由。 |
| `fact_granularity` | HiCache state 主线要求为 `atomic`。 |

## TraceGraph 结构

C++ 后端位于 `src/modeling/trace_graph`：

| 目录 | 内容 |
| --- | --- |
| `include/trace_graph/core` / `src/core` | `TraceEvent`、`DagGraph` 等基础结构。 |
| `include/trace_graph/frontend` / `src/frontend` | model config 和 trace normalize。 |
| `include/trace_graph/io` / `src/io` | Chrome trace 读取输出。 |
| `include/trace_graph/simulation` / `src/simulation` | 拓扑仿真。 |
| `include/trace_graph/modules` / `src/modules` | SimulationModule 子模块。 |
| `modules/hicache` | HiCache fact parser、radix tree、state model、summary。 |

构建目标：

```bash
scripts/run.sh modeling -- bash -lc 'cmake --build build/modeling --target trace_graph -j2'
```

## SimulationModule

所有 what-if 都必须规约为 C++ `SimulationModule`。Python 侧只做配置、trace merge、validation 编排。

子模块职责：

- 读取 normalized DAG / trace event；
- 解析自身事实；
- 维护内部状态；
- 必要时通过统一 mutation API 修改 DAG；
- 输出可选 summary/debug。

当前 active 子模块：

| 模块 | 状态 |
| --- | --- |
| `NodeScaleModule` | smoke / 节点耗时缩放。 |
| `HiCacheModule` | state-only；维护 cache state，不修改 DAG。 |

## HiCache State Backend

HiCache backend 当前是 state-only `SimulationModule`：它消费 invariant facts 和显式 target config，维护 target cache state，
输出 final state、transition trace、policy decision trace 和 validation summary；它暂不修改 DAG。

主链路：

```text
HiCacheFact
  -> HiCacheFactRouter
  -> HiCacheTokenPathStore
  -> HiCacheTargetPager
  -> scoped canonical HiCacheTokenRadixTree
  -> StorageDirectory / RefLedger / CapacityIndex / AsyncOperationTable / TargetControlClock
  -> HiCachePolicy
  -> DerivedStateView / transition summary / validation
```

### 输入边界

后端输入分流规则：

```text
consume fact iff model_input == true
    && fact_class == "invariant_state"
    && fact_granularity == "atomic"
    && role is a known atomic invariant
```

其它 HiCache 事件计入 `skipped_non_invariant_events`，不能更新 target state。`source_actual`、`timing_observation`、
`oracle_state` 和 debug/provenance 字段只能用于 token dictionary 水合、质量审计、validation label 或 transition
归因；不能回写为 target state mutation。

当前正常 state input role：

| role | 语义 |
| --- | --- |
| `request_bound_match_anchor` | request-scoped match-prefix token anchor；用于把 request id 绑定到可重建 token path，并做 target lookup / touch。 |
| `request_lifecycle_anchor` | finished/unfinished lifecycle 边界；模型基于 token store 恢复 committed/fill path，插入 radix 并释放 request KV lifecycle。 |
| `request_admission` | admission 边界；模型构造 target-side extend allocation intent、request ref 和 device allocator pressure。 |
| `prefetch_decision` | scheduler prefetch decision checkpoint；模型按 target policy 重新判断 planned pages、storage hit prefix、host reservation 和 anchor ref。 |
| `prefetch_check_point` | prefetch progress/wait 边界；模型推进 wait-complete / best-effort / timeout 的 ready、apply、late、revoked 或 suppressed。 |

match-prefix concrete path、lookup result、source insert/capacity/lock/maintenance、storage/controller result 和 async completion
只作为 `source_actual` / `timing_observation` evidence。unknown invariant role 必须进入 quality / summary error，不能静默消费。

cross-config rule diagnosis 必须先通过 hard `model_input_contract`：只比较 atomic invariant facts，逐 role 对比 count
和 request-normalized canonical fact multiset。raw `request_id` 是 run-local correlation id，不是跨配置 invariant。

### 组件边界

当前 C++ HiCache backend 文件：

| 文件 | 作用 |
| --- | --- |
| `hicache_fact.hpp/.cpp` | 识别 HiCache event、收集 token dictionary、解析 span 和事实字段。 |
| `hicache_router.hpp/.cpp` | role enum、输入门禁和 required field 检查。 |
| `hicache_token_store.hpp/.cpp` | request / operation scoped token path store；不保存 source page identity 作为状态输入。 |
| `hicache_target_pager.hpp/.cpp` | 按 target page size 投影完整 page hash、page id 和 page path。 |
| `hicache_token_radix_tree.hpp/.cpp` | 每个 `cache_scope` 一棵 canonical compressed radix tree；device/host/storage/ref 都是 node residency/ref 字段，不再维护 device tree 与 host tree 两套事实源。 |
| `hicache_storage_directory.hpp/.cpp` | target storage namespace；区分 materialized page record 与 backend-readable hash record，支持连续 storage hit prefix 查询。 |
| `hicache_ref_ledger.hpp/.cpp` | request / writeback / loadback / storage / prefetch owner 级 ref 账本，负责同步 tree 上的 lock ref 和 host ref。 |
| `hicache_capacity_index.hpp/.cpp` | mutation-driven device/host leaf index、capacity snapshot、victim choice 和 audit trace。 |
| `hicache_async_state.hpp/.cpp` | prefetch、writeback、loadback、storage operation table 和 lifecycle transition。 |
| `hicache_target_control_clock.hpp/.cpp` | target-side control checkpoint 与内部 operation id，避免把 source timestamp 当作 target 调度事实。 |
| `hicache_node_split_policy.hpp/.cpp` | radix split 时 residency/ref/hit count/page projection 的结构化迁移策略。 |
| `hicache_policy.hpp/.cpp` | 显式 target config 解析、SGLang-derived default 和 policy decision trace。 |
| `hicache_state_index.hpp/.cpp` | `DerivedStateView`，从 tree / storage / async 派生 validation-facing state sets。 |
| `hicache_model.hpp/.cpp` | role dispatch 和 state orchestration；不承担 fact schema、summary 或 DAG patch 职责。 |
| `hicache_summary.hpp/.cpp` | final state、transition trace、policy/capacity/ref/async 审计输出。 |
| `hicache_module.hpp/.cpp` | SimulationModule registry glue。 |

### Target Page Projection

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

### Target State

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

### Policy 与资源语义

request / allocator：

- `request_admission` 先用 target radix lookup 得到 prefix，再构造 `ExtendAllocationIntent`；
- eviction gate 对齐 SGLang allocator：用 `DeviceAllocatorLedger.available_pages()` 判断是否需要 eviction，不从 radix occupancy 反推；
- eviction budget 使用完整 allocation request；实际 active request reservation 使用本次真正分配/占用的 page；
- 当前 batch-level allocation intent 尚未由 profiling 提供，模型以 `extend_allocation_batch_size=1` 作为显式短期合同；
- `request_lifecycle_anchor` 在 finished / unfinished 上插入 committed path，并释放 duplicate / tail / overallocated KV 到 allocator ledger。

host / storage / prefetch：

- host cleanup victim 是 host radix leaf，必须 host-visible、evicted、无 lock/host ref protection，且没有 host-present backup child；
- host cleanup budget 来自本次 allocation request：prefetch 对齐 `evict_host(prefetch_length)`，write backup 对齐
  `evict_host(len(node.value))`；
- storage hit query 只保留连续命中前缀；storage-readable 不等于 host-visible；
- prefetch operation 保存 planned path、hit prefix、requested host pages、reserved host pages 和 anchor ref；
- wait-complete 完成后 apply ready pages，best-effort 在 checkpoint terminate，timeout 在 completed 或 timeout 边界 terminate/late；
- revoke / timeout incomplete 的 host reservation 进入 deferred release 近似，不立即从 host budget 中消失。

write policy：

- `write_through`、`write_through_selective` 和 `write_back` 共享 device insert、host backup、storage readable、capacity cleanup helper；
- `write_through_selective` 的 hit-count threshold 由 target policy 决定；
- write-through backup ACK 前会持有普通 lock ref；当前按下一条 target control fact drain，最后一条 fact 后的 pending ACK 可以保留到 final；
- write-back ACK 时序当前折叠为同步 completion，结果语义统一落到 host backup / storage readable / dirty clear；
- source writeback ACK、storage hit result、node remove result 和 async wall-clock completion 不能作为 normal state input。

### Summary

summary 输出位置：

```text
model_summary.json.modules[0].hicache
```

关键字段：

| 字段 | 说明 |
| --- | --- |
| `input_hicache_events` | 识别到的 HiCache events。 |
| `processed_hicache_events` | 实际消费的 invariant end events。 |
| `skipped_non_invariant_events` | 跳过的 source_actual / timing / oracle / debug events。 |
| `processed_events_by_role` | 各 role 消费计数。 |
| `missing_invariant_facts` | 缺失或未知 invariant 输入。 |
| `non_invariant_fact_usage` | 非不变量实际消费审计；正常必须为空。 |
| `final_state` | `DerivedStateView` 派生的模型最终 state sets 和 counts。 |
| `storage_directory_inclusive_state` | 包含 backend-readable hash 的 storage-inclusive projection。 |
| `transition_trace` | request / operation / page 级模型状态转移。 |
| `async_lifecycle_trace` | prefetch / writeback / loadback / storage operation lifecycle。 |
| `policy_decision_trace` | policy、allocator、capacity、loadback 和 cleanup 决策账本。 |
| `capacity_mutation_trace` / `capacity_victim_choices` | capacity index 增量更新和 victim 选择证据。 |
| `ref_mutation_trace` / `ref_audit` | owner 级 ref acquire/release 和 tree ref 一致性审计。 |

### State 到 DAG / E2E 路线

HiCache 的最终目标不是只让 final state 对齐，而是预测 target config 下的 E2E、关键路径和主要 cache 开销变化。
后续链路按以下边界推进：

```text
target semantic chain:
  invariant probe events
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
| target state | invariant facts + target config + oracle label | final state / transition trace | final state 对齐，`non_invariant_fact_usage=[]`。 |
| target intent | invariant facts + transition / policy / async / ref traces | cache operation intent stream | intent 可追溯到 state transition，source outcome 不混入。 |
| source physical attribution | torch / LD_PRELOAD / timing evidence + invariant anchors | source cache-owned node / edge groups | physical op group 归因稳定且不重复占用 DAG node。 |
| cache-neutral baseline | source full DAG + source physical groups | cache-neutral DAG | source cache cost 能拆出去并装回去。 |
| source/target cache diff | source physical groups + target intents | delete / insert / replace / resize decisions | self-config diff 基本 identity，cross diff 可解释。 |
| DAG patch | cache-neutral DAG + target intents | predicted target DAG | 无 dangling edge、无 cycle，blocking intent 位于正确依赖边界。 |
| duration / E2E | source calibration + target intents + target oracle label | predicted E2E / critical path audit | E2E 误差、phase 误差和 cache op contribution 可解释。 |

`transition` 解释 state 怎么变；`intent` 解释 target 下应该有哪些物理 cache 操作。DAG patch 应消费 intent，
不能直接消费 raw page-level transition 或 final page set。

## Validation

validation 不是默认输出，只有 `--emit-validation` 或 config 中 `outputs.emit_validation=true` 时生成。

HiCache state validation 必须同时看：

- `validation_ready`；
- `validation_errors`；
- `hicache_state.invariant_coverage_ready`；
- `hicache_state.missing_invariant_facts`；
- `hicache_state.non_invariant_fact_usage`；
- `hicache_state.final_state_match` / `raw_final_state_match`；
- normalized `sets_diff_by_tier`。

只要 `non_invariant_fact_usage` 非空，即使 final state 偶然对齐，也不能宣称 invariant-only prediction 通过。
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
