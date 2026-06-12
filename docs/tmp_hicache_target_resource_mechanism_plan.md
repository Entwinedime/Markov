# HiCache Target Resource Mechanism 临时方案

创建时间：2026-06-12

状态：临时文档。C++ device-side target resource mechanism 已按本文完成一轮实现和验证；因 host/prefetch/storage
visibility 仍有开放输入契约问题，本文暂时保留到下一轮 host-side 方案定稿后再删除。

## 2026-06-12 实施结果

已完成：

- C++ normal path 仍只消费 `model_input=true && fact_class=invariant_state && fact_granularity=atomic`；
- `HiCacheFact` / router 已解析并要求 `request_admission.admission_kind`，并补齐 admission scalar；
- `HiCacheTokenRadixTree` 已暴露 match/insert terminal node、ancestor page groups、动态 device eviction leaf groups；
- page -> group 索引不再把每个 page 映射回整条 projected request path；
- `HiCacheState` 已新增 request execution state、device request lock/ref count、admission reservation；
- `request_admission` 已按 target radix match 派生 active device lock/ref，并主动执行 target L1 pressure；
- `request_lifecycle_anchor` 已在 unfinished/finished 上转移或释放 request lock/ref；
- 未恢复 `capacity_request`、`lock_scope_delta` 或其它 source_actual normal state mutation。

验证结果：

| prediction | result |
| --- | --- |
| S1A self | final match：L1 `25/25`、L2/backuped `67/67`、evicted `42/42`、dirty/locked `0/0` |
| S1B -> S1A | final match，与 S1A self 同形 |
| S1B self | L1/dirty/locked match；L2/backuped/evicted `56/55`，missing 13、extra 14 |
| S1A -> S1B | 与 S1B self 同形 |

结论：

- device-side target-derived lock/ref、admission pressure、protected victim eligibility 已经修复 S1A final，并把 S1B
  从 backend-refactor 的 `70/55` 收敛到 `56/55`；
- S1B 剩余 diff 只在 host/L2 侧，附近 evidence 是 source_actual `host_ref_delta_observed`、storage hit query、
  prefetch terminate/completion、host memory release；
- 当前 profiling normal input 没有 target-independent host/prefetch completion work anchor，不能为消除 S1B residual
  而把 source_actual host/storage result 接回 normal model；
- 下一轮应设计 host/prefetch/storage 的 atomic invariant work intent，或在 backend 中实现完整 target async
  storage/host-ref model。

## 背景

当前 HiCache state normal prediction 已经完成 atomic invariant input contract 收窄：

- C++ normal path 只消费 `model_input=true && fact_class=invariant_state && fact_granularity=atomic`；
- 当前 normal role 为 `request_bound_match_anchor`、`request_lifecycle_anchor`、`request_admission`、
  `prefetch_decision`、`prefetch_check_point`；
- `source_actual` / `timing_observation` 不再作为 normal state mutation 输入，只可用于 token dictionary hydration、
  provenance 和诊断；
- legacy normal handlers 已删除，包括 `apply_capacity_request`、`apply_lock_scope_delta` 等。

在 backend-refactor 后，四个 normal prediction 都能处理同一组 350 个 atomic invariant end events，且同一 target 的
self/cross 结果形状一致，说明前端 atomic input 已经进入 C++，剩余问题主要是 target-derived mechanism 不完整。

当前 boundary-elision 诊断显示：

- full trace 首个分歧是 `lock_ref_transient_boundary`，主要影响 `locked_pages`；
- 排除 `locked_pages` 后，S1A 剩余 unlocked 分歧包括 `target_capacity_pressure_boundary=8`、
  `lock_protected_capacity_boundary=4`；
- 排除 `locked_pages` 后，S1B 剩余 unlocked 分歧包括 `target_capacity_pressure_boundary=12`、
  `lock_protected_capacity_boundary=2`；
- unlocked diagnostic oracle injection 可以让 S1A/S1B final 对齐，但这只证明缺口位置，不代表 normal model 可以消费
  oracle/source state。

## 重新审查结论

`target_capacity_pressure_boundary`、`lock_protected_capacity_boundary` 和 `lock_ref_transient_boundary`
不应拆成三个互不相关的修复项。它们是同一个 target resource mechanism 缺口的三个外观：

- `lock_ref_transient_boundary`：模型没有从 target request 生命周期派生 transient lock/ref，所以 `locked_pages`
  trace-level 状态缺失；
- `target_capacity_pressure_boundary`：模型只在 resident set 超 target capacity 后被动 enforce，没有模拟 admission /
  allocator reserve pressure；
- `lock_protected_capacity_boundary`：capacity pressure 发生时，模型缺少正确的 active lock/ref victim eligibility，
  导致可能驱逐被 request 保护的 radix path。

因此只修 `target_capacity_pressure_boundary` 风险很高：即使 eviction 数量对了，victim 仍可能因为缺少 target-derived
lock/ref 而错，后续换入/换出、dirty/backuped/evicted 都会继续漂移。

## 证据

当前 C++ 已经在 eviction victim 选择时检查 `state_index_.locked()`，但 normal path 几乎没有维护它。因此现有
`locked_pages` 更像 diagnostic/final-state 容器，不是完整的 request active protection layer。

第三方 SGLang HiRadix 真实语义：

- `inc_lock_ref(node)` / `dec_lock_ref(node)` 沿 radix node 到 root 的 ancestor chain 加减 `lock_ref`；
- device eviction heap 遇到 `x.lock_ref > 0` 会跳过该 node；
- `cache_unfinished_req` 会先 insert 新 path，再 release old last node，再 acquire new last node；
- `cache_finished_req` 会 insert final path，然后 release `req.last_node`；
- host storage 还有独立的 `host_ref_counter`，主要影响 host eviction / storage lifecycle。

逐 trace 证据：

- S1A 某个首个 unlocked capacity 分歧中，模型 L1 约 27 页，oracle L1 约 14 页，oracle 新增 13 个 evicted pages；
- 同一附近 source diagnostic capacity request 为 `1057` tokens，result evicted `1664` tokens，即 13 个 128-token pages；
- 这说明 oracle 不是简单的 “resident count > capacity” 后驱逐，而是 admission/allocator reserve pressure 触发；
- 附近同时存在 `lock_scope_inc/dec` evidence，说明 capacity pressure 与 lock/ref victim eligibility 是交叉问题。

这些 source diagnostic event 只能作为验证机制是否重建正确的证据，不能作为 normal model input。

## 设计目标

新增一个 target-derived resource mechanism，覆盖三类 boundary：

1. 从 invariant request anchors 派生 active request lock/ref；
2. 从 target config 和 admission facts 派生 allocation / capacity pressure；
3. 在同一次 pressure 中用 active lock/ref 约束 eviction victim eligibility。

原则：

- 不恢复 `apply_capacity_request`；
- 不恢复 `apply_lock_scope_delta`；
- 不消费 `source_actual` 的 `requested_tokens`、`evicted_tokens`、`lock_direction`、`delta` 作为 normal state mutation；
- 不引入 final-set patch；
- 优先使用现有 atomic invariant role 和 target config；
- 若现有 admission facts 不能精确还原 allocation work，再扩展 profiling contract，但扩展也必须是 target-independent /
  invariant work anchor，而不是 source actual result。

## 目标状态结构

建议在 `HiCacheState` 内新增或拆出以下状态。

### RequestExecutionState

按 scoped request key 记录 target-side request runtime：

```text
request_key
full_pages
matched_device_prefix_pages
last_device_chain_pages
active_device_lock_pages
device_reservation_tokens
device_reservation_pages
lifecycle_state
```

用途：

- `request_bound_match_anchor` 记录 target radix match 结果；
- `request_admission` 根据 match 结果 acquire request lock，并计算 allocation pressure；
- `request_lifecycle_anchor` 在 unfinished / finished 时更新或释放 active lock；
- request state 结束时清理 reservation，避免 resident + reservation 双算。

### ProtectionState

维护 ref count，而不是布尔锁：

```text
device_lock_count_by_page
host_lock_count_by_page
request_device_locks: request_key -> pages
request_host_locks: request_key -> pages
```

第一阶段优先实现 device lock/ref，因为本轮三个 boundary 都主要来自 device capacity / device lock interaction。

host protection 可先建结构但不急于完全驱动 storage state，避免把 async/storage boundary 混入本轮修复。

### TargetCapacityPressure

capacity 不再只靠 resident count 超容量时 enforce。需要新增 target allocation pressure：

```text
available_pages = l1_capacity_pages - l1_resident_pages - active_device_reservation_pages
if available_pages < requested_pages:
    evict target requested work, skipping protected radix node groups
```

注意：真实 `evict_from_tree_cache` 在 available 不足时调用 `tree_cache.evict(num_tokens=requested_tokens)`，不是只驱逐
shortage。因此模型中 eviction target 应优先按 requested work，而不是 `requested_pages - available_pages`。

由于 radix leaf group 可以大于 requested pages，实际 evicted pages 允许 overshoot。

## 事件语义

### request_bound_match_anchor

保持现有 target lookup / touch / promote 语义，新增：

- 记录 target radix longest prefix；
- 记录 request 对应 last matched device node；
- 记录该 node 的 ancestor page chain；
- 不在这里建立持久 request lock。

原因：真实持久 request lock 是 admission 时由 scheduler acquire，不是 match_prefix 本身长期持有。

### request_admission

这是本轮重构的核心事件。

在 admission end：

- 根据 request 的 target path / target radix match 找到 `req.last_node` 对应 ancestor page chain；
- `acquire_device_request_lock(request_key, chain_pages)`；
- 根据 admission invariant facts 和 target config 计算 requested allocation work；
- 用 active lock/ref 作为 victim protection 执行 target capacity pressure；
- 记录 request reservation，后续 lifecycle insert 时抵消或清理。

当前 profiling 中 `request_admission` 已包含这些可用字段：

```text
admission_kind
token_count
full_path_span
has_chunked_req
truncation_align_size
priority
ignore_eos
max_new_tokens
policy_params.thresholds
```

C++ `HiCacheFact` 目前尚未解析大部分 admission scalar / policy fields，这是需要补齐的后端对齐项。

### request_lifecycle_anchor: unfinished

模拟真实 `cache_unfinished_req`：

1. 插入 unfinished path；
2. 清理或抵消 admission reservation；
3. release old request lock；
4. acquire new inserted terminal node 的 ancestor chain；
5. 更新 request 的 `last_device_chain_pages`。

这样 chunked / unfinished request 的 active protection 会延续到下一阶段，capacity pressure 不会错误驱逐仍在使用的 prefix。

### request_lifecycle_anchor: finished

模拟真实 `cache_finished_req`：

1. 插入 final path；
2. 应用 write policy；
3. 清理剩余 reservation；
4. release request active device lock；
5. 结束 request state。

finished 后不应继续持有该 request 的 device lock，否则 locked final state 会过高并改变后续 evictability。

## Radix Tree 扩展

现有 `HiCacheTokenRadixTree` 支持 longest prefix、insert、leaf group 查找，但缺少 lock/ref 需要的 node-chain 视图。

这是本轮重构的关键路径。当前建模 radix tree 内部并非完全扁平：它有 token tree、page tree、parent/children 和压缩
segment。但暴露给 `HiCacheState` 的能力接近扁平/弱结构化：

- state model 只能拿到 longest prefix page count、page membership 和近似 leaf group；
- `leaf_group_by_page_` 目前把 path 中每个 page 映射回整条 projected path，不能表达真实 radix leaf node segment；
- model 拿不到 match / insert 后的 terminal node；
- model 拿不到 terminal node 到 root 的 ancestor chain；
- model 不能在 node 或 ancestor group 上维护 `lock_ref` / `host_ref_counter`；
- eviction 只能近似按 page/path group 跳过 `locked_pages`，不能复现 HiRadix “node locked 则 heap victim skip”的语义。

因此如果只在现有 page set 上加 capacity pressure 或 flat page lock，仍可能出现“驱逐数量对了，但 victim 错了”的问题。
必须先加强 radix tree 表达能力，让后端模型能表达 target radix node、node segment、ancestor chain 和 leaf/group victim
之间的关系。

建议扩展：

```cpp
struct PagePathMatch {
    size_t terminal_node;
    std::vector<std::string> matched_pages;
    std::vector<std::vector<std::string>> ancestor_page_groups;
};

PagePathMatch match_page_path(...);
PagePathMatch insert_page_path(...);
std::vector<std::string> node_pages(size_t node_id) const;
std::vector<size_t> ancestor_node_ids(size_t terminal_node) const;
std::vector<std::string> flattened_ancestor_pages(size_t terminal_node) const;
std::vector<std::string> leaf_group_for_page(...) const;
```

设计要求：

- lock/ref 保护 ancestor chain，不只保护 leaf page；
- ancestor chain 应保留 node/group 边界，不能只提前 flatten 成 page set；flatten 只能作为 summary / `locked_pages`
  输出；
- page -> leaf group 的映射必须来自真实 page radix leaf node，不能继续简单映射到整条 projected request path；
- eviction victim 仍以 radix leaf/group 为单位；
- protected check 应作用在 group/node 维度：若 group 中任一 page 被 active lock/ref 保护，则跳过该 group；
- page hash / target page projection 继续走 target pager，保证跨 page-size config 时 page identity 是 target-derived。

## Input Contract 审查

本轮不建议把 `lock_scope_delta`、`lock_scope_result_observed`、`capacity_request`、
`capacity_result_observed` 改成 normal invariant input。它们含 source/runtime/control-flow result，直接消费会破坏跨配置预测口径。

现有 profile 可能已经足够启动本轮重构：

- path-bearing facts 可给出 target page projection；
- `request_admission` 已携带 max_new / ignore_eos / chunking / policy params；
- source diagnostic `capacity_request.requested_tokens` 可作为 audit 对照。

但需要承认一个风险：当前 `request_admission` 采集的 token path 是 `origin_output`，不是直接的 `extend_input_len` /
`real_input_tokens`。如果 C++ 无法稳定从现有字段还原 allocator requested work，就需要扩展 profiling contract。

可接受的新 invariant fields / role 应是 work intent，而不是 source result：

```text
request_admission.extend_token_count
request_admission.prefix_token_count
request_admission.host_hit_token_count
request_admission.allocation_page_overhead
request_admission.allocation_kind = prefill_extend | chunked_extend | decode_reserve
```

也可以单独新增 role：

```text
device_allocation_work_anchor
```

但该 role 只能描述 target-independent allocation work identity / requested work shape，不能记录 actual evicted pages 或
source capacity result。

## 实施计划

### 阶段 1：机制审计报告

先增强 `scripts/internal/hicache_state_trace_divergence.py`，输出 structured boundary mechanism audit：

```json
{
  "classification": "target_capacity_pressure_boundary",
  "request_id": "...",
  "available_normal_context": {
    "computed_pressure_tokens": 1057,
    "computed_requested_pages": 9,
    "diagnostic_source_requested_tokens": 1057,
    "l1_pages_before": 27,
    "active_device_locked_pages": 6,
    "candidate_victim_pages": 13
  },
  "mechanism_input_ready": true
}
```

目的：

- 逐 trace 判断现有 admission facts 能否还原 source diagnostic capacity request；
- 检查 lock/ref active chain 是否可由 target radix state 推导；
- 避免在 C++ 中加入 underdetermined 规则。

### 阶段 2：C++ fact parser 对齐

扩展 `HiCacheFact`：

- `admission_kind`
- `has_chunked_req`
- `truncation_align_size`
- `priority`
- `ignore_eos`
- `max_new_tokens`
- 必要的 `policy_params` thresholds

这不是放宽 normal input gate，而是消费已经属于 `request_admission` atomic invariant contract 的字段。

### 阶段 3：Radix tree node-chain 能力

这是 C++ mechanism 的前置关键路径，不应被 flat page-set helper 替代。

扩展 `HiCacheTokenRadixTree`：

- match/insert 返回 terminal node；
- 暴露 node page segment、ancestor node ids 和 ancestor page groups；
- 能从 terminal node 返回 ancestor page groups；
- victim group 查询支持 protected group skip；
- 修正 page -> leaf group 语义，避免继续把每个 page 映射到整条 projected path；
- 保持现有 page projection / leaf group 行为稳定。

### 阶段 4：ProtectionState

实现：

- `acquire_device_request_lock(request_key, pages)`
- `release_device_request_lock(request_key)`
- `replace_device_request_lock(request_key, pages)`
- `device_locked_pages()`

所有 lock mutation 产生 state transition，方便 trace-level 对比 `locked_pages`。

### 阶段 5：Target capacity pressure

实现：

- admission-time requested work estimation；
- active reservation accounting；
- target device pressure eviction；
- protected group skip；
- dirty/writeback/write-through 语义沿用现有 eviction path。

不恢复 source capacity handler。

### 阶段 6：Lifecycle lock transfer

调整 `request_lifecycle_anchor`：

- unfinished：insert 后 old lock -> new lock；
- finished：insert 后 release active lock；
- request state cleanup。

### 阶段 7：验证

验证顺序：

1. `cmake --build build --target trace_graph -j2`
2. `python3 -m py_compile scripts/internal/hicache_state_trace_divergence.py`
3. `git diff --check`
4. full trace scan，不排除 `locked_pages`，观察 `lock_ref_transient_boundary` 是否下降；
5. unlocked scan，观察 `target_capacity_pressure_boundary` 和 `lock_protected_capacity_boundary` 是否同步下降；
6. 重跑四个 normal prediction：
   - S1A self
   - S1B self
   - S1A -> S1B
   - S1B -> S1A

## 预期结果

成功标准：

- normal model 不消费 source_actual capacity/lock delta；
- `locked_pages` 可由 target-derived active request locks 驱动；
- capacity pressure 在 admission / allocator work 边界主动发生；
- eviction victim eligibility 使用 active lock/ref；
- `lock_ref_transient_boundary`、`target_capacity_pressure_boundary`、`lock_protected_capacity_boundary` 同步下降；
- 同 target 的 self/cross prediction 仍保持同形；
- 剩余分歧若仍存在，应主要落在 async/storage/prefetch visibility boundary，而不是 device lock/capacity。

## 风险与开放点

1. Admission requested work 可能欠定。

现有 `request_admission` 字段可能不足以精确恢复 allocator requested work。如果 audit 发现 computed pressure 与 source
diagnostic `capacity_request.requested_tokens` 系统性不一致，需要补 profile invariant work fields，而不是回退到
source_actual capacity consumption。

2. Radix node split / group identity 必须谨慎。

lock/ref 是 node-chain 语义，eviction 是 leaf/group 语义。若 page radix tree 的 split 和 group 映射不稳定，会出现
locked count 对了但 victim 错的问题。

3. Host lock/ref 不宜混入第一阶段完成标准。

host `host_ref_counter` 会影响 L2/backuped/evicted 和 storage visibility，但它与 async/prefetch/storage boundary 耦合更强。
第一阶段只要求 device lock/capacity 三类 boundary 明显下降，host storage 后续单独建模。

4. Final state pass 不是唯一标准。

本轮目标是修 trace-level mechanism。即使 final set 暂时对齐，也要检查 transition trace 和 boundary classification 是否合理，
避免重新制造 final-set patch。

## 当前建议

下一步先做阶段 1 的 structured audit，再进入 C++ mechanism 实现。原因是本轮机制涉及 pressure 数量、request lock window、
radix victim eligibility 三个变量；先用 audit 证明现有 invariant facts 是否足够，可以避免在 C++ 中写出不可解释的经验规则。
