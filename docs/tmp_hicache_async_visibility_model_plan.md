# HiCache Async Visibility Model 临时方案

创建时间：2026-06-12

状态：临时文档。本文记录 `host/prefetch/storage` 剩余 diff 的当前判断和下一轮 async model 方向；
待后端 async visibility 机制定稿并写入主文档后删除。

## 当前判断

最新 target-resource 后端重构后，四向 normal prediction 的 final 形状已经收敛到：

| prediction | result |
| --- | --- |
| S1A self | final match |
| S1B -> S1A | final match |
| S1B self | L1/dirty/locked match；L2/backuped/evicted `56/55`，missing 13、extra 14 |
| S1A -> S1B | 与 S1B self 同形 |

因此现在需要继续处理的不是全部五类 boundary。按之前 `4+1` 口径：

- `lock_ref_transient_boundary`：主要影响 full trace 的 `locked_pages` 暂态；当前 S1B final `locked_pages=0/0`，
  不再是 final mismatch 的主因；
- `target_capacity_pressure_boundary`：device/L1 capacity pressure 已由 target admission、radix leaf group 和
  device lock/ref mechanism 基本闭合；
- `lock_protected_capacity_boundary`：device victim eligibility 对 final L1/dirty/locked 已基本闭合；
- `async_prefetch_storage_completion`：仍需要处理；
- `async_checkpoint_with_source_progress_evidence` / `host_storage_visibility_boundary`：仍需要处理。

后两类应合并理解为一个诊断边界：

```text
host_prefetch_storage_visibility_boundary
```

这个名字适合作为 trace divergence 的分类，但不应直接变成 normal profiling event。它描述的是 async work queue、
completion visibility、host protection 和 storage lifecycle 没有被 target-side model 闭合。

## 为什么不优先新增 profiling 事件

S1B 剩余 diff 只落在 host/L2/storage 侧：

```text
l1_resident_pages: 28 / 28
dirty_pages:       28 / 28
locked_pages:      0 / 0

l2_resident_pages: 56 / 55
backuped_pages:    56 / 55
evicted_pages:     56 / 55
```

附近可见的 source evidence 包括：

- `prefetch_progress_observed`；
- `terminate_prefetch` / prefetch completion；
- storage hit query / load-back；
- `ready_to_load_host_cache`；
- `host_ref_delta_observed`；
- host memory release / maintenance checkpoint。

这些大多是源配置实际执行结果。若把 completed pages、loaded pages、host ref delta 或 storage hit result 接成 normal
input，self-config 可能短期对齐，但 cross-config 会重新污染输入契约。

profiling 端应继续只提供：

- work intent；
- checkpoint structure；
- target-independent scheduling metadata；
- request / radix / policy anchor。

不应提供：

- source completed pages；
- source loaded pages；
- source ready pages；
- source host ref delta；
- source storage hit result；
- source wall-clock completion time。

## Async model 的目标

Async model 不应追求物理线程完成时间完全一致，而应追求 target-side visibility boundary 对齐：

```text
在这个 checkpoint 之前，哪些 async work 对 target state 可见？
在这个 checkpoint 之后，哪些 work 仍为 pending / ready / suppressed / late？
```

对 HiCache state prediction，真正影响 final set 的不是 storage load 在真实系统里第几微秒完成，而是它在同步边界前后是否改变了：

- `l2_resident_pages`；
- `backuped_pages`；
- `evicted_pages`；
- `prefetch_ready_pages`；
- `prefetch_suppressed_pages`；
- host radix / host protection state。

因此原则是：

```text
用 target config + logical checkpoint + async queue order + policy budget 推导 visibility；
不要复用 source wall-clock completion；
不要消费 source completed page result。
```

## 事件完成时机

模型内部可以推进 pending work，但 state visibility 只在明确 checkpoint 上 apply：

```text
prefetch_decision
  enqueue target-derived work intent

prefetch_check_point
  pending -> ready / suppressed / late

request_lifecycle_anchor
  在 request lifecycle 需要 host/storage visibility 时 apply ready work

target maintenance-like checkpoint
  推进 flush / release / host visibility transition
```

这意味着：

```text
completion 计算可以是逻辑连续的；
state set mutation 必须发生在 checkpoint boundary。
```

这样可以避免模型在两个 oracle snapshot 之间提前或延后改动 L2/backuped/evicted。

## Target-side logical async clock

不使用 source timestamp 判断目标完成。`ts` 只保留排序意义。后端应维护一个逻辑 async clock，例如：

```text
scheduler_epoch
checkpoint_index
issued_work_units
completed_work_units
```

这些 clock 由 normal invariant anchors 推进：

- `request_admission`；
- `request_bound_match_anchor`；
- `request_lifecycle_anchor`；
- `prefetch_decision`；
- `prefetch_check_point`。

source_actual / timing_observation 仍只作为诊断 evidence，不参与 normal state mutation。

## Async state 结构

后端应显式拆开 async 状态，避免把 planned / ready / visible / suppressed 混成一个集合：

```text
planned_prefetch_queue
pending_storage_loads
ready_host_pages
suppressed_prefetch_pages
late_prefetch_pages
pending_host_releases
host_protected_pages
```

work item 应保留 deterministic ordering 所需字段：

```text
work_kind
request_key
pages
anchor_path
enqueue_epoch
checkpoint_epoch
priority
best_effort
ignore_eos
work_group_id
```

排序规则必须绑定 target policy，建议顺序为：

1. request / scheduler order；
2. priority；
3. radix path / page order；
4. enqueue epoch；
5. stable page key tie-breaker。

这样当一个 checkpoint 只能完成部分 work 时，模型能稳定选择 target 下更早可见的 work。

## Checkpoint policy

每个 `prefetch_check_point` 给 async queue 一个 target-side completion budget，而不是读取 source completed pages：

```text
completion_budget =
  f(target_config,
    checkpoint_kind,
    prefetch_policy,
    queue_depth,
    pending_work_units,
    elapsed_logical_epochs,
    host_capacity_pressure,
    request_phase)
```

初版规则应先保持清楚：

- `wait_complete`：drain eligible pending work，直到队列空或 target timeout policy 触发；
- `best_effort`：只 apply 已满足 logical budget 的 work，不等待剩余 work；
- `timeout`：超过 logical deadline 的 pending work 进入 late / suppressed；
- `terminate` / final checkpoint：先 apply ready work，再按 target policy 处理剩余 pending；
- disabled / no-prefetch：不 enqueue，或 enqueue 后立即 suppressed。

当前 S1B modeled state 中：

```text
prefetch_planned_pages:    156
prefetch_suppressed_pages: 156
prefetch_ready_pages:      0
```

这说明当前模型几乎把 S1B prefetch 全部压成 suppressed。下一轮应重点判断：缺的是 checkpoint budget、queue ordering，
还是 ready work 没有正确 apply 到 host/L2。

## Host visibility apply

`pending -> ready` 不应直接等价于所有 state mutation。建议拆成两步：

```text
Async completion
  pending -> ready

Host visibility apply
  ready -> l2_resident / backuped / evicted
  enforce host capacity / host protection
```

apply ready work 时需要检查：

- page 是否已经 resident；
- 是否需要插入 host radix；
- backuped / evicted 生命周期是否同步；
- host capacity 是否触发 eviction；
- host eviction victim 是否被 host ref / request protection 跳过；
- host radix leaf group 是否与 device radix projection 分离。

如果不拆这两步，trace audit 很难区分是 async completion 错、host insert 错，还是 host eviction/protection 错。

## Profile fallback 边界

若 target async model 证明现有 facts 不足，不应补 source result，而应补 target-independent logical progress metadata。

允许讨论的 metadata：

- `checkpoint_kind`；
- `checkpoint_epoch`；
- `scheduler_iteration`；
- `logical_prefetch_round`；
- `async_barrier_kind`；
- `work_group_id`；
- enqueue order；
- deadline / wait budget kind。

不应新增为 normal input 的字段：

- `completed_pages`；
- `loaded_pages`；
- `ready_pages`；
- `source_completed_tokens`；
- `source_host_ref_delta`；
- `source_storage_hit_result`。

## 建议实施顺序

1. 显式化 C++ `AsyncState`，拆开 planned / pending / ready / suppressed / late。
2. `prefetch_decision` 只 enqueue target-derived work intent，不直接产生 host-visible result。
3. `prefetch_check_point` 按 target policy 推进 queue。
4. `HostVisibilityApply` 将 ready work 写入 L2/backuped/evicted，并触发 host capacity / protection。
5. 为 S1B missing/extra pages 增加逐 trace audit：
   - 是否 enqueue 少了；
   - 是否 checkpoint 没 ready；
   - 是否 ready 后没 apply 到 L2；
   - 是否 host eviction victim 选错；
   - 是否 backuped/evicted 生命周期不同步。
6. 四向 validation：
   - S1A self 保持 final match；
   - S1B -> S1A 保持 final match；
   - S1B self 从 L2/backuped/evicted `56/55` 收敛到 match；
   - S1A -> S1B 同步收敛。

## 风险

`best_effort` 下如果没有 target-independent progress budget，纯 async model 可能无法判断具体哪些 work 在 checkpoint 前 ready。
此时才需要回到 profiling contract，但补的是 logical scheduling anchor，不是 source completion result。

因此本轮倾向：

```text
优先 async model；
保留 host_prefetch_storage_visibility_boundary 作为诊断分类；
不新增 source visibility normal event；
必要时只扩展 checkpoint / work intent metadata。
```
