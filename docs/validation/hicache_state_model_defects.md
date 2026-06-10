# HiCache State Model 缺陷清单

维护方式：本文只记录当前 HiCache state model 仍需要修复或验证的缺陷。实验流水账写入
`hicache_state_validation.md` 或 `work_progress.md`。

## 当前前提

截至 2026-06-10：

- profiling 已切到 token/range invariant contract；
- C++ backend 只消费 `fact_class=invariant_state && state_model_input=true`；
- target pages 由 token dictionary/span 和 target `page_size` 重建；
- source_actual、timing_observation、oracle_state 不更新 target state；
- `non_invariant_fact_usage=[]` 已在 S1A/S1B self-config 和 S1A<->S1B cross-config 四个方向中达成；
- S1A/S1B profile quality 和 invariant coverage 均为 ready；
- 四个方向的 final state 全部与 normalized oracle 不匹配。

最新有效验证：

```text
HCSV-20260610-four-way-s1a-s1b
```

核心 diff：

| prediction | L1 resident | L2 resident / backuped | dirty | evicted | locked |
| --- | --- | --- | --- | --- | --- |
| S1A self | 32/54, missing 22 | 80/106, missing 26 | 0/0 match | 48/52, missing 26, extra 22 | 11/11 match |
| S1B self | 64/108, missing 44 | 170/144, missing 36, extra 62 | 64/72, missing 8 | 170/108, extra 62 | 0/22, missing 22 |
| S1A -> S1B | 64/108, missing 44 | 164/144, missing 40, extra 60 | 64/72, missing 8 | 164/108, missing 4, extra 60 | 22/22 match |
| S1B -> S1A | 32/54, missing 22 | 80/106, missing 26 | 0/0 match | 48/52, missing 26, extra 22 | 0/11, missing 11 |

这说明采集和输入分流已经不是当前阻塞点；阻塞点在状态规则。

## 分级

| 等级 | 含义 |
| --- | --- |
| `P0` | 阻断当前 S1A/S1B self-config state correctness。 |
| `P1` | 影响 cross-config 或复杂机制，但可在 self-config 修复后继续验证。 |
| `P2` | 不阻塞当前 state final sets，但影响 exact oracle 或后续 DAG patch。 |

## 总览

| ID | 等级 | 缺陷 | 当前表现 | 下一步 |
| --- | --- | --- | --- | --- |
| `HCSM-D1` | P0 | L1/L2 resident 与 evicted 生命周期错误 | 四向均缺 L1 resident；S1A target 同时有 evicted missing/extra | 逐 page 追 transition，优先看 lookup/load-back、clear_evicted、promotion 与 capacity eviction。 |
| `HCSM-D2` | P0 | L2/backuped 写入与保留规则不准 | S1A target 低估 L2/backuped；S1B target 既 missing 又 extra | 分开验证 write-through-selective hit threshold、write-back flush、L2 eviction 后 backuped 语义。 |
| `HCSM-D3` | P0 | capacity / evictable / leaf group 近似不足 | S1B target raw L2/backuped/evicted 被推到 capacity 321，normalized extra 60-62 | 对比 victim 选择、locked skip、leaf group、touch order 和 dirty victim 规则。 |
| `HCSM-D4` | P1 | radix tree 仍是 page-level 近似，不是完整 SGLang node/ref 结构 | prefix/split 复杂时可能错 resident/evicted | 补 token-level node provenance 或完善 radix split/merge 模型。 |
| `HCSM-D5` | P1 | async prefetch scheduler exact 仍不可观测 | wait_complete checkpoint 全量 ready 已收紧；exact ready set 仍非 final-state oracle 目标 | 不从 checkpoint 强推 completion；后续需要 ordered prefetch completion 才做 exact。 |
| `HCSM-D6` | P0 | write-back background flush 尚未闭环 | S1B self dirty 64/72，L2/backuped 170/144 且 extra 62 | 追 flush enqueue/io、dirty clear、backuped 标记、dirty eviction 的顺序。 |
| `HCSM-D7` | P0 | lock/ref parent chain 仅按 logical path pages，未完整绑定 target radix parent chain | S1B self locked 0/22，S1B->S1A locked 0/11；S1A source 的 lock 能对齐 S1B target | 先查 source profile lock_scope_delta 如何绑定 target radix parent chain。 |
| `HCSM-D8` | P2 | ordered transition oracle 不足 | 目前主要看 final normalized sets | 后续加 transition provenance / exact oracle。 |
| `HCSM-D9` | P2 | state-to-DAG patch 未实现 | `dag_mutations=0` | state final sets 通过后再进入 DAG mutation。 |

## 已收紧的规则

### HCSM-F1：capacity_request 不再强制选择 victim

`capacity_request.requested_pages` 是 allocation pressure，不是 victim oracle。模型现在只把该事件作为容量检查点，
仅当 modeled tier 已超过 target capacity 时由正常 capacity enforcement 淘汰。fixture 已更新为不再要求
`capacity_request` 精确淘汰 requested pages。

四向影响有限：

- S1B self L1 missing 从 46 降到 44，L2/backuped/evicted extra 从 64 降到 62；
- S1B -> S1A L1 missing 从 39 降到 22，evicted extra 从 39 降到 22；
- 仍不能解释主要 mismatch，说明剩余问题不是单个 capacity_request 事件能决定的。

### HCSM-F2：wait_complete checkpoint 不再全量构造 resident/ready

`prefetch_check_point` 当前 invariant 事实没有 loaded pages / completion pages。模型现在只保留 `prefetch_intent`
产生的 planned pages；`wait_complete` checkpoint 不再把 pending pages 全部 add L2/L3，也不在 finalize 阶段把未 ready
页全部 suppressed。

四向影响：

- target=S1A raw `prefetch_ready_pages` 从 712/728 降到 36/20；
- S1A self L2/backuped missing 从 28 降到 26；
- S1B -> S1A L2/backuped missing 从 41 降到 26；
- exact prefetch ready 仍不可从当前 invariant facts 判断，不能为了对齐 oracle final state 强推 completion。

## HCSM-D1：Resident / Evicted 生命周期错误

### 现象

四个方向都缺 L1 resident：

- S1A self：L1 32/54，missing 22；evicted 48/52，missing 26，extra 22；
- S1B self：L1 64/108，missing 44；evicted 170/108，extra 62；
- S1A -> S1B：L1 64/108，missing 44；evicted 164/108，missing 4，extra 60；
- S1B -> S1A：L1 32/54，missing 22；evicted 48/52，missing 26，extra 22。

S1A self 的 L1 missing sample `08c4433f...` 同时出现在 evicted extra sample。这说明模型把一部分真实最终
resident 的 page 错误保留为 evicted，或者漏掉了后续把它们 load/promote 回 L1 的 transition。

### 可能原因

- `lookup_path` 只根据 target radix + modeled L2/L3 做 promotion，漏了真实 source 中由 load-back 表达的可读性；
- `insert_path` 的 prefix/longest-prefix 逻辑导致某些 page 没有被重新插入或 touch；
- capacity enforcement 的 LRU-like victim 顺序与 SGLang 不一致；
- `leaf_group_for_page` 粒度与真实 radix leaf group 不一致；
- prefetch/write-through 让页面进入 L2/L3 的时机不对，导致后续 lookup 无法提升。

### 当前禁止的修复

- 不能重新消费 source `load_back` movement 直接 add L1；
- 不能恢复 `target_page_identity_page<page_size>`；
- 不能从 final oracle 反向填 resident。

### 下一步

为 mismatch page 输出：

- model transition trace：kind、role、request_id、operation_id、seq、ts；
- final oracle membership；
- model 中最后一次 add/remove/evict/promotion；
- 同一 request 的 lookup / insert / capacity_request 上下文。

先解释 `08c4433f...`，再按 target=S1A / target=S1B 分组批量验证 L1 missing 与 evicted extra 是否同源。

## HCSM-D2：L2 / Backuped 建模不准

### 现象

S1A 是 `write_through_selective`，target threshold 默认 `2`，模型低估 L2/backuped：

- S1A self：L2/backuped 80/106，missing 26；
- S1B -> S1A：L2/backuped 80/106，missing 26；
- dirty 0/0 match。

S1B 是 `write_back + best_effort`，模型同时 missing 和 extra：

- S1B self：L2/backuped 170/144，missing 36，extra 62；
- S1A -> S1B：L2/backuped 164/144，missing 40，extra 60。

这说明问题不只是 selective write threshold。模型既会漏掉应保留的 L2/backuped page，也会把不该保留的 page 填到 L2。

### 可能原因

- hit count 只在 `insert_path` 或部分 full pages 上增长，不等价于 SGLang 的 selective write 条件；
- prefix hit / lookup hit 没有增加写回计数；
- insert prefix 裁剪后没有对已存在页面执行 selective write；
- write-back flush completion 和 dirty clear / backuped mark 的顺序不对；
- L2 capacity enforcement 或 eviction 清掉了 oracle 中仍 resident 的 backuped pages；
- `mark_backuped` 与 `remove_resident("L2")` 的联动过强，导致 L2 remove 同时 clear backuped。

### 下一步

- 对 S1A missing 页查 `increment_hit_count`、`add_l2_resident`、`mark_backuped` 是否出现；
- 验证 S1A threshold 是否来自 `cache_config_observed.thresholds.write_through_threshold`，而不是固定默认；
- 对 S1B missing/extra 页分别查 flush enqueue/io、dirty clear、backuped mark 和 L2 eviction；
- 对比 missing 页是否曾被模型 L2 eviction 清掉，extra 页是否来自过强 capacity fill；
- 必要时把 write-through-selective 的触发事实定义为新的 invariant 或 oracle 采集项。

## HCSM-D3：Capacity / Evictable / Leaf Group 近似不足

### 现象

evicted 同时出现 missing 和 extra，说明不是单纯容量大小错误，而是 victim 集合或 evicted lifecycle 错误。S1B target
尤其明显：raw model final state 中 L2/backuped/evicted 都达到 `321`，等于目标 L2 capacity；normalized 后仍比 oracle 多
60-62 个 L2/backuped/evicted page。

### 当前实现

- L1/L2 有显式 capacity；
- `capacity_request` 只触发容量检查，不再强制按 requested pages 淘汰；
- touch order 是简化 LRU；
- dirty victim 触发 modeled writeback；
- locked pages 会跳过；
- page-level radix tree 提供 leaf group。

### 缺口

- 没有 SGLang allocator / pool exact 行为；
- 没有完整 evictable set；
- lock/ref 与 radix parent chain 没有严格绑定；
- leaf group 是 page-level 近似；
- L2 eviction 后 `backuped` / `evicted` 的语义可能不等价。

### 下一步

- 分离 evicted missing 和 evicted extra 两类 page；
- 对每个 victim 输出 eviction 前 L1/L2 set、touch order、locked、dirty、backuped、leaf group；
- 判断 extra 是否来自 L1 missing 组、L2 over-fill 组或 dirty eviction 组；
- 如果 victim 选择无法由现有 facts 解释，再把 evictable snapshot 放入下一轮集中采集。

## HCSM-D4：Radix Tree 仍是近似

当前 `HiCacheRadixTree` 是 target page-level radix tree。它支持 common prefix、split、leaf group 和 remove，
但不是完整 SGLang HiRadixCache node/state/ref 实现。

风险：

- token-level node split 与 page-level split 不完全一致；
- parent-child/ref chain 信息不足；
- prefix overlap 复杂时 leaf group 可能不同；
- insert/remove 合并生命周期不完整。

当前不因该项立即重采。先用四向 mismatch 判断它是否是实际 root cause；如果锁链、leaf group 或 prefix split 无法从
token path 推导，再集中补 node/ref provenance。

## HCSM-D5：Async Prefetch Scheduler 简化

当前模型：

- `prefetch_intent` 生成 planned pages；
- `wait_complete` checkpoint 不再把 pending pages 直接标为 L2/L3 resident 和 ready；
- `best_effort` checkpoint / finalize 会 suppress 未 ready pages；
- `timeout` 只按简单 elapsed timeout 标 late。

缺口：

- 没有 queue / bandwidth / concurrent transfer；
- 没有 controller task lifecycle；
- source timing observation 不直接驱动 target ready；
- oracle final state 当前主要比较 resident/backuped/dirty/evicted/locked，不足以证明 prefetch exact。

四向结果显示 prefetch exact 仍不可直接验证：

- target=S1A 的 wait_complete 下，S1A self raw prefetch ready 36，S1B->S1A raw prefetch ready 20；
- target=S1B 的 best_effort 下，S1B self raw prefetch suppressed 1484，S1A->S1B raw prefetch suppressed 1452。

短期只修不变量边界明确的过度推导，不把 prefetch set exact 作为当前验收门槛。

## HCSM-D6：Write-Back Flush 未闭环

S1B 是 `write_back + best_effort`，已经暴露 write-back 不闭环：

- S1B self dirty 64/72，missing 8；
- S1A -> S1B dirty 64/72，missing 8；
- S1B target 的 L2/backuped/evicted normalized extra 60-62。

需要解释：

- dirty page 何时 flush；
- flush 后是否形成 L2/backuped；
- flush 与 eviction、prefetch、insert 的顺序；
- background writeback 对后续 lookup 是否可见。

当前不为单点重采。先做 provenance；只有证明现有 invariant facts 不能区分真实 dirty/backuped 转换时，才把 writeback
trigger/completion 扩成新的集中采集契约。现有 `writeback_enqueue_observed` / `writeback_io_observed` 仍不是 state model input。

## HCSM-D7：Lock / Ref Parent Chain

lock/ref 呈现明显 source asymmetry：

- S1A self locked 11/11 match；
- S1A -> S1B locked 22/22 match；
- S1B self locked 0/22，missing 22；
- S1B -> S1A locked 0/11，missing 11。

这说明 S1A source facts 可以驱动 target lock final set，但 S1B source facts 会把 lock 全部丢掉。需要验证：

- target radix parent chain 完整；
- eviction eligibility 使用了正确 lock/ref；
- page-size what-if 下 lock/ref 仍正确。

短期先查 S1B `lock_scope_delta` 的 token path、scope、seq 和 end-state 是否与 S1A 不同；随后验证 lock 是否影响 D3 victim
选择。若发现 victim 被真实 lock/ref 保护但模型无法推导，再补 ordered lock/ref oracle。

## HCSM-D8：Ordered Transition Oracle 不足

当前主要依赖 final state snapshot 和 normalized set diff。它能判断最终集合错，但不能完整证明 transition 顺序。

后续需要：

- operation-level ordered transition oracle；
- transition before/after；
- request_id、operation_id、cache_scope、page、kind；
- required / optional / unobservable transient 标记。

## HCSM-D9：State-to-DAG Patch 未实现

HiCacheModule 当前 `dag_mutations=0`。这不是 state validation 失败原因，但说明 HiCache E2E what-if 还没开始。

进入 DAG patch 前必须先满足：

- S1A self-config final state match；
- S1B self-config final state match；
- 至少一组 cross-config final state match；
- `non_invariant_fact_usage=[]`；
- mismatch provenance 能解释。

## 当前不允许的修复方式

- 不允许恢复 observed replay。
- 不允许显式 observed policy。
- 不允许消费 source movement、target actual trace 或 oracle snapshot 修 target state。
- 不允许新增 page-size-specific target identity 矩阵。
- 不允许因为 final state 偶然对齐就忽略 `non_invariant_fact_usage`。

## 当前处理顺序

1. 使用 `scripts/internal/hicache_state_provenance.py` 继续扩展逐 page evidence；不要用 oracle 反向补 transition。
2. 先解释 S1A `08c4433f...` 这类 L1 missing / evicted extra，再批量分组 S1A target 的 resident/evicted mismatch。
3. 并行解释 S1B target raw L2/backuped/evicted 到 capacity 321 的路径，确认哪些来自 dirty eviction，哪些来自不可观测 writeback cleanup。
4. lock/ref source asymmetry 暂列难修：S1B self 和 S1B->S1A 的 locked final set 缺失不能简单通过“保留 lock”修复。
5. write-back dirty/backuped 规则只能修有事实支撑的顺序；没有 completion/victim 事实时不强推。
6. 每修一类状态规则后重跑四向矩阵，至少两个 self-config 都通过后再评估 cross-config 剩余缺口。
7. state 通过后再进入 DAG patch。
