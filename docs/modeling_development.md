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

不维护 fixture-backed smoke modeling 入口。Modeling 验证必须基于真实 profile manifest、显式 trace，或专项验证文档中记录的
可复现 profile/modeling run。

faithful replay：

```bash
python3 scripts/internal/model_runner.py \
  --config <modeling-config.json> \
  --profile-manifest <run_dir>/profile_manifest.json \
  --output-dir <run_dir>/modeling/faithful_replay \
  --mode faithful_replay \
  --emit-validation \
  --emit-module-summary
```

HiCache state prediction：

```bash
python3 scripts/internal/model_runner.py \
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

HiCache backend 以 token radix tree 为事实中心，page set 只是 target page projection：

```text
HiCacheFactParser
  -> HiCacheFactRouter
  -> TokenPathStore
  -> TargetPager
  -> TokenRadixTree
  -> RequestState / DeviceCacheState / HostCacheState / AsyncState
  -> PolicyEngine
  -> StateTransitionLog / Summary / Validation
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
`oracle_state` 和 debug/provenance 字段不能更新 target state。C++ reader 会保留 `source_actual` / `timing_observation`
事件，使 fact parser 能读取 token dictionary 和 provenance；router 仍只把 atomic invariant role 分发给 state mutation。

当前 profiling 契约提供的正常 state input role：

| role | 语义 |
| --- | --- |
| `request_bound_match_anchor` | request-scoped match-prefix token anchor；用于把 request id 绑定到可重建 token path。 |
| `request_lifecycle_anchor` | finished/unfinished lifecycle 边界的 request-level anchor，不携带 committed/fill/generated suffix path。 |
| `request_admission` | 在 admission 边界记录 request token path、admission kind 和 policy。 |
| `prefetch_decision` | 在 scheduler prefetch decision checkpoint 上由 target state 重新判断 anchor、suffix 和是否 enqueue prefetch。 |
| `prefetch_check_point` | 按 target prefetch policy 推进等待、timeout、late、suppressed，不从 source completion 直接构造 ready set。 |

match-prefix concrete path、lookup result、cache config、lifecycle path/runtime、insert/capacity/lock/maintenance 和
storage/controller 事件只作为 `source_actual` / `timing_observation` evidence。unknown invariant role 应进入
`missing_invariant_facts["unknown_invariant_role"]` 或等价质量错误，不能静默消费。

cross-config rule diagnosis 必须先通过 hard `model_input_contract`：只比较 atomic invariant facts，逐 role 对比 count
和 request-normalized canonical fact multiset。raw `request_id` 是 run-local correlation id，不是跨配置 invariant；
sequence mismatch 只作为诊断输出。

### 组件边界

当前 C++ HiCache backend 文件：

| 文件 | 作用 |
| --- | --- |
| `hicache_fact.hpp/.cpp` | 识别 HiCache event、收集 token dictionary、解析 span 和事实字段。 |
| `hicache_router.hpp/.cpp` | role enum、输入门禁和 required field 检查。 |
| `hicache_token_store.hpp/.cpp` | request scoped token path store。 |
| `hicache_target_pager.hpp/.cpp` | target page size projection 和 page hash 生成。 |
| `hicache_token_radix_tree.hpp/.cpp` | token-level radix tree，按 `cache_scope` 隔离 token prefix、split、insert 和 page projection leaf group。 |
| `hicache_state_index.hpp/.cpp` | scoped page/node projection state index，维护 resident、dirty、backuped、evicted、lock、prefetch 和 hit count 集合。 |
| `hicache_model.hpp/.cpp` | state model orchestration：按 role 调用 store/pager/radix/state index。 |
| `hicache_summary.hpp/.cpp` | 输出 final state、transition trace、审计计数。 |
| `hicache_module.hpp/.cpp` | SimulationModule registry glue。 |

目标职责：

| 组件 | 责任 |
| --- | --- |
| `HiCacheFactRouter` | 第一层只按 invariant 判断；第二层把 role 转成 enum；缺字段、未知 role、非法 fact class 形成硬错误或 summary error。 |
| `TokenPathStore` | 收集 dictionary，解析 span，维护 request/operation 到 token range 的映射；不保存 source page identity 作为状态输入。 |
| `TargetPager` | 按 target page size 从 token range 推导完整 page hash、page index、page->token range 反查。 |
| `TokenRadixTree` | 维护 token/page compressed radix tree、parent/children、split/insert/lookup、terminal ancestor chain 和 leaf-group victim 查询。 |
| `DeviceCacheState` | 维护 device radix view、L1 resident、device lock/ref、admission pressure 和 device leaf-group victim selection。 |
| `HostCacheState` | 维护 host radix view、host ref/protection、storage-known、ready-but-not-visible 和 host-visible 状态。 |
| `AsyncState` | 维护 pending / ready / applied / suppressed / late，并区分 requested host budget 与 actual reservation。 |
| `PolicyEngine` | 根据显式 target config 实现 write policy、capacity/eviction policy、prefetch policy 和 storage policy。 |
| `StateTransitionLog` | 为每个 state mutation 输出 role/request/operation/node/page/seq/ts/provenance。 |
| `OracleValidation` | 只比较 `source_actual`/`oracle_state` 和 predicted state，不反写模型。 |

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

page 级集合只能从 token/node 状态投影出来。不允许再把 page-level radix tree 当作 source of truth；否则 page size
what-if 会继续在 split、lock/ref chain 和 evictable victim 上退化。

### Target State

summary 输出当前 validation 使用的集合，但集合来源必须是 token node/page projection：

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

request/admission 设计边界：

- `request_bound_match_anchor` 做 target lookup / touch / loadback；
- `request_admission` 根据 target radix match 派生 active device lock/ref 和 admission pressure；
- `request_lifecycle_anchor` 在 unfinished / finished 上 insert、迁移或释放 active request lock/ref。

host/device/async 设计边界：

- L2/backuped/host-visible mutation 只能通过 `add_host_visible_page()` / `remove_host_visible_page()`；
- `add_resident()` / `remove_resident()` 不应对 L2 隐式写 host topology、backuped 或 host-visible；
- request lookup 分别维护 device radix match、host radix match 和 target-visible prefix，避免把 device resident、
  host-visible、storage-known 混成一个 `matched_pages`；
- host prefetch allocation 失败时 cleanup budget 来自本次 page-aligned prefetch request，不按最终 L2 count、deficit
  或 fallback reserved count 反推；
- best-effort threshold / capacity limit 来自显式 target config；未配置时按 SGLang 源码语义投影；
- rate-limit 判断保持 `prefetch_tokens_occupied >= prefetch_capacity_limit`，capacity limit 为 0 时不被当作无限制；
- `prefetch_check_point` 推进 modeled async work lifecycle：ready work apply，未 ready / revoked / 未发出 work suppress，
  并释放 reservation。

write-back 设计边界：

- 不使用 page-level 前置 release 代替 write-back batch state machine；
- source writeback ack、storage hit result、node remove result 和 async wall-clock completion 不能作为 normal state input；
- write-back enqueue、dirty clear、backuped/host-visible 结果必须来自 target-derived policy 或新的 target-independent
  invariant。

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
| `final_state` | 模型最终 state sets 和 counts。 |
| `transition_trace` | request / operation / page 级模型状态转移。 |

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
- write-back background flush / `_evict_backuped()` / `writing_check(write_back=True)` 的批处理时序；
- transition exact oracle 和逐步 provenance 验收；
- scope-normalized comparison 之外的多 scope page identity 验证。

这些风险的当前验证状态维护在 `docs/validation/hicache_state_validation.md`，不在本文件重复实验分析。
