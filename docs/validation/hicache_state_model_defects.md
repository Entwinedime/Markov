# HiCache State Model 缺陷清单

维护方式：本文只记录当前 HiCache state model 仍需要修复或验证的缺陷。实验流水账写入
`hicache_state_validation.md` 或 `work_progress.md`。

## 当前前提

截至 2026-06-12：

- profiling 已切到 target-level atomic fact contract；
- 当前 mainline S1A/S1B profiling 契约保留 33 个 target，正常状态模型输入是 7 个 target / 5 个 role：
  `request_bound_match_anchor`、`request_lifecycle_anchor`、`request_admission`、`prefetch_decision`、
  `prefetch_check_point`；
- C++ backend 必须只消费 `model_input=true && fact_class=invariant_state && fact_granularity=atomic` 且 role 属于
  已知 atomic invariant 的事件；
- target pages 必须由 token dictionary/span 和 target `page_size` 重建；
- backend radix tree 和 L1/L2 capacity enforcement 已按 `cache_scope` 隔离；当前有效 correctness 仍看 `strip_scope`
  normalized page hash union；
- source_actual、timing_observation、oracle_state 不更新 target state；
- 旧 `request_tokens`、`lookup_path`、`request_cache_lifecycle` 混合 role 已从主配置和 router 删除；match-prefix concrete
  path、lifecycle path/runtime、`insert_path`、`capacity_request`、`lock_scope_delta` 和具体 `maintenance_checkpoint`
  target 当前都是 source evidence；
- cross audit 的 hard `model_input_contract` 只比较 atomic invariant facts，逐 role 检查 count 和 request-normalized
  canonical fact multiset；raw `request_id` 不作为跨配置 invariant，sequence mismatch 是诊断信号；
- 旧 2026-06-10 S1A/S1B profile 的 `non_invariant_fact_usage=[]` 已在 self-config 和 cross-config 四个方向中达成，
  但该 run 仍是 12-target 旧采集契约，只作为历史诊断；
- 2026-06-11 S1B 31-target host-node-projection self prediction 已达成 `invariant_coverage_ready=true`、
  `missing_invariant_facts=[]`、`non_invariant_fact_usage=[]`；
- 当前 S1B self 的 L1/dirty/locked final sets 已与 normalized oracle 对齐；normal prediction 的剩余 mismatch 集中在
  L2/backuped/evicted，且已通过模型侧 diagnostic async-elision 证明该 self-config final diff 来自
  async/input-boundary 分岔后的连锁状态差，而不是已知 deterministic final-set model bug。
- 当前 S1A self normal prediction 已 final state match；但 trace scan 仍可见 `locked_pages` 暂态差异，归类为
  snapshot/input-boundary 暂态。
- 当前 33-target atomic profile 已完成 S1A/S1B manual run，双向 cross audit 的 `model_input_contract_ready=true`；
  修复前 retained cross-config 仍不能做 timestamp oracle injection，因为 S1A/S1B 执行窗口不重叠，旧
  `capacity_request`、`lock_scope_delta` 和 request token sequence 也未证明 target-independent。
- 当前 front-door workload audit 已确认 S1A/S1B 的 benchmark 入口请求形状与 prompt identity 对齐；cross 问题不是
  workload_report 层面的请求不一致。
- 修复前 retained cross input audit 已证明旧输入契约不闭环：两向 high-risk roles 都包括 `request_tokens`、`lookup_path`、
  `insert_path`、`request_cache_lifecycle`。增强审计显示 `request_tokens` / `lookup_path` 两边事件数和 binding
  shape 一致，都是 `150` 个 completed event、`100 request_bound + 50 unbound`，但第 16 个 unbound event 已是
  S1A `768` tokens vs S1B `832` tokens；`insert_path` 两边都是 `100` 个 completed event 且全部 unbound，
  第 8 个 event 也已是 `768` vs `832`。这里的 `request_tokens` 记录 `HiRadixCache.match_prefix(params.key)`
  的 cache-stage key path，不保证等同于 HTTP 原始 prompt。
- 修复前 contract layer 审计进一步显示：`request_bound_match_prefix_paths` 两向都是 `200/200` aligned；blocking
  layers 是 `unbound_match_prefix_paths`、`insert_paths`、`request_lifecycle_paths`。因此下一步应拆分
  request-scoped front-door token facts 与 cache-stage/control-flow path facts，而不是继续 patch L1/L2/evicted 规则。
- request-bound anchor 审计显示 unbound/insert path 多数能解释为 exact request-bound path 或按 source page size
  向下对齐后的 request-bound token prefix；新增 temporal anchor 后，`unbound_match_prefix_paths` / `insert_paths`
  在两向真实 audit 中都是 `100/100` temporal resolved：S1A->S1B 为 `96 exact + 0 candidate + 4 empty`，
  S1B->S1A 为 `36 exact + 60 candidate + 4 empty`。这说明它们更像 request-bound token facts 经 page-size /
  cache-stage anchor 派生出的 path，不应继续作为 direct source unbound path 被 normal cross model 消费。
- 修复前新增 projection gate 后，`projection_ready_layers` 两向都是 `insert_paths` 和 `unbound_match_prefix_paths`；
  `contract_blocking_layers_after_projection` 两向收敛为 `request_lifecycle_paths` 和
  `source_control_flow_checkpoints`。也就是说 unbound/insert 已经是可机制化投影目标，真正阻断 cross async-elision
  proof 的是 lifecycle/control-flow 输入契约。
- `after_projection_blockers` 进一步显示两向剩余 blocker 是同一组 role：
  `request_cache_lifecycle`、`capacity_request`、`lock_scope_delta`、`maintenance_checkpoint`。其中
  `request_admission`、`prefetch_decision`、`prefetch_check_point` 当前 role-level aligned，不是 after-projection
  blocker。
- 修复前新增 `normal_model_input_contract` 后，两向 retained cross audit 都明确输出
  `contract_status=blocked_by_input_contract`、`input_contract_ready_for_cross_state_rule_diagnosis=false`、
  `input_contract_ready_for_non_async_correctness_claim=false`；`unsafe_roles_after_projection` 仍是
  `capacity_request`、`lock_scope_delta`、`maintenance_checkpoint`、`request_cache_lifecycle`。这使“当前不能把
  cross final diff 当 deterministic state-rule bug”成为机器可检查的结论，而不是只写在文档里的判断。
- `capacity_request` 已修正为 target page-size 计数：当 fact 带 `requested_tokens` 时，按 target `page_size`
  重算 requested pages，再触发对应次数的 modeled L1 eviction pressure；它仍不是 source victim oracle。headroom/free-space
  假设已用真实 self-config 证伪，不能作为正常模型规则。
- 新增 `capacity_pressure_analysis` 后，cross capacity blocker 已确认不是 page-size-only：两向都是
  `page_size_only_explains_count=0`、`requested_tokens_differ_count=8`，并有 `4` 个 one-sided capacity event；
  首个 mismatch 是 S1A `requested_tokens=1057, requested_pages=9, page_size=128` vs S1B
  `requested_tokens=993, requested_pages=16, page_size=64`。因此 source `capacity_request` 记录的是 target/control-flow
  pressure sequence，不能直接作为 cross-safe normal invariant input。
- 新增 `lock_scope_analysis` 后，cross lock/ref blocker 的性质也更清楚：两向 inc/dec 在各自 stream 内都平衡且
  net delta 都是 `0`，但 pair 分类包含 path mismatch `228`、direction mismatch `166` 和 one-sided event `44`；
  首个有效 mismatch 是 index `17`，S1A `inc` 768-token page-aligned prefix 对 S1B empty-path `dec`。所以 final
  locked `0/0` 不代表 cross lock/ref delta 序列可作为 normal input 复用。
- 新增 `maintenance_checkpoint_analysis` 后，maintenance blocker 主要是 checkpoint schedule / async polling 差异：
  两向 `flush_write_through_acks=384`、`ready_to_load_host_cache=50` 对齐，`maintenance_check` 是 `592/596`
  或反向；pair 分类为 `check_kind_mismatch=348`、one-sided event `4`。首个 mismatch 是 index `108`，
  `maintenance_check` vs `ready_to_load_host_cache`。
- `request_lifecycle_paths` 两向仍是 `0/150` temporal resolved、`150 unresolved`。C++ 当前对
  `request_admission` / `request_cache_lifecycle` 只更新 request-scoped token store，不直接产生 resident/dirty/evicted
  transition；因此 lifecycle 是 cross input-contract blocker，而不是当前 final-set 特化补丁入口。
- 新增 lifecycle suffix 诊断后，`request_cache_lifecycle` 的分歧已从“path hash 不同”细化为：
  两向都有 `same_request_anchor_different_suffix=50`，首个真实 mismatch 是 index `4`，source/target 的同 request
  anchor hash 相同、anchor token count 都是 `5`，但 committed/generated suffix 都是 `9` tokens 且 hash 不同；
  另有 `both_missing_lifecycle_token_ids=26`、`one_side_missing_lifecycle_token_ids=24`。这说明 lifecycle path 把生成或
  fill/committed suffix 带入了 invariant 输入，不能靠 prompt/page-size projection 修正。

最新真实 modeling 验证仍是修复前 retained 结果：

```text
HCSV-20260612-async-elision-current-self-and-cross
```

当前 2026-06-12 input-contract repair 是 config/audit/fixture 层修复；真实 S1A/S1B profile 和 cross validation 需要在新契约下重跑。

最新 self-config 状态：

| prediction | final | 结论 |
| --- | --- | --- |
| S1A self normal | pass | final sets 全对齐；locked 暂态差异不作为 final bug。 |
| S1B self normal | fail | L2/backuped/evicted `56/55`，missing `13`，extra `14`。 |
| S1B self async-elided | pass | C++ 模型侧诊断注入 6 个 async checkpoint 后 L2/backuped/evicted `55/55`。 |

S1B normal diff：

| tier | model/oracle | missing | extra | 结论 |
| --- | ---: | ---: | ---: | --- |
| L1 resident | 28/28 | 0 | 0 | 已对齐 |
| dirty | 28/28 | 0 | 0 | 已对齐 |
| L2 resident / backuped | 56/55 | 13 | 14 | normal prediction 受 async/input-boundary 阻塞 |
| evicted | 56/55 | 13 | 14 | normal prediction 受 async/input-boundary 阻塞 |
| locked | 0/0 | 0 | 0 | 已对齐 |

旧 `20260611_050859` 逐 trace 首个分岔：

- last matched：`hicache_maintenance_check_end:state_snapshot`，order `4200`，L1/dirty `28`，
  L2/backuped/evicted `56`；
- first divergence：`hicache_prefetch_check_point_end:state_snapshot`，order `4204`，ts `1781155586790332`，
  scope `rank:unknown:HiRadixCache:281452413813520`；
- oracle 在该 checkpoint 把 L2/backuped/evicted 从 `56` 推到 `69`，model 仍是 `56`；
- missing 的 13 页与同 timestamp 的 source-only prefetch progress `operation_hash_pages` 对齐，但该 source evidence
  不能作为 target state 输入。

当前 `20260611_054436` 诊断补充：

- 包含 locked 时，trace scan 首先碰到 capacity/maintenance 附近 `locked_pages` 暂态差异；final locked 仍是 `0/0`；
- 排除 locked 后，S1B self trace scan 注入 6 个 async 分岔；
- Python replay 注入后仍显示 capacity 附近 L2/backuped/evicted extra `14`，但 C++ 模型侧 async-elision final match，
  说明该 extra 是 async 分岔后的连锁状态差，不是单独的最终模型 bug。

历史 2026-06-10 四向 diff：

| prediction | L1 resident | L2 resident / backuped | dirty | evicted | locked |
| --- | --- | --- | --- | --- | --- |
| S1A self | 32/54, missing 22 | 80/106, missing 26 | 0/0 match | 48/52, missing 26, extra 22 | 11/11 match |
| S1B self | 64/108, missing 44 | 170/144, missing 36, extra 62 | 64/72, missing 8 | 170/108, extra 62 | 0/22, missing 22 |
| S1A -> S1B | 64/108, missing 44 | 164/144, missing 40, extra 60 | 64/72, missing 8 | 164/108, missing 4, extra 60 | 22/22 match |
| S1B -> S1A | 32/54, missing 22 | 80/106, missing 26 | 0/0 match | 48/52, missing 26, extra 22 | 0/11, missing 11 |

这说明输入分流方向不是当前阻塞点；阻塞点在状态建模结构。下面的缺陷是新 token-level backend 的验收目标和
provenance 对照清单，不应继续解释为在旧 `HiCacheState` page-set 实现上逐个小修的补丁列表。

## 分级

| 等级 | 含义 |
| --- | --- |
| `P0` | 阻断当前 S1A/S1B self-config state correctness。 |
| `P1` | 影响 cross-config 或复杂机制，但可在 self-config 修复后继续验证。 |
| `P2` | 不阻塞当前 state final sets，但影响 exact oracle 或后续 DAG patch。 |

## 总览

| ID | 等级 | 缺陷 | 当前表现 | 下一步 |
| --- | --- | --- | --- | --- |
| `HCSM-D1` | P1 | Resident / evicted lifecycle 历史回归仍需复测 | 当前 S1B self L1/dirty/locked 已对齐；历史 S1A 仍有 L1 missing / evicted extra 样本 | 新 S1A profile 可用后复测，不再用当前 S1B 做 L1 小修。 |
| `HCSM-D2` | P1 | L2/backuped/evicted lifecycle 仍需在 cross 中复核 | S1B self normal 为 56/55，missing 13、extra 14；模型侧 async-elision 后 55/55 match | self-config 不继续打补丁；cross logical alignment 后再判断是否仍有 cleanup/victim 问题。 |
| `HCSM-D3` | P1 | capacity / evictable / leaf group 仍是近似 | host projection 修复已把 maintenance 分岔消除；完整 node/ref/evictable 仍未实现 | 当前不继续局部修 leaf group，等 async prefetch 分岔解除后再逐 trace 评估 victim。 |
| `HCSM-D4` | P1 | token radix tree 仍不是完整 SGLang node/ref 结构 | prefix/split 复杂时可能错 resident/evicted | 补 token-level node provenance 或完善 radix split/merge 模型。 |
| `HCSM-D5` | P0 | async prefetch completion / storage insert 不可由当前 checkpoint 推导 | S1B self 的 normal mismatch 已由模型侧 async-elision 归因到该边界 | 不加“best_effort 全 pending 完成”特化规则；正常 prediction 仍需 target async model 或新增 invariant 字段。 |
| `HCSM-D6` | P1 | write-back background flush / host release 尚未完整证明 | 当前 S1B dirty 已 28/28；write-back 不再是首个分岔，但仍可能影响后续 final extra | async prefetch 分岔解除后再追 dirty eviction、flush completion、host release 和 L2 eviction。 |
| `HCSM-D7` | P1 | lock/ref replay order 已修，完整 parent chain 仍需验证 | 2026-06-11 S1B self locked 已达 0/0；历史 cross-config 仍需复测 | 四向新 profile 完成后复测，不再把 S1B self locked 作为当前 P0。 |
| `HCSM-D8` | P2 | ordered transition oracle 不足 | 目前主要看 final normalized sets | 后续加 transition provenance / exact oracle。 |
| `HCSM-D9` | P2 | state-to-DAG patch 未实现 | `dag_mutations=0` | state final sets 通过后再进入 DAG mutation。 |
| `HCSM-D10` | P0 | cross-config logical alignment / input contract 需要 backend validation 复核 | 修复前 audit 已证明前门请求一致，但旧正常输入混入 `request_tokens`、`lookup_path`、`insert_path`、`request_cache_lifecycle`、`capacity_request`、`lock_scope_delta`、`maintenance_checkpoint` 等 variant/cache-stage/control-flow facts；本轮 profile config 已改成 atomic fact，删除旧混合 role，并且 2026-06-12 15:38 的双向 atomic cross audit 已通过 `model_input_contract_ready=true` | 在新 profile 上重跑 self/cross modeling validation；若 final diff 仍存在，再归因到 async boundary、target-derived projection 缺口或 C++ state rule bug。 |

## 已收紧的规则

### HCSM-F1：capacity_request 提供 target-size pressure，不提供 source victim

`capacity_request` 是 capacity pressure checkpoint，不是 victim oracle。当前模型优先使用 `requested_tokens`
和 target `page_size` 重算 requested pages；只有缺少 token count 时才回退到 source `requested_pages`。
随后模型按这个 target page count 执行 L1 modeled eviction，victim 仍由 modeled LRU / lock / radix leaf group
规则选择。它不能指定 source victim，也不能绕过 modeled evictable/lock 规则。

已测试并拒绝的语义：把 `capacity_request` 解释成 target-side headroom/free-space 检查，也就是只有
`resident_pages + requested_pages > l1_capacity_pages` 时才淘汰。真实 self-config 验证显示该假设会让 S1A self
从 pass 退化成 L1 extra `7` / evicted missing `7`，并扩大 S1B self 的 host/L2 diff。因此当前不把它作为正常模型规则。

这个 retained fix 的边界必须保持清楚：它只说明“已经决定发生的同一个 capacity pressure event”应按 target page size
计算需要释放多少页。它不说明 source trace 中出现的 `capacity_request` 序列可以跨 target 复用。真实 cross audit
显示 S1A/S1B 的 capacity event 数量为 `8/12` 或 `12/8`，两向 `requested_tokens_differ_count=8`，
`page_size_only_explains_count=0`，并有 `4` 个 one-sided event；首个 pair 是 S1A `1057/9@128` vs S1B
`993/16@64`。因此 cross 中要解决的是 target capacity pressure sequence，而不是继续围绕 requested page count
做局部换算。

2026-06-11 S1B self 的早期影响有限；host projection 修复后该项不再是当前首个分岔：

- 保留 global timestamp replay 且 radix/capacity 按 scope 隔离后，早期 normalized L1 final 是 `32/28`，missing 0、extra 4；
- 加入 host projection 后，当前 L1/dirty 已 `28/28`，L2/backuped/evicted 收敛到 `56/55`；
- 因此主要 mismatch 不是单个 capacity_request 事件能决定的，后续重点转向 async prefetch completion / storage lifecycle。

### HCSM-F2：wait_complete checkpoint 不再全量构造 resident/ready

`prefetch_check_point` 当前 invariant 事实没有 loaded pages / completion pages。旧 backend 曾只保留旧
`prefetch_intent` 产生的 planned pages；31-target 重构后 planned pages 必须由 `prefetch_decision` 和 target state
重新判断产生。`wait_complete` checkpoint 不能把 pending pages 全部 add L2/L3，也不能在 finalize 阶段把未 ready
页全部 suppressed。

四向影响：

- target=S1A raw `prefetch_ready_pages` 从 712/728 降到 36/20；
- S1A self L2/backuped missing 从 28 降到 26；
- S1B -> S1A L2/backuped missing 从 41 降到 26；
- exact prefetch ready 仍不可从当前 invariant facts 判断，不能为了对齐 oracle final state 强推 completion。

### HCSM-F3：host eviction 使用 page-level compressed radix projection

prefetch host pressure 需要按真实 host radix 的 leaf node 粒度释放 host pages。旧模型只用 insert path 的 flat
leaf group，遇到两个 child 合并成 parent host leaf 时会漏删 parent pages，导致 `maintenance_check_end` 后 oracle 与
model 立即分岔。

当前实现给 `HiCacheTokenRadixTree` 增加 page-level compressed radix projection，并让 `evict_host_pages()` 使用
`host_eviction_leaf_groups(...)` 选择 host leaf group。新增 fixture
`run_prefetch_host_pressure_promotes_parent_host_leaf_fixture` 覆盖 parent host leaf 被提升为可淘汰 leaf 的场景。

效果：

- 旧首个分岔 `hicache_maintenance_check_end:state_snapshot` 已消失；
- S1B self L1/dirty 从 `32/28`、`31/28` 收敛到 `28/28`；
- L2/backuped/evicted 从 `81/55`、`80/55` 收敛到 `56/55`；
- 新首个分岔后移到 `prefetch_check_point`，说明继续改 host eviction 已不是当前最有效路线。

## HCSM-D1：Resident / Evicted 生命周期错误

### 现象

历史 2026-06-10 四向结果里四个方向都缺 L1 resident：

- S1A self：L1 32/54，missing 22；evicted 48/52，missing 26，extra 22；
- S1B self：L1 64/108，missing 44；evicted 170/108，extra 62；
- S1A -> S1B：L1 64/108，missing 44；evicted 164/108，missing 4，extra 60；
- S1B -> S1A：L1 32/54，missing 22；evicted 48/52，missing 26，extra 22。

S1A self 的 L1 missing sample `08c4433f...` 同时出现在 evicted extra sample。当前 2026-06-11 S1B self 已经没有
L1/dirty mismatch；resident/evicted lifecycle 的当前风险主要留在 L2/backuped/evicted final sets，以及新 S1A
profile 可用后的交叉复测。

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
在 host projection 修复前尤其明显；当前 2026-06-11 S1B self normalized L2/backuped/evicted 是 `56/55`，
missing 13、extra 14。scope 隔离后 raw model final 会保留两个 cache scope，raw oracle snapshot 仍不是可比口径，
因此当前判断以 normalized diff 为准。

### 当前实现

- L1/L2 有显式 capacity；
- `capacity_request` 按 `requested_tokens` 和 target `page_size` 重算 requested pages 后触发 L1 modeled eviction
  pressure；victim 仍由 modeled LRU / lock skip / radix leaf group 决定；
- touch order 是简化 LRU；
- dirty victim 触发 modeled writeback；
- locked pages 会跳过；
- token radix tree 提供当前 projection leaf group。

### 缺口

- 没有 SGLang allocator / pool exact 行为；
- 没有完整 evictable set；
- lock/ref replay order 已在 S1B self 对齐，完整 radix parent chain 仍未在四向新 profile 中验证；
- host leaf group 已有 page-level projection，但仍不等价于完整 SGLang node/ref/evictable set；
- L2 eviction 后 `backuped` / `evicted` 的语义可能不等价。

### 下一步

- 分离 evicted missing 和 evicted extra 两类 page；
- 对每个 victim 输出 eviction 前 L1/L2 set、touch order、locked、dirty、backuped、leaf group；
- 判断 extra 是否来自 L1 missing 组、L2 over-fill 组或 dirty eviction 组；
- 如果 victim 选择无法由现有 facts 解释，再把 evictable snapshot 放入下一轮集中采集。

cross-config 里还要额外注意：`capacity_request` 的 `requested_tokens` 来自 SGLang
`HiRadixCache.evict(params.num_tokens)`，这个调用已经处在 target allocator/radix/lock/memory availability 决策之后。
所以 source capacity pressure 不是 target-independent workload fact。若后续 cross 仍有 resident/evicted 差异，先确认
pressure sequence 是否已由 target model 或高层 invariant 重新定义，再判断是否是 D3 的 victim/evictable 规则错误。

## HCSM-D4：Token Radix Tree 仍不是完整 SGLang node/ref 结构

原 `HiCacheRadixTree` 已删除，后端使用 `HiCacheTokenRadixTree` 做 token-level prefix、split 和 insert。
但它仍只是最小 token radix 建模，不是完整 SGLang HiRadixCache node/state/ref 实现。

风险：

- node parent/ref/host-ref chain 信息不足；
- prefix overlap 复杂时 projection leaf group 可能不同于 SGLang evictable node set；
- insert/remove 合并生命周期不完整。

当前不因该项立即重采。先用四向 mismatch 判断它是否是实际 root cause；如果锁链、leaf group 或 prefix split 无法从
token path 推导，再集中补 node/ref provenance。

## HCSM-D5：Async Prefetch Scheduler 简化

旧模型：

- `prefetch_intent` 生成 planned pages；
- `wait_complete` checkpoint 不再把 pending pages 直接标为 L2/L3 resident 和 ready；
- `best_effort` checkpoint / finalize 会 suppress 未 ready pages；
- `timeout` 只按简单 elapsed timeout 标 late。

31-target 后端重构后，`prefetch_intent` 不再是 invariant input；target planned pages 必须由 `prefetch_decision`
结合 modeled token radix state 和 prefetch policy 生成。

缺口：

- 没有 queue / bandwidth / concurrent transfer；
- 没有 controller task lifecycle；
- source timing observation 不直接驱动 target ready；
- oracle final state 当前主要比较 resident/backuped/dirty/evicted/locked，不足以证明 prefetch exact。

四向结果显示 prefetch exact 仍不可直接验证：

- target=S1A 的 wait_complete 下，S1A self raw prefetch ready 36，S1B->S1A raw prefetch ready 20；
- target=S1B 的 best_effort 下，S1B self raw prefetch suppressed 1484，S1A->S1B raw prefetch suppressed 1452。

当前 S1B 最新逐 trace 结果显示，prefetch exact 已经从“非 final-state oracle 目标”升级为当前首个 state 分岔：

- invariant `hicache_prefetch_check_point_end` 只有 `check_kind=progress`，没有 completion token/page；
- 同 timestamp 的 source-only `hicache_prefetch_progress_observed_start` 显示 `completed_tokens=832`、
  `ready_pages_estimate=13` 和 13 个 `operation_hash_pages`；
- oracle 在该 checkpoint 后 L2/backuped/evicted 变为 `69`，model 仍是 `56`。

当前 `20260611_054436` S1B self 进一步用模型侧 diagnostic async-elision 验证：注入 6 个 async checkpoint 后，
L2/backuped/evicted final 从 normal 的 `56/55` 收敛为 `55/55`。Python replay 中残留的 capacity extra `14`
在 C++ synthetic trace 重跑后消失，因此它是 async 分岔后的连锁差异，不是当前 self-config 的独立 final-set 模型 bug。

短期不能把 `best_effort` checkpoint 特化为“所有 pending prefetch 都完成”。正常 prediction 若要不靠 diagnostic injection
通过，下一步要么建立 target async prefetch progress 模型，要么集中讨论新增 invariant 字段，例如 target-side prefetch
completion token count / page hashes / storage insert boundary；在这之前 source `prefetch_progress_observed` 和
`terminate_prefetch` 只能作为定位证据。

## HCSM-D6：Write-Back Flush 未闭环

S1B 是 `write_back + best_effort`。当前 latest self prediction 中 write-back 不再是首个分岔，但 L2/backuped/evicted
final sets 未闭环，后续仍需要复查 write-back / host release：

- dirty 28/28，missing 0、extra 0；
- L2/backuped 56/55，missing 13、extra 14；
- evicted 56/55，missing 13、extra 14；
- `missing_invariant_facts=[]` 且 `non_invariant_fact_usage=[]`，说明这不是输入门禁问题。

需要解释：

- dirty page 何时 flush；
- flush 后是否形成 L2/backuped；
- flush 与 eviction、prefetch、insert、host release 的顺序；
- background writeback 对后续 lookup 是否可见；
- host pool release / L2 eviction 后 `backuped`、`evicted` 的生命周期。

当前不为 write-back 单点重采。先处理 prefetch checkpoint 的首个分岔；只有之后仍证明现有 invariant facts 不能区分真实
dirty/backuped 转换时，才把 writeback trigger/completion 扩成新的集中采集契约。现有 `writeback_enqueue_observed` /
`writeback_io_observed` 仍不是 state model input。

## HCSM-D7：Lock / Ref Parent Chain

2026-06-11 S1B 31-target self prediction 中，lock/ref final set 已修到 `0/0 match`。root cause 是 backend fact replay
排序 comparator 非严格全序，导致部分 lock/ref delta 乱序；当前已改为严格全局 timestamp order，同 timestamp/scope 下再用
`seq_no` 破 ties。

仍需验证：

- target radix parent chain 完整；
- eviction eligibility 使用了正确 lock/ref；
- page-size what-if 和 cross-config 下 lock/ref 仍正确；
- lock/ref 是否解释 D3 的 victim 差异。

短期不再把 S1B self locked final 当 P0。新 S1A/S1B 四向 profile 完成后复测；若发现 victim 被真实 lock/ref 保护但模型无法推导，
再补 ordered lock/ref oracle。

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

1. 保留 S1A self normal pass 和 S1B self C++ async-elision pass 作为修复前 self-config 基线；换 run 或换输入契约时必须重新证明。
2. 使用已完成的 33-target atomic S1A/S1B profile 作为当前输入基线，除非 hook 语义或新 scope 变化，不重复 profile。
3. hard `model_input_contract_ready=true` 已由双向 cross audit 达成；若后续 contract 回退，优先修 profile config /
   probe source / target-derived projection，不围绕 final L1/L2/evicted 做特化补丁。
4. 重跑 self-config 和 cross-config modeling validation，并用逐 trace 分岔对比区分 async/input-boundary、
   deterministic model bug 和 remaining input-contract gap。
5. 对仍需表达的 lifecycle、capacity、lock 或 maintenance 机制，只能新增 target-independent 高层 invariant 或 target-derived
   机制；不能把 source suffix、source `params.num_tokens`、source lock/ref delta 或 source polling/check-kind 序列重新标成正常输入。
7. 不加入“best_effort checkpoint 全 pending 完成”规则；这类修正会把 source timing / completion 结果伪装成 model rule。
8. 每修一类状态规则后重新审视它是机制级修正还是当前 run 的特化补丁；至少两个 self-config 和两个 cross-config 的
   async-elision proof 都闭环后，才宣称“排除 async 后没有其他 state-rule 问题”。
9. state 通过后再进入 DAG patch。
