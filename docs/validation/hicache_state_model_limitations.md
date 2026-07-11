# HiCache 状态模型近似限制记录

本文只记录 HiCache state model 为了做 target-side prediction 而不得不近似的 SGLang 机制。

写入本文的内容需要同时满足：

- 该机制属于 SGLang 运行语义的一部分，而不是一次实验的阶段性结论；
- 当前模型没有足够的 target-independent state-model fact 精确复现它；
- 模型已经用某种投影、折叠或边界近似继续推进 final-state / transition validation；
- 该近似可能影响 cross-config prediction、final state、transition exactness 或后续 DAG patch。

不写入本文的内容包括：

- 单次 profiling / validation run 的通过率流水账；
- transition exactness 脚本、catalog、gate 等验证链路本身的完成状态；
- forced-token capture/replay 这类 validation 输入合同；
- source_actual / oracle snapshot 的诊断证据清单。

## HCSV-LIMIT-001: cache extend batch grouping 仍依赖 source scheduler 近似

### 问题

当前模型已经不再从 per-request admission 反推 device KV allocation pressure，而是消费
`workload_identity/cache_extend_input`。该 fact 来自 `ScheduleBatch.prepare_for_extend` start-phase，携带本轮 batch 的
request id、位置和 accepted fill path 数组，模型据此构造 batch-level `CacheExtendBatchIntent`。

需要保留的近似是：这个 batch grouping 来自 source run 的 scheduler 结果。当前 forced-token manual workload 没有多 request
并发，因此 source / target 的 batch grouping 可以视为同一 workload identity；一旦引入并发、chunked/decode overlap 或跨配置
scheduler 差异，这个假设就可能失效。

SGLang 的 paged KV allocator 预算来自 `ScheduleBatch` 级别：

```text
requested_tokens = extend_num_tokens + len(seq_lens_cpu) * page_size
```

其中 `len(seq_lens_cpu)` 是本次 allocator call 的 batch size。模型现在按同一个 `cache_extend_input` 统一计算这项
batch overhead，不再把每个 request 拆成独立 allocation pressure。

### 当前近似

模型从 `cache_extend_input` 的 batch 数组读取 request 顺序，并按 target page size 重新计算：

```text
requested_tokens = total_extend_tokens + batch_size * target_page_size
requested_pages  = ceil(requested_tokens / target_page_size)
```

每个 request 的真实新增 page 仍按自身 target prefix / committed prefix 计算，batch-level requested pages 只用于 allocator
pressure 和 eviction gate。

代码锚点：

- `src/modeling/trace_graph/src/modules/hicache/model/request_model.cpp` 的
  `apply_cache_extend_input()` 从 batch paths 推导 `CacheExtendBatchIntent`；
- `third_party/sglang/python/sglang/srt/mem_cache/common.py` 的
  `alloc_paged_token_slots_extend()` 使用 batch-level `len(seq_lens_cpu)`。

### 为什么难以精确

要精确支持并发 workload，需要 target-independent 的 forced batch plan 或 scheduler intent，证明 source / target 在同一 logical
round 中使用同一 request group。直接消费 source `capacity_request` 或 eviction result 会把 source config 下的 victim /
capacity 结果泄漏进 target model。

### 风险

该近似在以下场景可能失效：

- source / target 的 `ScheduleBatch` grouping 因配置差异而不同；
- chunked prefill、decode / prefill overlap 或 scheduler timing 改变 batch grouping；
- cross-config prediction 中 HiCache 配置变化间接改变可运行 request 集合；
- forced-token 只稳定 token path，没有同时稳定 scheduler batch plan。

### 收敛方向

如果后续进入并发 workload，需要新增 forced batch plan 或 target-independent scheduler batch identity，至少描述：

```text
cache_scope
seq_no
batch_id / scheduler_round_id
request_ids
request_positions
batch_kind
accepted fill spans
```
模型侧仍按 target page size 重算 requested pages，并由 target capacity index 自己选择 victim。

## HCSV-LIMIT-002: loadback intent、mem_quota 与异步完成边界尚未完整建模

### 问题

HiRadix 的 L2(host) -> L1(device) loadback 不是简单的“radix 上有 host-visible prefix 就立刻回到 device”。
真实链路包含：

```text
match_prefix()
  -> MatchResult.host_hit_length / best_match_node
  -> scheduler add_one_req()
  -> init_load_back(best_match_node, host_hit_length, mem_quota)
  -> load_back() threshold / mem_quota / allocator / possible eviction
  -> async loading_check() release refs
```

当前 state-model fact 没有完整提供 `host_hit_length`、`best_match_node`、`mem_quota`、loadback allocator intent 或 loadback
completion boundary。

### 当前近似

模型在 `cache_lookup_input` 上只做 opportunistic loadback：

- 只使用 modeled host-visible prefix，不使用 storage-readable backend-only hash；
- 只有当前 device allocator free pages 足够时才同步 materialize 到 L1；
- 如果需要触发 device eviction 才能 loadback，则记录 `skip_loadback_eviction_without_intent`，不主动发明这次 eviction；
- loadback operation 的 enqueue、commit 和 ref release 在同一个模型边界内同步折叠。

代码锚点：

- `src/modeling/trace_graph/src/modules/hicache/model/request_model.cpp` 的
  `apply_cache_lookup_input()`；
- `third_party/sglang/python/sglang/srt/mem_cache/hiradix_cache.py` 的
  `load_back()` / `init_load_back()`；
- `third_party/sglang/python/sglang/srt/managers/schedule_policy.py` 的
  `req.needs_host_load_back()` 调用路径。

### 为什么难以精确

loadback 是否发生、是否允许 eviction、能 load 多少，不只取决于 radix state，还取决于 scheduler 当时传入的 host-hit 结果和
mem quota。没有这些 target-independent intent 时，模型如果仅凭自身 host-visible state 触发 eviction，会把同步折叠后的
host visibility 放大成真实 SGLang 未必执行的 device pressure。

### 风险

该近似可能：

- 低估真实 loadback 后的 L1 residency；
- 或者为了避免误触发 eviction，在 transition 层少掉真实 loadback operation；
- 在 loadback、writeback、prefetch 同时发生时改变 L1 victim order。

### 收敛方向

新增 loadback intent / boundary fact，至少包括：

```text
cache_scope
request_id
host_hit_length
best_match_node 的 target-independent 表达
mem_quota
loadback_threshold
loadback 是否进入 allocator path
```

模型拿到该 intent 后，才能按 target config 重算 loadback allocation、possible eviction 和 async completion lifecycle。

## HCSV-LIMIT-003: storage control queue drain 仍是 request-local 边界近似

### 问题

SGLang 的 host/storage control cleanup 是两段式：

```text
append_host_mem_release()
  -> 只把 host page 放入 host_mem_release_queue

drain_storage_control_queues()
  -> 同步决定本轮 drain 数量
  -> FIFO drain prefetch_revoke_queue / ack_backup_queue / host_mem_release_queue
  -> 真正 mem_pool_host.free()
```

正常 scheduler path 下，每个 rank 都有本地队列。`drain_storage_control_queues()` 读取各队列 `qsize()`，在 TP /
attention group 上做 `all_reduce(MIN)`，然后每个 rank 只 drain 本地队列前 `n` 个元素。shutdown / detach 的 local cleanup
才会 drain 到空。

### 当前近似

profiling 合同不采集 `drain_storage_control_queues()` runtime checkpoint。该函数属于 source scheduler round
边界，跨配置不能稳定映射到 target request timeline；profile quality、transition validator 和 C++ mutation path 都不消费
这个 source boundary。

当前用 async table 的 `reserved_host_pages` 表示 pending host release，并把释放时机建成 target-derived request-local
三段近似：

- revoked / late / suppressed / applied prefetch 仍可能占用 `reserved_host_pages`；
- `cache_extend_input` 进入前先 drain 同 request 已经存在的 pending release，模拟上一轮 scheduler drain 已经让 release
  对本轮 allocation 可见的情况；
- `cache_extend_input` 被视为同 request active prefetch 的 target-derived terminal boundary，用于结算 ready / revoked / late /
  suppressed；
- 对 best-effort below-threshold revoke，若模型认为后台 storage query 已经早于本次 `cache_extend_input` 可见，则在 cache
  extend side effect 前释放 reservation；
- 其它同 request terminal prefetch 的 pending reservation 在 `cache_extend_input` side effect 后做 post-extend release drain；
- `finalize()` 只兜底释放没有后续 cache extend 的残留 reservation；
- 模型不读取 source queue snapshot、source page identity 或 oracle final state 来决定释放哪些 page。

2026-07-06 forced replay suite 上的 unified workflow 75 格矩阵已经在当前 profiling 合同下达到 self/cross
final-state `75 / 75` exact 和 transition `75 / 75` exact。这说明当前 5x3 manual matrix 覆盖了该边界的已知失败形态，
但不说明 rank-synced FIFO release queue 已被精确复现。

当前 active 结果目录：

```text
data/profile_runs/sglang/20260706_020716_profiling_hicache_dag_analysis_forced_replay/modeling/modeling_workflow_full_refactor_validation_20260711
```

代码锚点：

- `src/modeling/trace_graph/src/modules/hicache/runtime/async_state.cpp` 的
  `release_prefetch_pending_host_pages_for_request()`；
- `src/modeling/trace_graph/src/modules/hicache/model/request_model.cpp` 的
  `apply_cache_extend_input()` pre-existing / pre-cache-extend / post-cache-extend drain；
- `src/modeling/trace_graph/src/modules/hicache/model/finalizer.cpp` 的 pending release finalize fallback；
- `third_party/sglang/python/sglang/srt/mem_cache/hiradix_cache.py` 的
  `_drain_storage_control_queues_impl()` / `drain_storage_control_queues()`；
- `third_party/sglang/python/sglang/srt/managers/cache_controller.py` 的
  `append_host_mem_release()`。

### 为什么难以精确

精确复现需要知道每个 rank 的 queue length、同步后的 MIN drain count、每个队列的 FIFO 前缀，以及 scheduler 何时调用
`check_hicache_events()`。这些信息不能用 source victim 或 oracle final state 代替；否则 cross-config target prediction
会被 source runtime 结果污染。

### 风险

request-local drain 不是 SGLang rank-synced FIFO queue 的精确复现。它保守保证同 request cache extend 附近能看到
terminal prefetch 的 host pressure，但不能表达其它 request、其它 rank 或后续 scheduler round 对同一 release queue 的精确交错。
release drain 过早会让 host budget 太早变空，可能少触发一次 host cleanup；release drain 过晚会让 host budget 继续被占用，
可能多触发一次 host cleanup。两者都会改变 L2/backuped/evicted page set 和 host victim order。

### 收敛方向

如果后续要支持 TP/rank exactness，需要新增真正 target-independent 的 scheduler / release-queue intent，显式描述 rank-local
queue、rank-synced FIFO drain count 或 scheduler batch round。不能把 source run 的 `drain_storage_control_queues()`
重新包装成跨配置 profiling checkpoint。

## HCSV-LIMIT-004: best-effort prefetch revoke 可见性使用 worker-ready 时间投影

### 问题

SGLang `best_effort` prefetch below threshold 的关键分歧不在 lookup，而在后台线程和 scheduler drain 谁先让 revoke
对本次 cache extend 可见：

```text
prefetch_from_storage()
  -> enqueue prefetch operation / reserve host pages

prefetch_thread_func()
  -> storage hit query
  -> all-reduce MIN
  -> hit < threshold 时把 request 放入 prefetch_revoke_queue
  -> append_host_mem_release()

scheduler path
  -> check_hicache_events() / drain_storage_control_queues()
  -> check_prefetch_progress()
  -> cache_extend_input
```

如果后台线程足够早完成 below-threshold 判定，后一次 scheduler drain 可能在 cache extend 前释放 host reservation；
否则 release 只能在同 request cache extend 后才对模型可见。

### 当前近似

C++ state model 仍只消费 `cache_lookup_input`、`cache_extend_input`、`cache_lifecycle_commit` 和
`prefetch_candidate_anchor` 四类 state facts。不新增 source runtime checkpoint，不消费 source actual、oracle state、
observed gate 或 Python probe 的最终答案。

在 `prefetch_candidate_anchor` 上，模型用 target-derived 信息估计后台 worker 的 storage query 可见时刻：

- 以 modeled storage readable prefix 计算 below-threshold 首个 miss 之前需要查询的 page 数；
- query page 数按 SGLang storage batch 粒度折叠，batch size 当前为 `128` pages，并被 planned prefetch pages 截断；
- 每个 `cache_scope` 维护一个串行 `prefetch_worker_available_ts`；
- ready time 近似为 `max(candidate_anchor.ts, prefetch_worker_available_ts) + query_duration`；
- `query_duration = 2000us + 1250us * query_pages`，当 `query_pages > 16` 时再加 `2500us`；
- 在 `cache_extend_input` 上，如果 `storage_query_ready_ts + 2500us <= cache_extend_input.ts`，则把 below-threshold
  host release 视为 pre-extend drain 可见；否则走 post-extend drain。

非 `best_effort` 的 below-threshold revoke 仍按原语义在 cache extend 前可见；这个 worker-ready 投影只特化
`best_effort` 的 A/B 分岔。

代码锚点：

- `src/modeling/trace_graph/src/modules/hicache/model/prefetch_model.cpp` 的
  `prefetch_storage_query_pages()`、`estimate_prefetch_storage_query_duration()`、
  `prefetch_release_visible_before_cache_extend()`、`apply_prefetch_candidate_anchor()` 和
  `settle_prefetch_before_cache_extend()`；
- `src/modeling/trace_graph/include/markov/trace_graph/modules/hicache/model/state.hpp` 的
  `ScopedState::prefetch_worker_available_ts`；
- `src/modeling/trace_graph/include/markov/trace_graph/modules/hicache/runtime/async_state.hpp` 的
  `HiCachePrefetchOperation::storage_query_ready_ts` 和 `release_before_cache_extend`；
- `third_party/sglang/python/sglang/srt/managers/cache_controller.py` 的 `prefetch_thread_func()`；
- `third_party/sglang/python/sglang/srt/mem_cache/hiradix_cache.py` 的 `prefetch_from_storage()`、
  `check_prefetch_progress()` 和 `drain_storage_control_queues()`。

### 为什么难以精确

精确判断需要知道后台 prefetch thread 的真实排队、storage backend latency、rank all-reduce、queue size snapshot、
以及 scheduler 何时执行 `check_hicache_events()`。这些都是 source runtime timing，不能直接变成 cross-config
state-model 输入。只看 lookup 也不够，因为改变状态的是后续 cache extend 边界附近的 revoke visibility。

### 风险

该近似依赖固定的 storage query latency 常数和每 scope 单 worker 假设。如果 storage backend 延迟、prefetch candidate
大小分布、scheduler round 间隔、rank 同步或后台线程并发度变化，pre-extend / post-extend drain 判定可能翻转。翻转后会改变
host budget、cleanup victim order、L2 host-visible page set 和 transition multiset。

### 收敛方向

长期应引入 target-independent 的 scheduler round / storage-control drain intent，或把 prefetch worker queue 抽象成
可校准的 target operation graph。允许记录的应是可复算边界条件，例如 cache scope、request id、enqueue order、query page
count、scheduler round id 或 drain intent；不应记录 source run 的 actual completion、oracle final state 或 observed gate
答案。

## HCSV-LIMIT-005: prefetch I/O progress 仍是未校准的完成度投影

### 问题

Storage prefetch 的真实 lifecycle 包含后台 storage hit query、异步 page get、`completed_tokens` 增长、terminate/revoke、
host-visible materialization 和 release queue enqueue。`storage readable prefix` 只说明 backend 有连续命中，不等价于这些 page
已经完成 I/O 并进入 host memory。

### 当前近似

模型的 I/O progress 估计当前是 zero-progress placeholder：

```text
estimate_prefetch_io_progress() -> completed_pages = []
```

在不同 prefetch policy 下模型再做策略性折叠：

- `best_effort`：可以立刻 terminate，但 completed prefix 使用 zero-progress，通常不会 materialize host prefix；
- `wait_complete`：在需要完整 completion 的 boundary 上把 completed prefix 近似为 storage hit prefix；
- `timeout`：cache-extend terminal boundary 未超时时把 completed prefix 近似为 storage hit prefix；已超时时只暴露
  zero-progress 并取消；
- completed prefetch 的 host insertion 当前只在模型判定可 apply 的 `cache_extend_input` terminal boundary 同步发生；如果 completed prefix
  不可知，模型会保守保留 pending release / suppressed / late 状态，而不是再用 request reuse 边界补齐 host-visible page。

跨 rank / cache scope 的真实 backend I/O 仍可能存在更细的阶段边界：例如先整体完成 `storage -> host` materialize，再在后续
request reuse / lock 边界推进 `host -> L1/GPU` loadback。当前模型只保证在现有 fact 边界上做 target-derived 折叠，不承诺重放
后台线程和 rank 同步的真实时间轴。

代码锚点：

- `src/modeling/trace_graph/src/modules/hicache/model/prefetch_model.cpp` 的
  `estimate_prefetch_io_progress()`、`estimate_prefetch_progress()`、`apply_prefetch_ready()`、
  `settle_prefetch_before_cache_extend()`；
- `third_party/sglang/python/sglang/srt/managers/cache_controller.py` 的
  `prefetch_thread_func()`、`terminate_prefetch()`、`_page_get_zero_copy()`；
- `third_party/sglang/python/sglang/srt/mem_cache/hiradix_cache.py` 的
  `prefetch_from_storage()` / `check_prefetch_progress()`。

### 为什么难以精确

真实 completed prefix 取决于后台线程、storage backend latency、rank synchronization 和 terminate 时刻。直接把 source
`completed_tokens` 当 state-model input 会让 target prediction 依赖 source runtime timing；完全不记录 progress 又只能做保守
projection。

### 风险

该近似会影响：

- prefetch 是否 materialize 成 L2 host-visible prefix；
- unused host reservation 何时进入 pending release；
- 后续 request match / loadback / host cleanup 的 victim order；
- transition exactness 中 `prefetch_ready`、`apply_prefetch_host_visibility`、`add_l2` 等操作数量和顺序；
- 多 rank/cache scope 的 union transition timeline 中，`storage -> host` 和 `host -> L1/GPU` 的阶段边界如果被折叠到
  不同粒度，仍可能改变 marker 顺序。2026-07-06/07 full matrix 已关闭当前 manual matrix 的该类 mismatch，但不能证明后台 I/O
  exactness。

### 收敛方向

需要设计 target-independent prefetch progress fact 或校准模型。可选方向：

```text
fact.role = prefetch_progress_boundary
```

字段应避免携带 source victim，但可以考虑记录可复算的边界条件：

```text
cache_scope
request_id
progress boundary kind
elapsed time / enqueue timestamp
storage hit prefix length
target-independent completed prefix policy
```

如果要 exact 复现 backend I/O，则需要把 storage backend completion 抽象成独立可重放的 intent，而不是在 state model 中继续猜测。
同时需要保留跨 scope 的阶段边界：先在 prefetch progress boundary 上整体 materialize host-visible prefix，再在后续 request reuse /
loadback boundary 上整体推进 L1/GPU materialization，避免 `scope A: mark+clear`、`scope B: mark+clear` 这种伪 transition。

## HCSV-LIMIT-006: write-through backup ACK 与普通 lock lifetime 是边界近似

### 问题

在 write-through / write-through-selective 下，SGLang `write_backup()` 会先把 device KV 写到 host，并对非 write-back backup
持有普通 lock ref。之后 `writing_check()` 轮询 async CPU write ACK，在 rank 间同步可处理 ACK 数量，再调用
`_finish_write_through_ack()`：

```text
publish host backup
optional write_backup_storage()
release ordinary lock ref
```

这条链路的真实释放时刻由 async finish event、ack queue、all-reduce MIN 和 scheduler polling 决定。

### 当前近似

模型在 `commit_host_backup()` 中同步完成 host/storage 可见性，然后用 `PendingWriteThroughBackup` 暂存 ordinary lock：

- backup 结果立即表现为 host-visible / storage-readable；
- ordinary lock ref 不立即释放；
- `apply_fact()` 在下一条 state-model fact 开始前调用 `drain_write_through_backup_refs()`；
- 如果 trace 尾部没有下一条 state-model fact，`finalize()` 会在 target finalize boundary 调用
  `drain_write_through_backup_refs()`，确保 final state 不保留 write-through ACK 的临时 ordinary lock。

代码锚点：

- `src/modeling/trace_graph/include/markov/trace_graph/modules/hicache/model/state.hpp` 的
  `PendingWriteThroughBackup`；
- `src/modeling/trace_graph/src/modules/hicache/model/writeback_model.cpp` 的
  `commit_host_backup()`、`hold_write_through_backup_ref()`、`drain_write_through_backup_refs()`；
- `src/modeling/trace_graph/src/modules/hicache/model/state.cpp` 的
  `apply_fact()`；
- `src/modeling/trace_graph/src/modules/hicache/model/finalizer.cpp` 的
  `finalize()`；
- `third_party/sglang/python/sglang/srt/mem_cache/hiradix_cache.py` 的
  `write_backup()`、`writing_check()`、`_finish_write_through_ack()`。

### 为什么难以精确

要 exact 建模，需要知道 async write event 何时 query 为完成、ack queue 的 FIFO 前缀、rank 同步后的 finish count，以及 tree
split 后 pending ACK publish nodes 如何映射回 target node。当前 state-model fact 只描述 cache lookup / extend / lifecycle / prefetch candidate boundary，
不足以还原 ACK polling timeline。

### 风险

ordinary lock 释放太早会让节点过早变成 evictable；释放太晚会导致 host/device victim 被保护过久。当前 final boundary
会收敛尾部 ACK，避免 final `locked_pages` 残留；但中间 timeline 仍可能因为 ACK polling 近似影响 subsequent eviction 的
victim order。

### 收敛方向

若要进一步提升 transition timeline exactness，需要新增 write-through ACK boundary，例如：

建议新增 workload-independent ACK boundary，不携带 source final answer：

```text
cache_scope
seq_no
ack_count
operation ids / stable node span
release_lock = true
```

模型侧仍应基于 target tree/ref ledger 自己释放对应 owner。

## HCSV-LIMIT-007: write-back dirty eviction ACK 时序被同步折叠

### 问题

write-back dirty eviction 的真实流程不是普通 `remove L1`：

```text
dirty device node
  -> write_backup(write_back=True)
  -> blocking writing_check(write_back=True)
  -> host backup confirmed
  -> evict backuped device value
```

它保留了后台 write queue / ACK 语义，只是在 eviction path 上阻塞等待完成。

### 当前近似

模型在 `evict_device_node()` 中把这条链路折叠到同一个 state-model boundary：

```text
enqueue_writeback
commit_host_backup(storage_readable=true)
set writeback committed
release writeback ref
evict_l1_node
```

也就是说，模型保留 dirty clear、host-visible、storage-readable 和 device free 的结果语义，但不模拟 ACK queue 的真实时间轴。
模型 summary 也会在出现 dirty eviction 时输出：

```text
write_back eviction used synchronous modeled writeback; ack timing is intentionally not modeled yet.
```

代码锚点：

- `src/modeling/trace_graph/src/modules/hicache/model/host_storage_model.cpp` 的
  `evict_device_node()`；
- `src/modeling/trace_graph/src/modules/hicache/model/apply_model.cpp` 的
  `apply_hicache_model()` warning；
- `third_party/sglang/python/sglang/srt/mem_cache/hiradix_cache.py` 的
  `evict()` 中 `write_back=True` path 和 `writing_check(write_back=True)`。

### 为什么难以精确

write-back eviction 处在 capacity cleanup 内部。要 exact 表达它，需要把 eviction loop、writeback enqueue、blocking ACK、host
backup commit、device free 分成可比较的 target operation lifecycle；当前 state-model fact 只提供触发 allocation pressure 的外层
boundary。

### 风险

同步折叠通常能维持 final state，但会改变中间 transition 顺序。与 host release queue、loadback 或其它 write-through ACK 交错时，
也可能影响后续 victim eligibility。

### 收敛方向

新增 write-back ACK / blocking writeback lifecycle boundary，或把 capacity cleanup 建成稳定 operation graph：

```text
device_eviction_intent
write_back_enqueue
write_back_ack_boundary
device_eviction_commit
```

在此之前，write-back 路径的 transition exactness 只能解释为结果语义近似，而不是完整 async timeline exact。

## HCSV-LIMIT-008: host allocator 仍是 capacity/reservation 投影

### 问题

SGLang host pool 是真实 allocator：prefetch 和 write backup 都会申请 host pages，申请失败后触发 `evict_host()`，prefetch
还可能按当前 `available_size()` 截断到 threshold-sized prefix。释放则可能来自 storage control release queue 或其它 cleanup path。

当前模型没有独立 host allocator/free-list，只从 canonical radix tree、capacity index 和 async reservation 推导：

```text
occupied_host_pages + reserved_host_pages <= l2_capacity_pages
```

### 当前近似

`request_host_allocation()` 只做 count-level 判断：

- 不直接 drain source storage-control release queue；pending prefetch host release 由 request-local pre-existing /
  worker-ready pre-extend / post-extend drain 推进；
- 按 requested pages 执行 modeled host cleanup；
- 用 `occupied_host_pages + reserved_host_pages` 计算 available pages；
- write backup 必须完整接受；
- prefetch 可以在满足 threshold 的前提下截断；
- 不建模 host pool index、fragmentation、extra host pools 或真实 `mem_pool_host.alloc()` 返回的 tensor。

代码锚点：

- `src/modeling/trace_graph/include/markov/trace_graph/modules/hicache/model/state.hpp` 的
  `HostAllocationResult` 注释；
- `src/modeling/trace_graph/src/modules/hicache/model/host_storage_model.cpp` 的
  `request_host_allocation()`、`enforce_host_capacity()`；
- `src/modeling/trace_graph/src/modules/hicache/model/prefetch_model.cpp` 的
  `apply_prefetch_candidate_anchor()`；
- `src/modeling/trace_graph/src/modules/hicache/model/writeback_model.cpp` 的
  `commit_host_backup()`；
- `third_party/sglang/python/sglang/srt/mem_cache/hiradix_cache.py` 的
  `prefetch_from_storage()`、`write_backup()`、`evict_host()`。

### 为什么难以精确

真实 host allocator state 不只是一组 radix residency page。它还包含 allocator free-list、pending release queue、可能的 extra pool、
以及后台 cleanup 已经归还但模型还没看到的 host indices。把这些全部纳入模型需要新增 host allocator ledger 和更精细的
storage-control queue 合同；直接用 source pool snapshot 又会把 source runtime timing 注入 target prediction。

### 风险

count-level 投影可能在以下场景偏离：

- host pool 有 pending release 但尚未 drain；
- prefetch allocation 因 available_size 截断；
- write backup 与 prefetch 同时竞争 host pages；
- host cleanup victim 顺序依赖真实 allocator / queue drain 时刻。

### 收敛方向

中长期应把 host allocator 从 capacity index 中拆成独立 ledger：

```text
host_allocator_capacity
host_allocator_free_pages
host_allocator_reserved_pages
host_release_queue
host_allocation_intent
```

当前 profiling 合同不采集 source storage-control boundary。下一步如果要继续收敛，需要新增 target-derived host allocator
ledger、release queue 和 rank-synced release count 合同，而不是在 allocation path 上重新加入 source scheduler boundary
或模型自选 scope-level drain。

## 维护约定

新增限制时继续使用以下结构：

```text
问题
当前近似
为什么难以精确
风险
收敛方向
```

如果某条内容只是“某个 run 当前通过/不通过”或“某个脚本已经存在”，不要写入本文；应放到对应的 validation run 记录或临时诊断文档。
