# HiCache State Validation

本文是 HiCache state validation 的 active 文档，只记录当前有效口径、最新结果、复现入口和下一步。
当前缺陷清单维护在 [hicache_state_model_defects.md](hicache_state_model_defects.md)。

## 目标

本阶段只验证一个问题：

```text
base profiling invariant facts + explicit target cache config
  -> C++ HiCache state model
  -> predicted target cache state
  -> compare with oracle state snapshot
```

它不是 DAG patch 验收，也不是 E2E 性能预测验收。`prediction.json.predicted_e2e_ns` 只能作为 runner / DAG sanity check；
不能用来证明 HiCache state 正确。

## 术语

- `faithful_replay`：`mode=faithful_replay`，不加载任何子模块，不 patch DAG，消费完整真实执行 trace。
- `self-config prediction`：base facts 和 target config 来自同一场景，但仍显式建模 target。
- `cross-config prediction`：base facts 来自 source run，target config 来自另一个场景；target run 只做 oracle。
- `oracle trace`：validation-only 状态答案，不能作为模型事实源。

禁用术语和口径：

- 不再把 HiCache state prediction 叫 replay；
- 不允许 `write_policy=observed` / `prefetch_policy=observed` / `storage_prefetch_policy=observed`；
- 不允许消费 source movement 修 target state；
- 不允许把 state snapshot 整体作为模型输入。

## 硬门槛

HiCache state prediction 必须同时满足：

| 门槛 | 要求 |
| --- | --- |
| invariant coverage | `invariant_coverage_ready=true`。 |
| missing invariant facts | `missing_invariant_facts=[]` 或 `{}`。 |
| illegal usage | `non_invariant_fact_usage=[]`。 |
| oracle | 有 oracle 时必须比较 final sets。 |
| final state | `final_state_match=true` 才能称为该场景 state 通过。 |
| DAG | state-only 阶段 `dag_mutations=0` 是预期。 |

只要 `non_invariant_fact_usage` 非空，即使 final state 偶然对齐，也不能宣称 invariant-only prediction 通过。

## 当前代码口径

截至 2026-06-12：

- HiCache profiling 使用 target-level atomic `fact` 契约，配合 token dictionary/span、`cache_scope` 和 `seq_no`；
- C++ backend 只消费 `model_input=true && fact_class=invariant_state && fact_granularity=atomic` 且 role 属于已知
  atomic invariant 的事件；
- 当前 profiling 契约保留 33 个 target，正常 state model input 是 7 个 target / 5 个 role：
  `request_bound_match_anchor`、`request_lifecycle_anchor`、`request_admission`、`prefetch_decision`、
  `prefetch_check_point`；
- 旧 `request_tokens`、`lookup_path`、`request_cache_lifecycle` 混合 role 已从主配置删除；match-prefix concrete path、
  lifecycle committed/fill path、runtime detail、insert/capacity/lock/maintenance 都是 `source_actual` 或
  `timing_observation` evidence；
- `sglang.hicache` probe 会自动采集 radix split/delete、evictable delta、host ref delta、node store/remove、load-back、
  write-back enqueue/start、write/load ack、storage control、storage hit query、prefetch rate-limit/terminate、abort cleanup
  等 source_actual provenance；
- `scripts/internal/hicache_state_cross_input_audit.py` 现在只比较 atomic invariant stream，逐 role 检查 count、sequence 和
  canonical fact value；
- HCSV-20260610 四向结果仍来自 12-target 旧 profile；`HCSV-20260612-async-elision-current-self-and-cross` 是 atomic
  contract 前的 retained audit 证据；33-target atomic 契约仍需重跑真实 S1A/S1B profile 和 cross audit；
- 2026-06-11 起，当前 `configs/modeling/hicache_state/modeling_hicache_state_mainline_one_prediction_s1*.json`
  已切到 fast-pressure 小容量口径；下面 HCSV-20260610 表格中的 `64/145`、`128/321` 是历史验证使用的
  archived target config，不是当前默认 modeling config；
- target pages 由 C++ 按 token path 和 target `page_size` 生成，不再消费 `target_page_identity_page64/128`；
- zero-token span 是合法空路径，不再被 backend 当作缺 token dictionary；
- `request_admission` 当前更新 request scoped token store，不直接产生 resident/dirty mutation；
- `maintenance_checkpoint`、`capacity_request` 和 `lock_scope_delta` 只作为 source evidence；若未来要用于正常模型，必须先定义
  新的 target-independent atomic invariant 或 target-derived 机制；
- legacy `maintenance_checkpoint` 处理仍不能消费 observed write/load ack 或 source host release page list；
- fact replay 使用严格全局时间顺序；同 timestamp/scope 下再用 `seq_no` 破 ties，避免 lock/ref delta 乱序；
- token radix tree 和 L1/L2 capacity enforcement 按 `cache_scope` 隔离；validation 默认仍用 `strip_scope` 的 normalized
  page hash union 与 oracle 对比；
- host eviction pressure 当前使用 token radix 派生出的 page-level compressed radix projection 选择 host leaf group，
  避免只按 insert full path leaf group 淘汰导致 parent host leaf 漏删；
- legacy `capacity_request` 不是 source victim oracle；如果历史 invariant trace 提供该 fact，backend 在同一
  capacity-pressure event 已成立的前提下按 target `page_size` 重算 requested pages，再按 modeled LRU/lock/radix 规则触发
  L1 eviction pressure；
- current cross-config 不能把 source `capacity_request` 视为 normal invariant input：修复前 audit 显示 S1A/S1B 不只是
  `requested_pages` 因 page size 不同，连 `requested_tokens` 和事件数量也不同；当前 config 保留它为 `source_actual`
  evidence；
- `prefetch_check_point` 在 `wait_complete` target 下不再直接把全部 planned pages 推入 L2/L3 或标记 ready；
- oracle page key 默认用 `oracle_page_key_mode=strip_scope` 和 raw snapshot hash 对比；
- HiCacheModule 仍是 state-only，不修改 DAG。

## 当前有效结果

### HCSV-20260612-atomic-input-contract

目的：把 HiCache state 正常输入契约从 mixed callable/event packets 收敛为 target-level atomic facts，避免把跨配置会变化的
source/cache-stage 事件混入 invariant role。

配置级结论：

| 项 | 当前结果 |
| --- | --- |
| configured target count | `33` |
| normal state input targets | `7` |
| normal input roles | `request_bound_match_anchor`, `request_lifecycle_anchor`, `request_admission`, `prefetch_decision`, `prefetch_check_point` |
| removed mixed roles | `request_tokens`, `lookup_path`, `request_cache_lifecycle` |
| evidence-only roles | `cache_stage_match_path_observed`, `request_lifecycle_path_observed`, `request_lifecycle_runtime_observed`, `insert_path`, `capacity_request`, `lock_scope_delta`, `maintenance_checkpoint` |
| cross audit hard gate | `model_input_contract_ready` |

修复口径：

- 正常模型输入只允许 target-independent facts 或 coarse checkpoint；
- `source_actual`、`timing_observation`、`oracle_state`、`debug_quality` 不更新 target state；
- cache-stage concrete match-prefix path、source `insert_path`、lifecycle generated/committed suffix、
  source `capacity_request`、source `lock_scope_delta` 和 source maintenance polling/check-kind 序列均不能作为正常 cross 输入；
- cross audit 不再保留旧 projection gate 作为正常输入豁免；它只比较 atomic invariant stream；
- C++ router 只接受 atomic invariant role，并移除了旧 mixed/source-control role 的正常入口。

本条是 config/audit 层结果，不是新的真实 S1A/S1B modeling 结果。下一步必须在 atomic profile config 下重跑
S1A/S1B，再用 `model_input_contract_ready=true` 作为 cross-config state-rule diagnosis 的前置条件。

### HCSV-20260612-async-elision-current-self-and-cross

目的：在 atomic contract 前的 fast-pressure S1A/S1B suite 上，区分 HiCache state model 的 deterministic bug 与
async/input-boundary 分岔；同时确认 cross-config 是否可以沿用 self-config 的逐 trace 注入方法。该结果是本次 demotion 的
历史证据，不能直接代表当前 33-target atomic 输入契约。

输入：

| 项 | 值 |
| --- | --- |
| suite | `data/profile_runs/sglang/20260611_054436_profiling_hicache_state_mainline_one_matrix` |
| S1A run | `01_s1a_manual` |
| S1B run | `03_s1b_manual` |
| S1A target config | page128, L1/L2 capacity `32/73`, `write_through_selective`, `wait_complete` |
| S1B target config | page64, L1/L2 capacity `32/81`, `write_back`, `best_effort` |

Self-config / cross-config final validation：

| prediction | output label | final | invariant coverage | non-invariant usage | normalized diff 摘要 |
| --- | --- | --- | --- | --- | --- |
| S1A self | `async_elision_current_s1a_self_capacity_target_pages_final` | pass | pass | `[]` | all active sets match：L1 `25/25`, L2/backuped `67/67`, dirty `0/0`, evicted `42/42`, locked `0/0` |
| S1B self | `async_elision_current_s1b_self_capacity_target_pages_final` | fail | pass | `[]` | L1/dirty/locked match；L2/backuped/evicted `56/55`, missing `13`, extra `14` |
| S1B self async-elided | `async_elision_current_s1b_async_elided_no_lock` | pass | pass | `[]` | all active sets match：L1 `28/28`, L2/backuped/evicted `55/55`, dirty `28/28`, locked `0/0` |
| S1A -> S1B | `async_elision_current_s1a_to_s1b_capacity_target_pages_final` | fail | pass | `[]` | L1/dirty/locked match；L2/backuped/evicted `56/55`, missing `13`, extra `14` |
| S1B -> S1A | `async_elision_current_s1b_to_s1a_capacity_target_pages_final` | fail | pass | `[]` | L2/backuped/dirty/locked match；L1 missing `13`; evicted extra `13` |

Capacity target page-size refinement：

- `capacity_request.requested_pages_source.requested_pages` 是 source page-size 下的页数；cross prediction 不能直接把它当
  target page count；
- 当前 C++ 在 fact 带 `requested_tokens` 时使用 target `page_size` 重算 requested pages，只有缺少 token count 时才回退到
  source `requested_pages`；
- `capacity_request` 仍只提供 eviction pressure，不提供 source victim list，victim 由 modeled LRU / lock / radix 规则决定；
- 这只是修正“同一个 capacity request 在不同 target page size 下应释放多少页”的局部语义，不能证明 cross
  capacity 输入已对齐。新增 `capacity_pressure_analysis` 后，真实两向 audit 都显示
  `page_size_only_explains_count=0`、`requested_tokens_differ_count=8`，并且另有 4 个 capacity event 只在一侧出现；
  首个 pair 是 S1A `requested_tokens=1057, requested_pages=9, page_size=128` vs S1B
  `requested_tokens=993, requested_pages=16, page_size=64`。所以 cross 的 capacity blocker 是 target/control-flow
  pressure sequence 问题，不是 source page count 换算问题；
- headroom/free-space 假设已被拒绝：只在 `resident_pages + requested_pages > capacity` 时才淘汰会让 S1A self
  从 pass 回退成 L1 extra `7` / evicted missing `7`，并扩大 S1B self 的 L2/backuped/evicted diff。

S1B self async-elision 细节：

- 对 `async_elision_current_s1b_self` 先排除 `locked_pages` 做 trace divergence scan：
  `trace_divergence_async_elided_no_lock.json`。
- Python diagnostic replay 识别并注入 `6` 个 async 分岔：
  - prefetch intent / checkpoint 附近的 host/storage state 可见性差；
  - prefetch storage completion 的 completed page list 不在 invariant input 中。
- Python replay 注入后仍出现 capacity 附近 L2/backuped/evicted extra `14`，不能单独作为模型 bug 结论。
- 用 `scripts/internal/hicache_state_async_elision.py` 生成 synthetic trace，再跑 C++ 模型侧 validation 后：
  - `validation_ready=true`
  - `final_state_match=true`
  - `missing_invariant_facts=[]`
  - `non_invariant_fact_usage=[]`
  - summary warning 明确标注这是 diagnostic oracle state injection，不是 normal prediction。

因此，当前 S1B self 的 final `13 missing / 14 extra` 应归类为 async/input-boundary 分岔后的连锁差异，而不是已发现的
deterministic final-set model bug。这里的 extra 不是“模型凭空 prefetch 额外页”的直接证据；它来自 async 分岔后，
后续 capacity、host pressure、eviction、cleanup 在不同基准状态上继续运行，导致真实系统释放/淘汰了模型仍保留的旧页。

S1A self 逐 trace 观察：

- final state 正常通过；
- trace scan 可见 maintenance 附近 `locked_pages` 暂态差异，oracle snapshot 仍显示锁，而模型按 invariant
  `lock_scope_delta` 已清掉；
- final locked 为 `0/0`，因此这类差异记录为 snapshot/input-boundary 暂态，不作为正常模型修复目标。

Cross-config 结论：

- 目前不能宣称“除 async 后 cross-config 没有其他问题”。
- S1A 与 S1B 是不同执行窗口：
  - S1A trace window `1781156739628126..1781158453440133`
  - S1B trace window `1781158517328888..1781160378311332`
- 因此不能把 target oracle snapshot 按 timestamp 注入 source trace；那会把 target 状态放到 source 执行窗口之外，得到假证明。
- cross 输入契约也尚未证明 target-independent：
  - S1A `capacity_request=8`，S1B `capacity_request=12`
  - S1A `lock_scope_delta=352`，S1B `lock_scope_delta=308`
  - `request_tokens` trace 到第 16 个 end 已出现 token count `768` vs `832`
- S1A->S1B 当前 L1/dirty/locked 已 match；L2/backuped/evicted 是 `56/55`，missing `13`、extra `14`，
  final diff 形态与 S1B self normal 一致。旧的 L1/dirty missing `4` 和 cross-only extra `14` 已被
  temporal anchor projection 与 target page-size capacity 修正消除。
- S1B->S1A 的 13 个页表现为 oracle final 在 L1、model final 留在 evicted；这是 cross-only resident/evicted lifecycle
  或输入边界问题，不能由 S1A self pass 自动排除。

已新增 cross logical alignment / input-contract 审计：

| audit | output | source/target event count | high-risk roles | result |
| --- | --- | ---: | --- | --- |
| S1A source -> S1B target | `async_elision_current_s1a_to_s1b_capacity_target_pages_final/cross_input_audit_s1a_to_s1b.json` | `2036/2000` | `insert_path`, `lookup_path`, `request_cache_lifecycle`, `request_tokens` | `cross_input_contract_ready=false` |
| S1B source -> S1A target | `async_elision_current_s1b_to_s1a_capacity_target_pages_final/cross_input_audit_s1b_to_s1a.json` | `2000/2036` | `insert_path`, `lookup_path`, `request_cache_lifecycle`, `request_tokens` | `cross_input_contract_ready=false` |

同时新增 front-door workload 审计，用来确认 benchmark 层输入是否一致：

| audit | output | request count | request shape | request sequence | policy | response observation | result |
| --- | --- | ---: | --- | --- | --- | --- | --- |
| S1A workload -> S1B workload | `async_elision_current_s1a_to_s1b/workload_input_audit_s1a_to_s1b.json` | `24/24` | match | match | `write_through_selective` vs `write_back` | differs from first `response_bytes` | `frontdoor_workload_ready=true` |
| S1B workload -> S1A workload | `async_elision_current_s1b_to_s1a/workload_input_audit_s1b_to_s1a.json` | `24/24` | match | match | `write_back` vs `write_through_selective` | differs from first `response_bytes` | `frontdoor_workload_ready=true` |

这说明 S1A/S1B 的 benchmark 入口请求形状和 prompt identity 一致：`sequence_id`、`phase`、`prompt_id`、
`prompt_chars`、`max_new_tokens` 都能逐项对齐。它排除了“workload 本来不同”这一层原因，但不等于
HiCache invariant state input stream 可直接 cross 使用。

审计结果显示 cross 不是简单的 async-elision 问题，也不是简单的事件 shape 不一致：

- `request_tokens` / `lookup_path` 两边都是 `150` 个 completed event，request binding shape 都是
  `100 request_bound + 50 unbound`；但第 16 个 event 已分岔，且两边都是 unbound：S1A 是 `768`
  tokens，S1B 是 `832` tokens，差异字段是 `path` 和 `token_count`；
- `insert_path` 两边都是 `100` 个 completed event，且全部 unbound；第 8 个 event 已分岔：S1A 是
  `768` tokens，S1B 是 `832` tokens，差异字段同样是 `path` 和 `token_count`；
- `insert_path` token count 分布不一致：S1A 侧主要是 `768 x64`、`896 x32`；S1B 侧主要是
  `832 x60`、`896 x32`、`768 x4`；
- `request_cache_lifecycle` 第 4 个 completed event 的 path hash 已不同；
- `capacity_request` 也不一致：S1A `8` 个 completed event，S1B `12` 个 completed event，且 requested tokens/pages 不同。

增强后的 contract layer 审计把分岔范围进一步缩小：

| layer | S1A->S1B | S1B->S1A | classification |
| --- | --- | --- | --- |
| `request_bound_match_prefix_paths` | `200/200`, aligned | `200/200`, aligned | `aligned` |
| `unbound_match_prefix_paths` | `100/100`, first mismatch `768` vs `832` | `100/100`, first mismatch `832` vs `768` | `cache_stage_path_mismatch` |
| `insert_paths` | `100/100`, first mismatch `768` vs `832` | `100/100`, first mismatch `832` vs `768` | `cache_stage_path_mismatch` |
| `request_lifecycle_paths` | `150/150`, token count 分布一致但 path hash 分岔 | `150/150`, token count 分布一致但 path hash 分岔 | `cache_stage_path_mismatch` |
| `source_control_flow_checkpoints` | `1486/1450` | `1450/1486` | `source_control_flow_or_async_boundary_mismatch` |

这条证据说明 request-scoped match-prefix 层不是当前 cross blocking 点；blocking 点集中在 unbound cache-stage path、
insert mutation path 和 lifecycle committed/fill path。

request-bound anchor 进一步说明 blocking layer 的性质不同：

- 两向 request-bound anchor 都有 `18` 个唯一 path，且这 `18` 个 path 都能找到 token ids；token counts 都是
  `5, 777, 832, 928`；
- 对 S1A，按 `source_page_size=128` 向下 page-align 后得到 `768, 896`；对 S1B，按 `source_page_size=64`
  向下 page-align 后得到 `768, 832, 896`；
- `unbound_match_prefix_paths` / `insert_paths` 大多能落到 exact request-bound path 或 token ids 级别验证的
  page-aligned request-bound prefix：
  - S1A->S1B source 是 `page_aligned_token_prefix=96, unanchored=4`，target 是
    `exact_request_bound_path=60, page_aligned_token_prefix=36, unanchored=4`；
  - S1B->S1A 方向相反；
- `request_lifecycle_paths` 两向都是 `unanchored=150`，说明它们不能只由 front-door prompt path 与 page size 推出。

temporal anchor 诊断继续把 unbound/insert 和 lifecycle 分开：

| layer | S1A->S1B source->target | S1B->S1A source->target | 结论 |
| --- | --- | --- | --- |
| `unbound_match_prefix_paths` | `100/100` temporal resolved：`96 exact + 0 candidate + 4 empty` | `100/100` temporal resolved：`36 exact + 60 candidate + 4 empty` | 可由 request-bound token facts + page size + cache-stage temporal anchor 解释；仍不能直接消费 source unbound path。 |
| `insert_paths` | `100/100` temporal resolved：`96 exact + 0 candidate + 4 empty` | `100/100` temporal resolved：`36 exact + 60 candidate + 4 empty` | 与 unbound match-prefix 同源，但它会直接改变 radix/cache state，下一步应改成 target-derived projection。 |
| `request_lifecycle_paths` | `0/150` temporal resolved，`150 unresolved` | `0/150` temporal resolved，`150 unresolved` | 不是 page-size 投影能解决的问题，需要高层 invariant、target lifecycle 推导或降级为 evidence。 |

新增 lifecycle suffix 诊断后，`request_cache_lifecycle` 的阻断性质更明确：

| metric | S1A->S1B | S1B->S1A | 结论 |
| --- | --- | --- | --- |
| request-bound request count | `50/50` | `50/50` | 每个 lifecycle request 都能在同 stream 找到 request-bound anchor。 |
| pair classification | `same_request_anchor_different_suffix=50`, `both_missing_lifecycle_token_ids=26`, `one_side_missing_lifecycle_token_ids=24` | 同左 | finished lifecycle path 的 prompt anchor 可对齐，但 committed/generated suffix 不同。 |
| first real lifecycle mismatch | index `4`, anchor `5` tokens same、suffix `9` tokens differs | index `4`, anchor `5` tokens same、suffix `9` tokens differs | lifecycle path 不是 page-size projection 缺口，而是 output/lifecycle suffix 输入契约缺口。 |

所以 `request_cache_lifecycle` 不能继续被当作 cross-safe token path 直接消费；它需要 target-independent 高层 invariant、
target-side lifecycle 推导，或降级为 source/target evidence。

新增 `projection_gate` 后的两向真实 audit：

| gate | S1A->S1B | S1B->S1A | 结论 |
| --- | --- | --- | --- |
| `projection_ready_layers` | `insert_paths`, `unbound_match_prefix_paths` | `insert_paths`, `unbound_match_prefix_paths` | 这两层可以作为 target-derived projection 的正常模型目标，不再作为 final-set 特化补丁入口。 |
| `contract_blocking_layers_after_projection` | `request_lifecycle_paths`, `source_control_flow_checkpoints` | `request_lifecycle_paths`, `source_control_flow_checkpoints` | lifecycle/control-flow 仍阻断 cross async-elision proof。 |
| `cross_input_contract_after_projection_ready` | `false` | `false` | cross 仍不能宣称排除 async 后通过。 |

修复前 `normal_model_input_contract` 将上述 gate 结论整理为正常模型可消费的输入契约。retained 两向真实 audit 都是：

- `contract_status=blocked_by_input_contract`；
- `input_contract_ready_for_cross_state_rule_diagnosis=false`；
- `input_contract_ready_for_non_async_correctness_claim=false`；
- `unsafe_roles_after_projection=[capacity_request, lock_scope_delta, maintenance_checkpoint, request_cache_lifecycle]`。

因此修复前 cross final-set mismatch 不能被解释成 deterministic state-rule bug；它首先说明当时的正常 cross 输入中仍混有
target/control-flow/lifecycle 边界事实。该历史 contract 只做诊断，不改变 C++ state model，不消费 oracle。

`after_projection_blockers` 将剩余 blocker 拆到 role：

| role | S1A->S1B count | S1B->S1A count | first mismatch | 当前处理方向 |
| --- | ---: | ---: | --- | --- |
| `request_cache_lifecycle` | `100/100` | `100/100` | index `4`, field `path` | lifecycle path 不是 page-size projection 能推出的输入，需要高层 invariant、target lifecycle 推导或降级为 evidence。 |
| `capacity_request` | `8/12`; `page_size_only=0`, `requested_tokens_differ=8`, missing `4` | `12/8`; `page_size_only=0`, `requested_tokens_differ=8`, missing `4` | index `0`, S1A `1057/9@128` vs S1B `993/16@64` | target capacity pressure sequence 需要由 target model 或高层 invariant 表达，不能消费 source `params.num_tokens` / source evict 调用序列。 |
| `lock_scope_delta` | `352/308`; inc/dec 都平衡 `176/176` vs `154/154`，net `0/0`; path mismatch `228`, direction mismatch `166`, missing `44` | `308/352`; 同左反向 | index `17`, S1A `inc` 768-token page-aligned prefix vs S1B empty-path `dec` | final locked 为 `0/0`，但 lock/ref delta 序列不是 cross-safe；需要 target radix/request lifecycle 推导或高层 invariant。 |
| `maintenance_checkpoint` | `1026/1030`; kind counts 只差 `maintenance_check` `592/596`; check_kind mismatch `348`, missing `4` | `1030/1026`; 同左反向 | index `108`, `maintenance_check` vs `ready_to_load_host_cache` | 主要是 maintenance polling/check_kind 调度错位，仍是 async/control-flow boundary。 |

因此，下一步可以把 unbound/insert path 作为“可由 request-bound token facts + target page-size + temporal anchor 派生”的模型推导目标；
但 lifecycle path 仍需要重新定义为高层 invariant、target-derived lifecycle，或明确降级为 source/target actual evidence。
修复前 C++ 对 `request_admission` / `request_cache_lifecycle` 只更新 request-scoped token store，不直接产生 resident/dirty/evicted
transition；所以 lifecycle 是 cross input-contract blocker，而不是围绕 final diff 追加 L1/L2/evicted 特化规则的理由。

cross-only final diff 当前仍不能归类为 deterministic state-rule bug。下一步必须先修正或重新定义 cross 输入契约：
核心 workload/token facts 要 target-independent；capacity、lock、maintenance 这类 source-control-flow facts 要么由 target model
推导，要么提升为更高层 invariant 输入。

修复前特别需要注意 `request_tokens` 的语义：它不是纯 HTTP prompt 记录，而是 `HiRadixCache.match_prefix(params.key)`
处的 key path。按 SGLang 调用点不同，它可能来自原始 request + 已生成 output、下一轮 fill 截断路径，或 cache-internal
page-aligned radix key。也就是说，workload report 一致但 `request_tokens` role 分岔并不矛盾；后者暴露的是修复前
HiCache invariant 输入流混入了 scheduler/cache-stage 状态，尚未满足 cross prediction 的 target-independent 输入契约。

后续动作已经并入本文“下一步”和 [hicache_state_model_defects.md](/home/markov/docs/validation/hicache_state_model_defects.md)。

### HCSV-20260611-s1b-host-node-projection-self

目的：使用已完成的 31-target `03_s1b_manual` profile，验证新 token-invariant backend 在 S1B self-config 下的当前状态。

输入：

| 项 | 值 |
| --- | --- |
| suite | `data/profile_runs/sglang/20260611_050859_profiling_hicache_state_mainline_one_matrix` |
| S1B run | `03_s1b_manual` |
| model config | `configs/modeling/hicache_state/modeling_hicache_state_mainline_one_prediction_s1b.json` |
| output | `03_s1b_manual/modeling/s1b_self_host_node_projection` |
| target config | page64, L1/L2 capacity `32/81`, `write_back`, `best_effort` |

运行摘要：

| item | value |
| --- | --- |
| `predicted_e2e_ns` | `8988548713` |
| `validation_ready` | `false` |
| `validation_errors` | `["hicache_final_state_mismatch"]` |
| `invariant_coverage_ready` | `true` |
| `missing_invariant_facts` | `[]` |
| `non_invariant_fact_usage` | `[]` |
| `skipped_non_invariant_events` | `2660` |
| `state_trace_events` | `5751` |
| `model_transition_events` | `5400` |

Normalized final-state diff：

| tier | model/oracle | missing | extra | status |
| --- | ---: | ---: | ---: | --- |
| L1 resident | 28/28 | 0 | 0 | match |
| dirty | 28/28 | 0 | 0 | match |
| L2 resident | 56/55 | 13 | 14 | mismatch |
| backuped | 56/55 | 13 | 14 | mismatch |
| evicted | 56/55 | 13 | 14 | mismatch |
| locked | 0/0 | 0 | 0 | match |

本轮已修复：

- invariant coverage 从 `token_dictionary_or_*` 和 `unimplemented_invariant_role.*` 误报恢复为 ready；
- zero-token request/lookup/insert/lock path 不再被误判为缺 token；
- lock/ref final set 从旧 S1B 31-target self 的 `26/0 extra` 修为 `0/0 match`，root cause 是 replay ordering；
- radix tree 和 capacity enforcement 已按 `cache_scope` 隔离；raw model final state 保留两个 cache scope，normalized
  final diff 与 scope 隔离前一致，说明当前 normalized mismatch 主因不在跨 scope 污染；
- `capacity_request` 不消费 source victim；当前按 `requested_tokens` 和 target `page_size` 重算 requested pages，
  再用 modeled LRU/lock/radix 选择 L1 victim；
- host eviction pressure 改用 page-level compressed radix projection，修复 parent host leaf group 漏删；
- 与前置 `s1b_self_scope_isolated` 相比，L1 从 `32/28` 收敛到 `28/28`，dirty 从 `31/28` 收敛到 `28/28`，
  L2/backuped 从 `81/55` 收敛到 `56/55`，evicted 从 `80/55` 收敛到 `56/55`。

逐 trace 分岔：

| item | value |
| --- | --- |
| last matched snapshot | `hicache_maintenance_check_end:state_snapshot`, order `4200` |
| last matched counts | L1 `28`, dirty `28`, L2/backuped/evicted `56` |
| first divergence | `hicache_prefetch_check_point_end:state_snapshot`, order `4204`, ts `1781155586790332` |
| scope | `rank:unknown:HiRadixCache:281452413813520` |
| model at divergence | L1 `28`, dirty `28`, L2/backuped/evicted `56` |
| oracle at divergence | L1 `28`, dirty `28`, L2/backuped/evicted `69` |
| immediate diff | L2/backuped/evicted missing `13`, extra `0` |

该分岔说明上一轮的 maintenance / host projection 问题已经被修掉；新的首个差异来自 storage prefetch completion 在真实
host radix 中插入 13 个 host-only pages，而当前 invariant checkpoint 只包含 `check_kind=progress`，没有完成 token 数或
完成 page list。

剩余结论：

- 当前不能宣称 S1B self state prediction 通过，因为 L2/backuped/evicted final sets 仍有 `13 missing / 14 extra`；
- 该 mismatch 不是输入分流问题：`missing_invariant_facts=[]` 且 `non_invariant_fact_usage=[]`；
- source-only 证据显示同 timestamp 的 `hicache_prefetch_progress_observed_start` 有 `completed_tokens=832`、
  `ready_pages_estimate=13` 和 13 个 `operation_hash_pages`，但该事件是 `fact_class=source_actual` 且
  不属于 atomic invariant model input；
- 不应新增“best_effort checkpoint 等于所有 pending prefetch 完成”的规则来硬凑 S1B；这会跨过 invariant 边界，
  也会把异步 storage progress 特化成该 trace 的偶然结果；
- 下一轮应把 async prefetch completion / storage lifecycle 作为集中建模或采集边界讨论，而不是继续在 host eviction、
  LRU 或 dirty writeback 上追加局部补丁。

### HCSV-20260610-four-way-s1a-s1b

目的：S1A 和 S1B profiling 均完成后，用同一批 token-invariant facts 做四个方向的 HiCache state prediction，
并用目标场景 oracle final state 验证模型正确性。

输入：

| 项 | 值 |
| --- | --- |
| profiling config | `configs/experiments/hicache_state/profiling_hicache_state_mainline_one_matrix.json` |
| S1A run | `20260610_073946_profiling_hicache_state_mainline_one_matrix/01_s1a_manual` |
| S1B run | `20260610_073946_profiling_hicache_state_mainline_one_matrix/03_s1b_manual` |
| S1A archived modeling config | page128, L1/L2 capacity `64/145`, `write_through_selective`, `wait_complete` |
| S1B archived modeling config | page64, L1/L2 capacity `128/321`, `write_back`, `best_effort` |
| current S1A modeling config | `configs/modeling/hicache_state/modeling_hicache_state_mainline_one_prediction_s1a.json`, fast-pressure capacity `32/73` |
| current S1B modeling config | `configs/modeling/hicache_state/modeling_hicache_state_mainline_one_prediction_s1b.json`, fast-pressure capacity `32/81` |
| archived target count | 12 |

Profile quality：

| run | `quality_ready` | `profiling_ready` | invariant events | required end events | token dictionary paths | missing token refs | seq errors | route errors |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| S1A | true | true | 6960 | 3480 | 172 | 0 | 0 | 0 |
| S1B | true | true | 6572 | 3286 | 145 | 0 | 0 | 0 |

四向 prediction / validation：

| prediction | source facts | target config / oracle | output label | `predicted_e2e_ns` | `validation_ready` | `final_state_match` | invariant coverage | non-invariant usage |
| --- | --- | --- | --- | ---: | --- | --- | --- | --- |
| S1A self | S1A | S1A | `modeling/four_way_s1a_self` | 10644954022 | false | false | true | `[]` |
| S1B self | S1B | S1B | `modeling/four_way_s1b_self` | 11833951018 | false | false | true | `[]` |
| S1A -> S1B | S1A | S1B | `modeling/four_way_s1a_to_s1b` | 10644954022 | false | false | true | `[]` |
| S1B -> S1A | S1B | S1A | `modeling/four_way_s1b_to_s1a` | 11833951018 | false | false | true | `[]` |

Normalized final-state diff：

| prediction | L1 resident | L2 resident | backuped | dirty | evicted | locked |
| --- | --- | --- | --- | --- | --- | --- |
| S1A self | 32/54, missing 22 | 80/106, missing 26 | 80/106, missing 26 | 0/0 match | 48/52, missing 26, extra 22 | 11/11 match |
| S1B self | 64/108, missing 44 | 170/144, missing 36, extra 62 | 170/144, missing 36, extra 62 | 64/72, missing 8 | 170/108, extra 62 | 0/22, missing 22 |
| S1A -> S1B | 64/108, missing 44 | 164/144, missing 40, extra 60 | 164/144, missing 40, extra 60 | 64/72, missing 8 | 164/108, missing 4, extra 60 | 22/22 match |
| S1B -> S1A | 32/54, missing 22 | 80/106, missing 26 | 80/106, missing 26 | 0/0 match | 48/52, missing 26, extra 22 | 0/11, missing 11 |

Raw model behavior highlights：

| prediction | state trace events | model transitions | skipped non-invariant | notable raw final state |
| --- | ---: | ---: | ---: | --- |
| S1A self | 7375 | 18253 | 416 | L1 64, L2 147, L3 712, evicted 83, locked 22, prefetch ready 36 |
| S1B self | 6867 | 27909 | 296 | L1 128, L2 321, dirty 128, evicted 321, locked 0, prefetch suppressed 1484 |
| S1A -> S1B | 6867 | 27465 | 416 | L1 128, L2 321, dirty 128, evicted 321, locked 44, prefetch suppressed 1452 |
| S1B -> S1A | 7375 | 18275 | 296 | L1 64, L2 147, L3 712, evicted 83, locked 0, prefetch ready 20 |

结论：

- 旧 token-invariant profile quality、token dictionary、seq order 和 invariant coverage 都通过；这只能证明旧输入分流方向有效，
  不能证明当前 33-target atomic 契约已经完成 validation。
- 后端输入分流有效：四个方向均无 `missing_invariant_facts` 和 `non_invariant_fact_usage`。
- 四个方向全部 final state mismatch，因此当前不能宣称 self-config 或 cross-config state prediction 通过。
- 已收紧两个有明确证据的过度推导：`capacity_request.requested_pages` 不指定 source victim，
  `wait_complete` checkpoint 不再全量构造 resident/ready。它们只小幅改善 S1A/S1B mismatch，说明剩余问题集中在
  victim/order、write-back flush、lock/ref chain 和 radix node/ref 近似。
- 失败是模型缺陷或当前 invariant 仍不可观测机制的证据；已知缺陷记录在 `hicache_state_model_defects.md`。
- S1B target 的 modeling config 必须设置 `require_oracle_state_trace=true`；否则 final mismatch 可能被错误标成 ready。

首个 S1A L1 normalized missing page：

```text
08c4433f3c8ddb201c1d2b54e9045b63308a491a20f6b8b6b6e4686b6cfd39be
```

该 page 也是 evicted extra sample，说明模型把部分 oracle-final L1 resident 页错误地留在 evicted 集合。

## 复现命令

Profile quality：

```bash
python3 scripts/internal/profile_quality.py \
  --manifest <run_dir>/profile_manifest.json \
  --output <run_dir>/profile_quality_token_backend.json
```

Modeling self-config：

```bash
python3 scripts/internal/model_runner.py \
  --config configs/modeling/hicache_state/modeling_hicache_state_mainline_one_prediction_<target>.json \
  --profile-manifest <target_run_dir>/profile_manifest.json \
  --output-dir <target_run_dir>/modeling/four_way_<target>_self \
  --mode cache_state \
  --emit-module-summary \
  --emit-validation
```

Modeling cross-config：

```bash
python3 scripts/internal/model_runner.py \
  --config configs/modeling/hicache_state/modeling_hicache_state_mainline_one_prediction_<target>.json \
  --profile-manifest <source_run_dir>/profile_manifest.json \
  --output-dir <source_run_dir>/modeling/four_way_<source>_to_<target> \
  --mode cache_state \
  --emit-module-summary \
  --emit-validation \
  --hicache-oracle-trace <target_run_dir>/trace/python_probe/python_probe_trace.rankunknown.pid*.json
```

Diff 摘要：

```bash
jq '.hicache_state.sets_diff_by_tier
  | to_entries[]
  | {tier: .key,
     match: .value.match,
     model_count: .value.model_count,
     oracle_count: .value.oracle_count,
     missing_count: (.value.missing_in_model | length),
     extra_count: (.value.extra_in_model | length),
     missing_sample: (.value.missing_in_model[:5]),
     extra_sample: (.value.extra_in_model[:5])}' \
  <output_dir>/validation.json
```

## 下一步

短期不应为了旧后端 mismatch 继续追加采集 target，也不应把修复前 retained audit 的 unsafe role 当作当前正常输入。当前顺序：

1. 用 33-target atomic profile config 重跑 S1A/S1B manual profile。
2. 运行 profile quality 和本地 config checks，确认正常 state input role 是当前 5 个 atomic invariant role。
3. 对两向 cross run 执行 `scripts/internal/hicache_state_cross_input_audit.py`，要求
   `model_input_contract_ready=true`。
4. 在输入契约通过后再跑 self-config / cross-config modeling validation。
5. 若 final state 仍 mismatch，再用逐 page provenance 区分 async boundary、target-derived projection 缺口和可修的
   C++ state rule bug。

只有在新契约下证明 5 个正常输入 role 仍不足以表达某个 target-independent 机制时，才进入下一轮集中新增或重定义 target。

## 新结果模板

```markdown
### HCSV-YYYYMMDD-<scope>

目的：

输入 / 配置：

运行摘要：

| prediction | final | invariant coverage | non-invariant usage | 结论 |
| --- | --- | --- | --- | --- |

失败定位：

后续动作：
```
