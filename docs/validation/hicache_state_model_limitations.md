# HiCache State Model 长期限制记录

本文记录 HiCache state model 当前仍存在的中长期建模缺口、临时妥协方案和后续收敛方向。这里的内容不是实验流水账，也不是一次性 debug
记录；只有会影响后续 validation 解释、cross-config prediction 或 transition exactness 推进的模型边界才写入本文。

## HCSV-LIMIT-001: batch-level KV allocation intent 尚未完整建模

### 问题

当前 HiCache state model 主要从 `request_admission` 这类 per-request invariant role 推导 device capacity pressure。模型在 admission
阶段根据 target radix lookup 估算 request path 中尚未 resident 于 L1/device 的 page，并以此触发 modeled eviction。

但 SGLang 在 paged KV allocator 链路中，device capacity pressure 的真实预算来自 batch-level allocation，而不是单个 request 的
radix miss page 数。

SGLang `alloc_paged_token_slots_extend()` 的预算语义是：

```text
requested_tokens = extend_num_tokens + len(seq_lens_cpu) * page_size
```

其中：

```text
len(seq_lens_cpu) == batch_size == len(batch.reqs)
```

也就是说，一次 `HiRadixCache.evict(EvictParams(num_tokens=...))` 对应的是一次 allocator batch，而不是必然对应一个 request。

因此，当一个 allocation batch 中包含多个 request 时，下面两种预算不等价：

```text
模型当前近似：
  对每个 request 独立估算 capacity pressure

SGLang 实际语义：
  对整个 ScheduleBatch 估算一次 allocation pressure
```

这会影响：

- 是否触发 L1 eviction；
- eviction requested page 数；
- eviction 发生时机；
- radix victim order；
- 最终 L1 / evicted page set。

### 当前证据

在当前 `c0_wt_timeout_p128_balanced` self prediction 的 `manual_pressure_prefetch` slice 中，source trace 显示若干 `capacity_request`
发生在：

```text
request_admission
  -> capacity_request
  -> request_lifecycle_anchor(unfinished)
```

之间。

这些 `capacity_request` 的 `requested_tokens` 与 admission token count 满足：

```text
requested_tokens = token_count + 128
```

由于 c0 的 page size 为 128，因此该 slice 中可以反推出：

```text
batch_size = 1
```

这说明当前残差不是 storage prefetch 本身导致的，而是 `request_admission -> paged extend KV allocation -> device capacity pressure`
这段链路尚未完整覆盖。

### 当前妥协方案

短期为了推进 c0 self prediction，模型先把 `request_admission` 显式提升成一个 extend allocation intent，再在该 intent 中指定：

```text
batch_size = 1
```

这不是消费 source `capacity_request`，而是把当前 manual workload 的同步单请求形态写成可审计的模型合同。对应的 paged extend allocation budget 为：

```text
requested_tokens = extend_num_tokens + batch_size * target_page_size
requested_pages = ceil(requested_tokens / target_page_size)
```

其中当前显式指定：

```text
batch_size = 1
```

模型落地时还有一个额外收紧条件：

```text
target radix lookup 已覆盖 request path 时，不触发该临时 allocation pressure。
```

原因是 full-hit admission 不需要新增 paged extend KV 分配。如果在 full-hit 路径上仍强行套用 `token_count + page_size`，会把 SGLang
不会执行的 allocator pressure 注入模型，导致 L1 victim set 偏离。

该妥协只用于当前 final-state alignment 的阶段性推进，不能解释为完整 batch-level allocator model。它的价值是先把 batch
语义建成一个单独层次，后续新增 `extend_allocation_intent` invariant 后，只替换 intent 来源，不再改散落的 request admission 逻辑。

### 风险

该妥协在以下场景中可能失效：

- 一个 `ScheduleBatch` 中包含多个 request；
- chunked prefill 将多个 request 或 continuation 合并进同一 forward batch；
- decode / prefill overlap 改变实际 batch grouping；
- cross-config prediction 中 HiCache 配置变化间接影响 scheduler timing 或 request grouping；
- allocator pressure 需要按 batch-level 一次性触发，而模型按 per-request 多次触发，导致 victim order 偏离。

即使总 requested page 数相近，多次 per-request eviction 与一次 batch-level eviction 也可能因为中间 radix touch、lock/ref、host backup
状态变化而产生不同 victim set。

### 正确方向

中长期应新增或调整一个 batch-level invariant role，例如：

```text
extend_allocation_intent
```

推荐采集边界：

```text
sglang.srt.mem_cache.common.alloc_for_extend(batch)
```

或更底层：

```text
alloc_paged_token_slots_extend(...)
```

该 role 应记录 allocator intent，而不是 source eviction 结果。建议字段包括：

```text
allocation_kind = extend
cache_scope
seq_no
batch_size
extend_num_tokens
source_page_size
policy_params
```

如果需要和 request 关联，可额外保留 request ids 或 request token span，但不得携带 source victim、source evicted count 或 actual movement
result。

模型侧应基于 target config 重算：

```text
requested_tokens = extend_num_tokens + batch_size * target_page_size
requested_pages = ceil(requested_tokens / target_page_size)
```

然后交给 target capacity model 判断是否需要 eviction：

```text
if target_free_pages < requested_pages:
    enforce_device_capacity(requested_pages)
```

victim 仍由 modeled target radix tree、LRU/touch order、lock/ref eligibility 和 write policy 决定。

### 当前状态

该妥协已在 `src/modeling/trace_graph/src/modules/hicache/hicache_model.cpp` 的 `request_admission` 链路中落地：

- `HiCacheResolvedPolicyState.extend_allocation_batch_size` 当前显式固定为 `1`；
- `request_admission` 先构造 `ExtendAllocationIntent`，再从 intent 派生 allocator pressure 和 active reservation；
- 当 target radix 中存在缺失的 device page 时，模型按 `batch_size=1` 的 extend allocation intent 推导 paged extend allocation pressure；
- capacity cleanup 的触发条件对齐 SGLang allocator：只有当前 free page 不足以满足本次 allocation request 时才触发 eviction；
- 一旦触发 eviction，清理预算使用本次 allocation request 的 page 数，而不是只清理 free-space deficit；
- host allocation fallback 采用同一源码语义：prefetch 失败后按 `prefetch_length` 清理，`write_backup` 失败后按
  `len(node.value)` 清理，不再保留“只清理 host allocation deficit”的旧分支；
- active request reservation 使用不含 conservative extra page 的实际占用页数，避免把临时 over-estimate 长期保留到后续 capacity check；
- policy decision summary 会输出 `batch_size`、`extend_tokens`、`requested_tokens` 和 `allocated_pages`，用于审计当前 batch 合同；
- source trace 中的 `capacity_request` 仍不作为 model input，只作为解释该限制的诊断证据。

## HCSV-LIMIT-002: loadback intent / mem_quota 尚未作为 invariant 输入

### 问题

HiRadix 的 host-to-device loadback 不是单纯由“radix 上存在 host-visible prefix”决定。SGLang 链路是：

```text
match_prefix()
  -> MatchResult.host_hit_length / best_match_node
  -> scheduler add_one_req()
  -> if req.host_hit_length > 0:
       init_load_back(best_match_node, host_hit_length, mem_quota)
```

`init_load_back()` 内部还会检查 `load_back_threshold`、`mem_quota`，并先尝试 `mem_pool_device_allocator.alloc(len(host_indices))`；
只有 allocation 失败且 loadback 本身成立时，才会调用 `tree_cache.evict(EvictParams(num_tokens=len(host_indices)))`。

当前 C++ model 没有 `host_hit_length`、`best_match_node`、`mem_quota` 或明确的 loadback intent invariant。另一方面，write-back ACK
时序目前被折叠为同步结果，model 可能比真实 SGLang 更早看到 host-visible prefix。如果直接用 modeled host-visible prefix 触发
device eviction，会把 source 中没有发生的 loadback capacity pressure 注入 L1 victim order。

### 当前妥协方案

`request_bound_match_anchor` 只做 target lookup / touch。loadback 只在当前 modeled allocator 的 `free_pages` 足够时，作为
opportunistic promotion 消费 free pages；如果需要 device eviction 才能 loadback，则记录：

```text
policy_area=device_allocator
policy_name=loadback_allocation
decision=skip_loadback_eviction_without_intent
```

这不是消费 source `load_start_observed` 或 source movement，也不是认为 SGLang 不会 loadback；它只是承认当前 normal invariant
不足以安全地产生 loadback eviction。

### 风险

该妥协会低估真实 loadback 成功后带来的 L1 residency，并可能影响 transition exactness。它对 final-state alignment 的价值是避免
由同步 write-back 可见性放大的假阳性 device eviction。

### 正确方向

后续应新增 target-independent loadback intent 或 scheduler allocation intent，至少包含：

```text
request identity
cache_scope
host_hit_length
best_match_node / host-hit page span 的 target-independent 表达
mem_quota 或等价 admission quota
loadback 是否进入 allocator allocation path
```

只有有了这类输入，model 才能按 SGLang 源码完整表达 loadback allocation、loadback-triggered eviction 和 subsequent prefix
rewrite。

## HCSV-LIMIT-003: transition exactness 已有验证链路，但中间状态仍非全量 exact

### 问题

当前新增的 `scripts/internal/hicache_transition_exactness.py` 已能把 HiCache transition exactness 拆成独立阶段：

```text
model-self-check
extract-target-oracle
compare-self / compare-cross
compare-cross-matrix
operation-intent
```

这条链路证明了模型侧 transition 账本可以按 stable active state replay，并能从 target full Python probe 中抽取 validation-only
observed state delta。但这不等价于所有中间 transition 都已和 SGLang 完全 exact。

### 当前证据

在当前 5 config x 3 manual input 的 75 个 prediction 上，矩阵级 transition exactness 结果为：

```text
T0 final-state exact: 75 / 75
model/target oracle ready: 75 / 75
T1 transition-count exact: 25 / 75
T2 page-lifecycle multiset exact: 25 / 75
```

通过集中在：

```text
c4_wb_timeout_p64_low_capacity: 15 / 15
c0_wt_timeout_p128_balanced:
  manual_phased_fast 5 / 5
  manual_pressure_prefetch 5 / 5
```

未通过的主要 mismatch family：

- `c1_wts_wait_p128_low_l1` target 下存在大量 `mark_dirty` / `clear_dirty` observed delta，但模型的同步 write-through-selective
  语义没有逐步复现这些 transient dirty oscillation；
- `c2_wb_best_effort_p64_low_l1` target 下存在 dirty transition 与 evicted transition 交错差异，final state 对齐但中间
  write-back / eviction exactness 未对齐；
- `c3_wt_best_effort_p32_low_host` target 下有 `add/remove_l1`、`add/remove_l2`、`mark/clear_backuped` 和 `mark/clear_evicted`
  mismatch，集中体现 low-host capacity 下 host cleanup、host-visible prefix 和 loadback/eviction transient 的顺序差异；
- `manual_deeper_pressure_prefetch` 的 `c0`/`c1` target 即使 self prediction 也存在 `mark_evicted` / `clear_evicted`
  transient 差异，说明更深 prefetch pressure 下模型和 SGLang 的 evicted marker oscillation 并非逐步 exact；
- raw `operation-intent` scaffold 仍直接面对 full probe 的 source_actual 粒度，observed evidence 中包含大量
  maintenance、lookup、config、lock/ref 事件，不能直接作为 DAG patch intent。

### 当前妥协方案

transition exactness 的当前 hard gate 分层如下：

```text
T0:
  final-state exact，继续作为 state model 通过条件。

model-self-check:
  stable active state sets 必须能 replay；
  page_hit_counts 只作为诊断 metadata；
  locked_pages 的 strict replay mismatch 只作为 advisory。

T1/T2:
  比较 strip_scope 后的 global-union state delta；
  暂不比较 locked_pages transient，因为 lock/ref inc/dec 来自 source_actual evidence，
  按约束不能作为 state model input。

operation-intent:
  仅作为 DAG patch 前的 scaffold，不作为 exact pass 条件。
```

这不是把 mismatch 视为通过，而是先把“验证链路是否可用”和“模型是否逐步 exact”分开。当前 T1/T2 mismatch 不能用小的 Python
比较器补丁修成语义通过；需要回到 SGLang 源码和 full probe 逐 trace 对齐具体机制。

### 风险

如果直接用当前 transition trace 做 DAG patch，会有以下风险：

- final state 正确，但中间 dirty / evicted / backuped / host-visible 变化次数或顺序错误；
- device eviction 与 host cleanup 的 raw operation 数量不准确，导致 DAG 中 memcpy / storage IO / cleanup node 的增删不可靠；
- operation intent 未聚合前，raw source_actual 会把 maintenance polling 和 diagnostic evidence 当成真实 patch intent；
- low-host / deeper prefetch 场景下，transient mismatch 可能对应真实物理操作差异，而不是纯 state marker 差异。

### 正确方向

后续应按以下顺序收敛：

1. 针对 `c1` 的 dirty oscillation 做逐 trace 对齐，区分 write-through-selective 的真实 dirty marker 与 snapshot 包围差分重复计数；
2. 针对 `c2` 的 write-back / eviction 交错，把同步 ACK 近似替换成可验证的 operation lifecycle 边界；
3. 针对 `c3` low-host，沿 host cleanup victim、host-visible prefix、loadback promotion 和 host eviction 顺序做 source-level 对齐；
4. 针对 deeper prefetch pressure，检查 prefetch apply / revoke / timeout 期间 evicted marker 的 node-level oscillation；
5. 把 operation intent 聚合到 stable cache operation 层，再把该层作为 DAG patch 输入，而不是直接消费 raw transition rows。

### 当前状态

`scripts/internal/hicache_transition_exactness.py` 已落地为 active 只读验证入口：

- `model-self-check` 输出 `model_transition_self_check.json`；
- `extract-target-oracle` 输出 `observed_target_transition_trace.json`；
- `compare-self` / `compare-cross` 输出 per-prediction transition exactness JSON；
- `compare-cross-matrix` 输出 `transition_exactness_cross_matrix.json`；
- `operation-intent` 输出 `operation_intent_exactness.json` scaffold。

该脚本不生成 `model_input=true` 事件，不修改 profiling trace，不替代 final-state validation。

## HCSV-LIMIT-004: host/storage 异步控制边界仍是 coarse approximation

### 问题

当前 5x3 manual matrix 的 final state 已经全部对齐，但其中若干 host/storage 机制仍使用 target-control 边界近似，而不是完整复现
SGLang 后台线程、ACK、release queue drain 和 scheduler progress 的精确时序。

这些近似包括：

- prefetch revoke / timeout incomplete 后，host reservation 先进入 deferred release，而不是立即从 host budget 消失；
- completed prefetch 在 request reuse boundary 上 materialize / release，用来近似 `check_prefetch_progress()` 已经使 request 继续执行；
- write-through backup ACK 前持有普通 lock ref，并在下一条 target control fact 开始时 drain；最后一条 fact 之后的 pending ACK 可以保留到 final；
- write-back ACK 时序仍折叠为同步 completion，主要保留 dirty clear、host-visible、storage-readable 和 device free 的结果语义。

这些机制都来自 SGLang 源码语义和逐 trace 诊断，但它们还不是完整的 target scheduler replay。

### 当前证据

这些近似关闭了此前的 final-state failure：

- `manual_pressure_prefetch/c2` 中 best-effort prefetch revoke 后立即释放 host reservation，会导致模型少触发一次 SGLang-style
  host cleanup，最终多留 L2/backuped/evicted page；
- `manual_pressure_prefetch/c1`、`manual_deeper_pressure_prefetch/c1` 和 `manual_deeper_pressure_prefetch/c0` 中，旧模型把 active
  prefetch reservation 保留过久，导致后续 host cleanup 比 SGLang 多执行一次；
- `manual_deeper_pressure_prefetch/c1` 中，write-through-selective backup ACK 前的普通 lock ref 没有建模，导致 final
  `locked_pages` 少 11 页。

修复后，当前 15 个 manual self prediction 和 75 个同 input cross prediction 的 final state 均通过。

### 当前妥协方案

模型只用 invariant request/control boundary 和 target config 推进这些近似：

```text
prefetch_decision / prefetch_check_point
request_bound_match_anchor / request_admission reuse boundary
request_lifecycle_anchor
target control fact boundary
```

full Python probe 中的 `prefetch_io_observed`、`prefetch_terminate_observed`、`hicache_flush_write_through_acks_end`、host
eviction result 和 oracle snapshot 只作为诊断证据；它们不进入 normal state mutation。

### 风险

这些近似对 final state 有效，但会影响 transition exactness：

- T1/T2 可能多出或少掉 `mark_dirty` / `clear_dirty`、`mark_evicted` / `clear_evicted`、`add_l2` / `remove_l2` 等 transient；
- background release queue drain 的真实时机可能改变 host cleanup victim；
- ACK 与 request boundary 之间的交错可能改变 locked transition 的数量和顺序；
- loadback / write-back / prefetch 同时发生时，同步折叠可能让 operation order 与 SGLang 不一致。

这也是当前 transition exactness 只有 `25 / 75` 严格通过的主要解释之一。

### 正确方向

中长期应把这些 coarse boundary 拆成 target-independent operation lifecycle facts 或 target scheduler replay：

```text
prefetch_progress_intent
prefetch_apply_boundary
host_mem_release_drain_boundary
write_through_ack_boundary
write_back_ack_boundary
storage_control_drain_boundary
```

这些 facts 必须描述 target-independent boundary 或 intent，不得携带 source victim、source completed page set、source ACK result 或 oracle
状态答案。模型侧再基于 target policy、storage directory、ref ledger 和 capacity index 推导具体 state mutation。

### 当前状态

该限制不阻塞当前 final-state validation，但阻塞 transition exactness 和 DAG patch。后续应优先在 `c1`、`c2`、`c3` 和
`manual_deeper_pressure_prefetch` 上做逐 trace 对齐，确认哪些 mismatch 是可观测性差异，哪些需要新增 invariant boundary。

后续如果发现新的长期建模缺口，应继续按同一结构追加：

```text
问题
当前证据
当前妥协方案
风险
正确方向
当前状态
```
