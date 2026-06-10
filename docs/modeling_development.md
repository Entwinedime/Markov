# Modeling 开发文档

维护方式：这是 modeling 主线文档。更新时直接删改本文件内容，不写流水账。

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

`predicted_e2e_ns` 来自 DAG 拓扑仿真。对于当前 HiCache state-only backend，它不是 cache state 正确性的验收指标。

## 运行入口

smoke：

```bash
python3 scripts/internal/model_runner.py \
  --config configs/modeling/smoke/modeling_smoke_hicache.json
```

faithful replay：

```bash
python3 scripts/internal/model_runner.py \
  --config configs/modeling/hicache/modeling_hicache_from_manifest.json \
  --profile-manifest <run_dir>/profile_manifest.json \
  --output-dir <run_dir>/modeling/faithful_replay \
  --mode faithful_replay \
  --emit-validation \
  --emit-module-summary
```

HiCache S1A self-config prediction：

```bash
python3 scripts/internal/model_runner.py \
  --config configs/modeling/hicache_state/modeling_hicache_state_mainline_one_prediction_s1a.json \
  --profile-manifest <run_dir>/profile_manifest.json \
  --output-dir <run_dir>/modeling/token_backend_s1a \
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
| `state_model_input` | 是否允许状态子模块消费。 |
| `fact_class` | 子模块事实分类。 |

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
cmake --build build --target trace_graph -j2
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

当前 C++ HiCache backend 文件：

| 文件 | 作用 |
| --- | --- |
| `hicache_fact.hpp/.cpp` | 识别 HiCache event、收集 token dictionary、解析 span 和事实字段。 |
| `hicache_router.hpp/.cpp` | 31-target invariant role enum、输入门禁和 required field 检查。 |
| `hicache_token_store.hpp/.cpp` | request scoped token path store。 |
| `hicache_target_pager.hpp/.cpp` | target page size projection 和 page hash 生成。 |
| `hicache_token_radix_tree.hpp/.cpp` | token-level radix tree，支持 token prefix、split、insert 和 page projection leaf group。 |
| `hicache_state_index.hpp/.cpp` | page/node projection state index，维护 resident、dirty、backuped、evicted、lock、prefetch 和 hit count 集合。 |
| `hicache_model.hpp/.cpp` | state model orchestration：按 role 调用 store/pager/radix/state index。 |
| `hicache_summary.hpp/.cpp` | 输出 final state、transition trace、审计计数。 |
| `hicache_module.hpp/.cpp` | SimulationModule registry glue。 |

当前代码仍是过渡实现，但已经不再保留旧 page-level radix backend。剩余主要问题：

- `request_cache_lifecycle`、`request_admission`、`maintenance_checkpoint` 仍只被 router 识别，尚未实现 state mutation；
- token radix tree 已按 token split/insert，但还没有完整 SGLang node parent/ref/host-ref 对等结构；
- `state_index` 已集中 page projection sets，但 evictable、priority、host ref 和 node-level ownership 仍是后续阶段；
- write policy、capacity victim、async prefetch/load-back/writeback 仍沿用过渡规则，后续要进入 `PolicyEngine` 和 `AsyncState`。

后续 C++ 工作继续按下面的目标架构推进；旧 page-level state machine 不作为兼容对象保留。

### 输入边界

后端输入分流规则已经收紧成一个主判断：

```text
consume fact iff fact_class == "invariant_state" && state_model_input == true
```

其它 HiCache 事件计入 `skipped_non_invariant_events`，不能更新 target state。`source_actual`、`timing_observation`、
`oracle_state` 和 debug/provenance 字段只能进入 validation、diagnostics 或 profile quality。

当前 mainline S1A/S1B profiling 契约固定为 31 个 target：

| fact class | target count | state input |
| --- | ---: | --- |
| `invariant_state` | 16 | true |
| `source_actual` | 13 | false |
| `timing_observation` | 2 | false |

当前 profiling 契约提供的 invariant roles：

| role | 语义 |
| --- | --- |
| `request_tokens` | 注册 request 的完整 token path。 |
| `lookup_path` | 在 target token radix tree 上查 longest prefix；matched/source hit 只能由模型自己产生。 |
| `request_cache_lifecycle` | 用 finished/unfinished request 的 committed/fill token path、chunked/is_insert/priority 驱动 request lifecycle。 |
| `request_admission` | 在 prefill admission / chunked continuation 边界重放 target-side load-back、临时/正式 lock/ref 和截断后的 admitted path。 |
| `insert_path` | 插入 target token path，更新 node/page resident、hit count、priority、dirty/backuped 和 write policy 状态。 |
| `prefetch_decision` | 在 scheduler prefetch decision checkpoint 上由 target state 重新判断 anchor、suffix 和是否 enqueue prefetch。 |
| `prefetch_check_point` | 按 target prefetch policy 推进等待、timeout、late、suppressed，不从 source completion 直接构造 ready set。 |
| `maintenance_checkpoint` | 在 check/load/flush 边界推进 modeled async queue、write/load ack 和 storage control。 |
| `capacity_request` | 作为 allocation pressure checkpoint，victim 由 target eviction policy 决定。 |
| `lock_scope_delta` | 以 token logical path 和 target radix parent chain 重建 lock/ref，不消费 source return delta。 |
| `cache_config_observed` | 只做配置审计；target page size、capacity、write/prefetch policy 仍来自 modeling config。 |

`prefetch_intent` 已从 invariant 中移除；当前配置只保留 `prefetch_intent_observed` 作为 `source_actual`。
后端重构必须显式处理每个 invariant role；不能通过消费 `source_actual` 事件绕过 role 缺口。unknown invariant role
应进入 `missing_invariant_facts["unknown_invariant_role"]` 或等价质量错误，不能静默消费。

31-target 契约在当前 mainline S1A/S1B scope 内冻结。profile quality 通过后，如果 prediction 仍 mismatch，默认归类为
backend model/rule 缺陷；只有下列情况才考虑新增采集 target：

- profile quality 明确证明现有 target 缺 required field、token dictionary 或 seq/scope；
- 进入 DLLM、disaggregation、streaming session、abort/timeout/preemption 等新 scope；
- SGLang upstream path 改变，导致当前 hook 无法覆盖同一语义边界。

### 目标架构

重构后的 HiCache backend 以 token radix tree 为事实中心，page set 只是 projection：

```text
HiCacheFactParser
  -> HiCacheFactRouter
  -> TokenPathStore
  -> TargetPager
  -> TokenRadixTree
  -> RequestState / NodeStateIndex / AsyncState
  -> PolicyEngine
  -> StateTransitionLog / Summary / Validation
```

| 组件 | 责任 |
| --- | --- |
| `HiCacheFactRouter` | 第一层只按 invariant 判断；第二层把 role 转成 enum；缺字段、未知 role、非法 fact class 形成硬错误或 summary error。 |
| `TokenPathStore` | 收集 dictionary，解析 span，维护 request/operation 到 token range 的映射；不保存 source page identity 作为状态输入。 |
| `TargetPager` | 按 target page size 从 token range 推导完整 page hash、page index、page->token range 反查。 |
| `TokenRadixTree` | 维护 token-level node、edge token slice、parent/children、split/insert/remove/lookup；node 是 ref/lock/evictable 的归属点。 |
| `NodeStateIndex` | 维护 node/page 的 L1/L2/L3 resident、dirty、backuped、evicted、hit count、priority、lock/ref、host ref 和 evictable membership。 |
| `RequestState` | 维护 request full/fill/committed/admitted path、lookup result、chunked continuation、临时 lock/ref 和 lifecycle phase。 |
| `PolicyEngine` | 根据显式 target config 实现 write policy、capacity/eviction policy、prefetch policy 和 storage policy。 |
| `AsyncState` | 维护 prefetch/load-back/writeback queue、ready/late/suppressed/acked 状态；source timing 只作为 validation 样本。 |
| `StateTransitionLog` | 为每个 state mutation 输出 role/request/operation/node/page/seq/ts/provenance。 |
| `OracleValidation` | 只比较 `source_actual`/`oracle_state` 和 predicted state，不反写模型。 |

建议文件边界：

| 文件 | 目标 |
| --- | --- |
| `hicache_fact.*` | 只做 event/fact/schema 解析和 token dictionary 观察。 |
| `hicache_router.*` | role enum、输入门禁、required field 检查和错误分类。 |
| `hicache_token_store.*` | token path/span/request mapping。 |
| `hicache_target_pager.*` | token range 到 target page projection。 |
| `hicache_token_radix_tree.*` | token-level radix node 模型。 |
| `hicache_state_index.*` | node/page resident、dirty、backuped、evicted、lock/ref、evictable 索引。 |
| `hicache_policy.*` | write/capacity/prefetch/storage policy。 |
| `hicache_async_state.*` | prefetch/load-back/writeback queue。 |
| `hicache_model.*` | orchestration：按 seq 应用 role，不承载复杂策略细节。 |
| `hicache_summary.*` | summary、transition trace 和 validation-facing 输出。 |

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
- `cache_scope` 参与内部 page id，validation 可用 `oracle_page_key_mode=strip_scope` 与 raw oracle hash 对齐。

page 级集合只能从 token/node 状态投影出来。重构后不允许再把 page-level radix tree 当作 source of truth；否则
page size what-if 会继续在 split、lock/ref chain 和 evictable victim 上退化。

### Target State

重构后的 summary 仍输出当前 validation 使用的集合，但集合来源改为 token node/page projection：

| 集合 | 来源 |
| --- | --- |
| `l1_resident_pages` | node/page device resident projection。 |
| `l2_resident_pages` | host resident projection。 |
| `l3_resident_pages` | storage-readable projection。 |
| `dirty_pages` | write-back dirty page projection。 |
| `backuped_pages` | 已持久/host backuped projection。 |
| `evicted_pages` | target eviction lifecycle projection。 |
| `locked_pages` | token radix parent/ref chain 上 lock/ref 非零的 page projection。 |
| `prefetch_planned_pages` | `AsyncState` 中 target policy 已 enqueue 的 page projection。 |
| `prefetch_ready_pages` | modeled async queue 已完成且 target 可见的 page projection。 |
| `prefetch_late_pages` | target policy 判定 timeout/late 的 page projection。 |
| `prefetch_suppressed_pages` | best_effort/finalization/timeout 下被 target policy 放弃的 page projection。 |
| `page_hit_counts` | policy-visible page hit count projection。 |

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
| `final_state` | 模型最终 state sets 和 counts。 |
| `transition_trace` | request / operation / page 级模型状态转移。 |

### 重构阶段

1. `router/schema`：实现 role enum 和 required field hard fail；删除旧 `prefetch_intent` invariant 分支；fixtures 覆盖 31-target role。
2. `token store + pager`：把 token dictionary/span、request path、target page projection 从 `HiCacheState` 中拆出；验证 page64/page128 hash。
3. `token radix tree`：实现 token-level insert/lookup/split/remove、node parent chain、node->page projection；page-level radix tree 退役。
4. `state index`：实现 node/page resident、dirty、backuped、evicted、hit/priority、lock/ref、host ref、evictable 索引。
5. `request/admission lifecycle`：用 `request_tokens`、`lookup_path`、`request_admission`、`request_cache_lifecycle` 串起 lookup、load-back、chunked admission、insert/free。
6. `policy engine`：重建 write-through-selective、write-back、capacity victim、evictable skip、lock/ref skip 和 L2/L3 backup 规则。
7. `async state`：用 `prefetch_decision`、`prefetch_check_point`、`maintenance_checkpoint` 重建 prefetch/load-back/writeback queue；只把 timing/source actual 用于对照。
8. `validation/provenance`：四向 S1A/S1B prediction 必须输出逐 trace diff，能把 mismatch 分类为已建模规则 bug、当前 scope 不可观测机制或新 scope 需求。
9. `DAG patch`：只有 state final sets 在 self-config 和 cross-config 均闭环后，才实现 cache state 到 DAG mutation。

### 当前实现进度

截至 2026-06-11 02:03，阶段 1/2/3/4 已完成最小建模：

- 新增 `hicache_router.hpp/.cpp`，集中维护 31-target invariant role enum、第一层输入门禁和 required field 检查；
- `HiCacheStateModel::run` 不再在本地维护 role 字符串集合，而是通过 router 分发；
- unknown invariant role 进入 `missing_invariant_facts["unknown_invariant_role"]`；
- 缺 token dictionary/span、`cache_scope` 或 `seq_no` 的 invariant 在进入 state mutation 前被拒绝，不计入 `processed_hicache_events`；
- 新增 `hicache_token_store.hpp/.cpp`，request path 由 token store 维护，不再在 state model 内保存 request pages；
- 新增 `hicache_target_pager.hpp/.cpp`，target page projection 从 token path 和 target page size 推导；
- 新增 `hicache_token_radix_tree.hpp/.cpp`，旧 `hicache_radix_tree.hpp/.cpp` 已删除；
- 新增 `hicache_state_index.hpp/.cpp`，resident/dirty/backuped/evicted/lock/prefetch/hit count 集合由 state index 维护；
- `prefetch_intent` 不再是 invariant input；当前 transitional backend 用 `prefetch_decision` 在 target token radix 上计算 planned suffix；
- `request_cache_lifecycle`、`request_admission`、`maintenance_checkpoint` 已被 router 识别，但还未实现 target state mutation；
  目前会进入 `missing_invariant_facts["unimplemented_invariant_role.<role>"]`，避免静默吞事件。
- 最小验证新增并通过：target page size projection fixture、token radix split projection fixture、原 HiCache state fixtures。

## HiCache 当前验证状态

当前有效 modeling 证据仍是 2026-06-10 四向结果；它来自 31-target 契约之前的 profile，只能证明旧 page-level backend
不正确，不能作为新 31-target 契约的验收结果。新 profile 完成后必须按上面的目标架构验证。

历史 S1A manual run：

```text
run label: 20260610_073946_profiling_hicache_state_mainline_one_matrix/01_s1a_manual
```

profile quality：

| 指标 | 值 |
| --- | --- |
| configured target count | 12 |
| `quality_ready` | true |
| `profiling_ready` | true |
| invariant events | 6960 |
| required end events | 3480 |
| missing token dictionary refs | 0 |
| seq order errors | 0 |

model summary：

| 指标 | 值 |
| --- | --- |
| `input_hicache_events` | 7376 |
| `processed_hicache_events` | 3480 |
| `skipped_non_invariant_events` | 416 |
| `missing_invariant_facts` | `{}` |
| `non_invariant_fact_usage` | `[]` |
| `dag_mutations` | 0 |

normalized oracle validation：

| set | model | oracle | 结果 |
| --- | ---: | ---: | --- |
| `l1_resident_pages` | 32 | 54 | missing 22 |
| `l2_resident_pages` | 80 | 106 | missing 26 |
| `dirty_pages` | 0 | 0 | match |
| `backuped_pages` | 80 | 106 | missing 26 |
| `evicted_pages` | 48 | 52 | missing 26, extra 22 |
| `locked_pages` | 11 | 11 | match |

结论：旧 profile 已经证明 invariant-only 分流方向正确，但 page-level state model 不正确。新 31-target profile 完成后，
应按目标架构重构并重跑四向验证，而不是继续在旧 page-level backend 上小修。

## 当前未实现

- HiCache state-to-DAG patch；
- async prefetch scheduler 的 timing/queue 模型；
- write-back background flush 的完整 target decision；
- exact eviction / evictable / allocator 逻辑；
- token-level radix node parent/ref chain 的完整 SGLang 等价实现；
- transition exact oracle。

这些缺口维护在 `docs/validation/hicache_state_model_defects.md`。

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
