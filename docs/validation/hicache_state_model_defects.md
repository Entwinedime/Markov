# HiCache State Model 缺陷清单

维护方式：本文只记录当前 HiCache state model 仍需要修复或验证的缺陷。实验流水账写入
`hicache_state_validation.md` 或 `work_progress.md`。

## 当前前提

截至 2026-06-10：

- profiling 已切到 token/range invariant contract；
- C++ backend 只消费 `fact_class=invariant_state && state_model_input=true`；
- target pages 由 token dictionary/span 和 target `page_size` 重建；
- source_actual、timing_observation、oracle_state 不更新 target state；
- `non_invariant_fact_usage=[]` 已在 S1A token backend 中达成；
- S1A self-config 仍与 normalized oracle 不匹配。

最新有效验证：

```text
HCSV-20260610-token-backend-s1a
```

核心 diff：

| set | model | oracle | mismatch |
| --- | ---: | ---: | --- |
| `l1_resident_pages` | 32 | 54 | missing 22 |
| `l2_resident_pages` | 78 | 106 | missing 28 |
| `backuped_pages` | 78 | 106 | missing 28 |
| `evicted_pages` | 46 | 52 | missing 28, extra 22 |
| `dirty_pages` | 0 | 0 | match |
| `locked_pages` | 11 | 11 | match |

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
| `HCSM-D1` | P0 | L1/L2 resident 与 evicted 生命周期错误 | S1A L1 missing 22，evicted extra 22 | 逐 page 追 transition，优先看 lookup/load-back 与 capacity eviction。 |
| `HCSM-D2` | P0 | write-through-selective / backuped 建模不准 | S1A L2/backuped missing 28 | 对 hit count、threshold、insert prefix 和 backuped 清理做 provenance。 |
| `HCSM-D3` | P0 | capacity / evictable / leaf group 近似不足 | evicted missing 28 + extra 22 | 对比 victim 选择、locked skip、leaf group 和 touch order。 |
| `HCSM-D4` | P1 | radix tree 仍是 page-level 近似，不是完整 SGLang node/ref 结构 | prefix/split 复杂时可能错 resident/evicted | 补 token-level node provenance 或完善 radix split/merge 模型。 |
| `HCSM-D5` | P1 | async prefetch scheduler 仍过强简化 | wait_complete 下 modeled ready/planned raw count 过大；oracle final 不暴露 prefetch sets | 明确 ready 对 resident 的影响，后续再做 timing/queue。 |
| `HCSM-D6` | P1 | write-back background flush 尚未闭环 | S1B 预计仍会在 L2/dirty/backuped 上失败 | S1A 通过后验证 S1B，必要时补集中 event contract。 |
| `HCSM-D7` | P1 | lock/ref parent chain 仅按 logical path pages，未完整绑定 target radix parent chain | S1A final locked 已对齐，但 eviction eligibility 仍可能受影响 | capacity victim 中继续验证 locked/evictable。 |
| `HCSM-D8` | P2 | ordered transition oracle 不足 | 目前主要看 final normalized sets | 后续加 transition provenance / exact oracle。 |
| `HCSM-D9` | P2 | state-to-DAG patch 未实现 | `dag_mutations=0` | state final sets 通过后再进入 DAG mutation。 |

## HCSM-D1：Resident / Evicted 生命周期错误

### 现象

S1A normalized final state：

- L1 model 32，oracle 54，missing 22；
- evicted model 46，oracle 52，missing 28，extra 22；
- L1 missing sample `08c4433f...` 同时出现在 evicted extra sample。

这说明模型把一部分真实最终 resident 的 page 错误保留为 evicted，或者漏掉了后续把它们 load/promote 回 L1 的 transition。

### 可能原因

- `lookup_path` 只根据 target radix + modeled L2/L3 做 promotion，漏了真实 source 中由 load-back 表达的可读性；
- `insert_path` 的 prefix/longest-prefix 逻辑导致某些 page 没有被重新插入或 touch；
- `capacity_request` 的 LRU-like victim 顺序与 SGLang 不一致；
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

先解释 `08c4433f...`，再批量分组 22 个 L1 missing 页。

## HCSM-D2：Write-Through-Selective / Backuped 建模不准

### 现象

S1A 是 `write_through_selective`，target threshold 默认 `2`。normalized final state：

- L2 model 78，oracle 106；
- backuped model 78，oracle 106；
- dirty 0/0 match。

模型低估 L2/backuped，说明部分应被 selective backup 的 pages 没进入 modeled L2/backuped，或后来被模型错误清理。

### 可能原因

- hit count 只在 `insert_path` 或部分 full pages 上增长，不等价于 SGLang 的 selective write 条件；
- prefix hit / lookup hit 没有增加写回计数；
- insert prefix 裁剪后没有对已存在页面执行 selective write；
- L2 capacity enforcement 或 eviction 清掉了 oracle 中仍 resident 的 backuped pages；
- `mark_backuped` 与 `remove_resident("L2")` 的联动过强，导致 L2 remove 同时 clear backuped。

### 下一步

- 对 28 个 L2/backuped missing 页查 `increment_hit_count`、`add_l2_resident`、`mark_backuped` 是否出现；
- 验证 threshold 是否应来自 `cache_config_observed.thresholds.write_through_threshold`，而不是固定默认；
- 对比 missing 页是否曾被模型 L2 eviction 清掉；
- 必要时把 write-through-selective 的触发事实定义为新的 invariant 或 oracle 采集项。

## HCSM-D3：Capacity / Evictable / Leaf Group 近似不足

### 现象

evicted 同时出现 missing 和 extra，说明不是单纯容量大小错误，而是 victim 集合或 evicted lifecycle 错误。

### 当前实现

- L1/L2 有显式 capacity；
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
- 判断 extra 是否全部来自 L1 missing 组；
- 如果 victim 选择无法由现有 facts 解释，再把 evictable snapshot 放入下一轮集中采集。

## HCSM-D4：Radix Tree 仍是近似

当前 `HiCacheRadixTree` 是 target page-level radix tree。它支持 common prefix、split、leaf group 和 remove，
但不是完整 SGLang HiRadixCache node/state/ref 实现。

风险：

- token-level node split 与 page-level split 不完全一致；
- parent-child/ref chain 信息不足；
- prefix overlap 复杂时 leaf group 可能不同；
- insert/remove 合并生命周期不完整。

当前不因该项立即重采。先用 S1A mismatch 判断它是否是实际 root cause。

## HCSM-D5：Async Prefetch Scheduler 简化

当前模型：

- `prefetch_intent` 生成 planned pages；
- `wait_complete` 在 checkpoint 把 pending pages 直接标为 L2/L3 resident 和 ready；
- `best_effort` / `wait_complete` finalize 会 suppress 未 ready pages；
- `timeout` 只按简单 elapsed timeout 标 late。

缺口：

- 没有 queue / bandwidth / concurrent transfer；
- 没有 controller task lifecycle；
- source timing observation 不直接驱动 target ready；
- oracle final state 当前主要比较 resident/backuped/dirty/evicted/locked，不足以证明 prefetch exact。

S1A 中 modeled raw prefetch planned/ready 都是 712，normalized 后是 356；这很可能过强。短期只修它对 resident 的影响，
不把 prefetch set exact 作为当前验收门槛。

## HCSM-D6：Write-Back Flush 未闭环

S1A 不是 write-back 场景，因此该缺陷不解释当前 S1A 主 mismatch。但 S1B 是 `write_back + best_effort`，
预计会继续暴露：

- dirty page 何时 flush；
- flush 后是否形成 L2/backuped；
- flush 与 eviction、prefetch、insert 的顺序；
- background writeback 对后续 lookup 是否可见。

当前不为单点重采。S1A 修完后跑 S1B，再决定是否需要把 writeback trigger/completion 放入集中采集契约。

## HCSM-D7：Lock / Ref Parent Chain

S1A final locked normalized 11/11 match，但这只说明最终集合对齐；还不能证明：

- target radix parent chain 完整；
- eviction eligibility 使用了正确 lock/ref；
- page-size what-if 下 lock/ref 仍正确。

短期关注 lock 是否影响 D3 victim 选择。若发现 victim 被真实 lock/ref 保护但模型无法推导，再补 ordered lock/ref oracle。

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

1. 写 provenance debug，先解释 `08c4433f...`。
2. 修 S1A resident/evicted 生命周期。
3. 修 S1A L2/backuped selective write。
4. S1A self-config 通过后跑 S1B self-config。
5. 根据 S1B 结果决定 write-back / prefetch 是否需要集中重采。
6. 两个 self-config 通过后再跑 cross-config。
7. state 通过后再进入 DAG patch。
