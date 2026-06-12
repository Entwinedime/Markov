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
| `hicache_router.hpp/.cpp` | HiCache role enum、输入门禁和 required field 检查。 |
| `hicache_token_store.hpp/.cpp` | request scoped token path store。 |
| `hicache_target_pager.hpp/.cpp` | target page size projection 和 page hash 生成。 |
| `hicache_token_radix_tree.hpp/.cpp` | token-level radix tree，按 `cache_scope` 隔离 token prefix、split、insert 和 page projection leaf group。 |
| `hicache_state_index.hpp/.cpp` | scoped page/node projection state index，维护 resident、dirty、backuped、evicted、lock、prefetch 和 hit count 集合。 |
| `hicache_model.hpp/.cpp` | state model orchestration：按 role 调用 store/pager/radix/state index。 |
| `hicache_summary.hpp/.cpp` | 输出 final state、transition trace、审计计数。 |
| `hicache_module.hpp/.cpp` | SimulationModule registry glue。 |

当前代码仍是过渡实现，但已经不再保留旧 page-level radix backend。当前 mainline profile config 使用 target-level
atomic fact 契约，只把 `request_bound_match_anchor`、`request_lifecycle_anchor`、`request_admission`、
`prefetch_decision` 和 `prefetch_check_point` 作为正常 state 输入；其它 cache-stage concrete path、lifecycle path/runtime
和 source control-flow role 都是 `source_actual` / `timing_observation` evidence。剩余主要问题：

- `request_admission` 作为 request-scoped token context 进入 token store；当前不直接产生 resident/dirty mutation；
- 旧 `request_tokens`、`lookup_path`、`request_cache_lifecycle` 混合 role 已从主配置和 router 删除；`insert_path`、
  `capacity_request`、`lock_scope_delta` 和 `maintenance_checkpoint` 保留为 source evidence，不进入正常模型；
- token radix tree 已按 `cache_scope` 做 token split/insert 隔离，但还没有完整 SGLang node parent/ref/host-ref 对等结构；
- `state_index` 已集中 scoped page projection sets，L1/L2 capacity 按 `cache_scope` 统计和驱逐，但 evictable、priority、
  host ref 和 node-level ownership 仍是后续阶段；
- write policy、capacity victim、async prefetch/load-back/writeback 仍沿用过渡规则；如果后续要重新启用
  lifecycle/capacity/lock/maintenance 的正常输入，必须先通过 cross audit 的 `model_input_contract_ready`，不能直接复用
  source trace 的 concrete path 或 control-flow 序列。

后续 C++ 工作继续按下面的目标架构推进；旧 page-level state machine 不作为兼容对象保留。

### 输入边界

后端输入分流规则已经收紧成一个主判断：

```text
consume fact iff model_input == true
    && fact_class == "invariant_state"
    && fact_granularity == "atomic"
    && role is a known atomic invariant
```

其它 HiCache 事件计入 `skipped_non_invariant_events`，不能更新 target state。`source_actual`、`timing_observation`、
`oracle_state` 和 debug/provenance 字段只能进入 validation、diagnostics 或 profile quality。

当前 mainline S1A/S1B profiling 契约为 33 个 atomic target，正常 state model input 是其中 7 个 target / 5 个 role：

| fact class | target count | state input |
| --- | ---: | --- |
| `invariant_state` | 7 | true |
| `source_actual` | 24 | false |
| `timing_observation` | 2 | false |

当前 profiling 契约提供的正常 state input：

| role | 语义 |
| --- | --- |
| `request_bound_match_anchor` | request-scoped match-prefix token anchor；用于把 request id 绑定到可重建 token path。 |
| `request_lifecycle_anchor` | finished/unfinished lifecycle 边界的 request-level anchor，不携带 committed/fill/generated suffix path。 |
| `request_admission` | 在 prefill admission / chunked continuation 边界记录 request token path、admission kind 和 policy。 |
| `prefetch_decision` | 在 scheduler prefetch decision checkpoint 上由 target state 重新判断 anchor、suffix 和是否 enqueue prefetch。 |
| `prefetch_check_point` | 按 target prefetch policy 推进等待、timeout、late、suppressed，不从 source completion 直接构造 ready set。 |

当前 profile config 已删除 `request_tokens`、`lookup_path`、`request_cache_lifecycle` 这类混合 role。match-prefix concrete
path、lookup result、cache config、lifecycle path/runtime、insert/capacity/lock/maintenance 和 storage/controller 事件只作为
`source_actual` / `timing_observation` evidence。后端仍必须显式处理每个真正进入模型的 invariant role；不能通过消费
`source_actual` 事件绕过 role 缺口。unknown invariant role 应进入
`missing_invariant_facts["unknown_invariant_role"]` 或等价质量错误，不能静默消费。

cross-config rule diagnosis 必须先通过 hard `model_input_contract`：只比较 atomic invariant facts，逐 role 对比 count
和 request-normalized canonical fact multiset。raw `request_id` 是 run-local correlation id，不是跨配置 invariant；
sequence mismatch 只作为诊断输出。只有下列情况才考虑新增采集 target：

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
| `pending_writeback_pages` | `AsyncState` 中 dirty eviction 已 enqueue、尚未由 maintenance checkpoint ack 的 page projection。 |
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

1. `router/schema`：实现 atomic role enum 和 required field hard fail；router 只接受当前 5 个正常输入 role。
2. `token store + pager`：把 token dictionary/span、request path、target page projection 从 `HiCacheState` 中拆出；验证 page64/page128 hash。
3. `token radix tree`：实现 token-level insert/lookup/split/remove、node parent chain、node->page projection；page-level radix tree 退役。
4. `state index`：实现 node/page resident、dirty、backuped、evicted、hit/priority、lock/ref、host ref、evictable 索引。
5. `request/admission lifecycle`：当前直接消费 request-bound match anchor、lifecycle anchor 和 admission facts；更细的 lookup、load-back、chunked admission、insert/free 仍需要 target-derived 机制或新的 target-independent atomic invariant。
6. `policy engine`：重建 write-through-selective、write-back、capacity victim、evictable skip、lock/ref skip 和 L2/L3 backup 规则。
7. `async state`：当前只直接消费 `prefetch_decision` 和 `prefetch_check_point`；maintenance polling/check-kind 序列必须先变成 target-derived 机制或高层 invariant，才能参与 prefetch/load-back/writeback queue。
8. `validation/provenance`：四向 S1A/S1B prediction 必须输出逐 trace diff，能把 mismatch 分类为已建模规则 bug、当前 scope 不可观测机制或新 scope 需求。
9. `DAG patch`：只有 state final sets 在 self-config 和 cross-config 均闭环后，才实现 cache state 到 DAG mutation。

### 当前实现进度

截至 2026-06-12 12:42，完成 HiCache state atomic fact 输入契约重构：

- profile config 现在是 33 个 target-level atomic `fact` target，正常 state input 是 7 个 target / 5 个 role；
- `request_tokens`、`lookup_path`、`request_cache_lifecycle` 混合 role 已删除，相关 callable 拆成 invariant anchor 与
  `source_actual` path/runtime evidence；
- C++ router 已同步切到 atomic role gate，不再把 source/control-flow role 作为正常模型入口；
- cross audit 的 hard `model_input_contract` 按 atomic invariant role 比较 count 和 request-normalized canonical fact
  multiset，sequence mismatch 不再阻塞输入契约。

截至 2026-06-12 15:38，新的 atomic profile 已完成 S1A/S1B manual run：

- suite：`data/profile_runs/sglang/20260612_053153_profiling_hicache_state_mainline_one_matrix`；
- suite `failures=[]`，两侧 `profiling_ready=true`，Python probe trace 各 2 个；
- 两侧 normal model input 都是 `350` 个 completed atomic invariant facts，双向 cross audit
  `model_input_contract_ready=true`、blocking roles 为空；
- S1A profile quality 的整体 `quality_ready=false` 只来自 source evidence `prefetch_transfer` 未观测到，不影响 normal
  model-input hard gate。

截至 2026-06-12 03:49，基于修复前 S1A/S1B 31-target fast-pressure suite 完成 async-elision 与 cross input-contract
诊断；这些结果是本轮 demotion 的历史证据：

- S1A self normal prediction 已 final state match；
- S1B self normal 仍有 L2/backuped/evicted `56/55`、missing `13`、extra `14`，但 C++ 模型侧
  diagnostic async-elision 后 final state match，说明当前 self-config diff 来自 async/input-boundary 分岔后的连锁状态差；
- cross-config 仍未闭环，不能用 timestamp oracle injection：S1A/S1B trace window 不重叠；
- `unbound_match_prefix_paths` / `insert_paths` 已被 temporal anchor 诊断证明是 target-derived/page-aligned projection
  目标，不应继续直接消费 source unbound path；
- `request_cache_lifecycle` 的 mismatch 已定位为同 request prompt anchor 一致、committed/generated suffix 不同；
- `capacity_request` 的 cross mismatch 已定位为 target/control-flow pressure sequence 差异，而不是 page-size-only：
  两向 `page_size_only_explains_count=0`、`requested_tokens_differ_count=8`，另有 `4` 个 one-sided event。
- `lock_scope_delta` 的 cross mismatch 已定位为各自 inc/dec 平衡但序列/path/direction 不可复用：net 都是 `0`，
  但 path mismatch `228`、direction mismatch `166`、one-sided event `44`；`maintenance_checkpoint` 的 cross
  mismatch 主要是 check_kind 调度错位，`check_kind_mismatch=348`、one-sided event `4`。
- 修复前 `normal_model_input_contract` 已把 cross audit 的可消费结论结构化：两向都是
  `contract_status=blocked_by_input_contract`，`unsafe_roles_after_projection` 为 `request_cache_lifecycle`、
  `capacity_request`、`lock_scope_delta`、`maintenance_checkpoint`。因此现在还不能声称 cross 已排除 async 后无其他
  state-rule 问题；本轮已先把这些 source/control-flow 事件降级为 evidence/control-flow boundary，真实 cross run 仍需重跑。

截至 2026-06-11 15:11，基于 S1B 31-target fast-pressure profile 完成一轮 backend 修正：

- router 接受 zero-token span：`begin == end` 的合法空路径不再被误报为缺 token dictionary；
- `request_admission` / `request_cache_lifecycle` 不再被报为 unimplemented invariant role，而是更新 request scoped token store；
- `maintenance_checkpoint` 不再被报为 unimplemented invariant role，当前作为显式 no-op 边界事件处理；
- fact replay 排序改为严格全局时间顺序，并在同 timestamp/scope 下使用 `seq_no` 破 ties，避免旧 comparator 的非全序导致
  lock/ref delta 乱序；
- radix tree 和 L1/L2 capacity enforcement 改为按 `cache_scope` 隔离；raw model state 会保留两个 HiRadixCache scope，
  normalized validation 仍按 page hash union 对比 oracle；
- `capacity_request` 接入 L1 modeled eviction pressure：当 fact 带 `requested_tokens` 时按 target `page_size`
  重算 requested pages，再按 target eviction policy 选择 victim；不消费 source victim，也不把 cross source
  `params.num_tokens` 序列当作 target-independent 输入；
- 最新 S1B self validation 的 `missing_invariant_facts=[]`、`non_invariant_fact_usage=[]`，`locked_pages` 已对齐
  `0/0`；剩余 mismatch 集中在 L2/backuped/evicted 以及少量 L1/dirty extra。

截至 2026-06-11 02:03，阶段 1/2/3/4 已完成最小建模：

- 新增 `hicache_router.hpp/.cpp`，集中维护 HiCache role enum、第一层输入门禁和 required field 检查；
- `HiCacheStateModel::run` 不再在本地维护 role 字符串集合，而是通过 router 分发；
- unknown invariant role 进入 `missing_invariant_facts["unknown_invariant_role"]`；
- 缺 token dictionary/span、`cache_scope` 或 `seq_no` 的 invariant 在进入 state mutation 前被拒绝，不计入 `processed_hicache_events`；
- 新增 `hicache_token_store.hpp/.cpp`，request path 由 token store 维护，不再在 state model 内保存 request pages；
- 新增 `hicache_target_pager.hpp/.cpp`，target page projection 从 token path 和 target page size 推导；
- 新增 `hicache_token_radix_tree.hpp/.cpp`，旧 `hicache_radix_tree.hpp/.cpp` 已删除；
- 新增 `hicache_state_index.hpp/.cpp`，resident/dirty/backuped/evicted/lock/prefetch/hit count 集合由 state index 维护；
- `prefetch_intent` 不再是 invariant input；当前 transitional backend 用 `prefetch_decision` 在 target token radix 上计算 planned suffix；
- `request_cache_lifecycle`、`request_admission`、`maintenance_checkpoint` 已被 router 识别；
- 最小验证新增并通过：target page size projection fixture、token radix split projection fixture、原 HiCache state fixtures。

## HiCache 当前验证状态

当前有效代码/配置状态是 2026-06-12 input-contract repair。最新真实 modeling 证据仍是修复前的
`HCSV-20260612-async-elision-current-self-and-cross`，记录在 `docs/validation/hicache_state_validation.md`，只能作为
降级变体输入的历史证据。修复前简要结论：

- S1A self normal final state match；
- S1B self normal final state mismatch 仅剩 L2/backuped/evicted，但模型侧 async-elision 后 final state match；
- S1A->S1B 和 S1B->S1A 仍未通过，阻塞点是 cross input contract：lifecycle/control-flow facts 尚未证明
  target-independent。

atomic profile config 下的 S1A/S1B profile、profile quality 和 cross audit 已完成，当前
`model_input_contract_ready=true`。旧 retained audit 不能再被解读为当前 atomic 正常输入契约下的失败结果；下一步是
在新 profile 上重跑 modeling validation。

下面的 2026-06-10 四向结果来自 atomic 契约之前的 profile，只能证明旧 page-level backend 不正确，不能作为当前
33-target atomic 契约的验收结果。

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

结论：旧 profile 已经证明 invariant-only 分流方向正确，但 page-level state model 不正确。新 atomic profile 完成后，
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
