# HiCache State Validation 历史只读结论

本文只保存已经被当前 active 文档取代的旧批次结论，作为历史诊断和回归定位参考。
当前有效规则、约束、下一步主线和新结果记录入口见
[hicache_state_validation.md](hicache_state_validation.md)。

## 使用约束

- 本文只读，不追加新主线、新规则、新修复和新结果。
- 本文中的旧结果形成时，部分批次还没有 `non_invariant_fact_usage` 字段。
- 本文中的 `final_state_match=true`、`timeline match=true` 只能说明历史口径下的 final/timeline 对齐，不能单独证明
  invariant-only prediction 已闭环。
- 需要继续推进的结论必须回到 active 文档重新记录，并满足显式 target config、`non_invariant_fact_usage=[]`、
  `invariant_coverage_ready=true`、`final_state_match=true`。

## HCSV-20260608-state-matrix

历史定位：这是旧基准矩阵，覆盖 `I0`、`I1`、`I2`、`I3` 四类输入，以及 `C0-C7`、`write_back + low capacity`
共九类配置/组合。该矩阵在历史口径下用于证明 profile quality、page identity、final state 和 timeline coverage 的
基础能力。

当前解释：该矩阵仍可用于机制覆盖和回归定位，但不能作为 invariant-only 闭环证明，因为当时尚未记录
`non_invariant_fact_usage`。

### 输入集合

| input id | workload 形态 | 覆盖重点 |
| --- | --- | --- |
| `I0_full_mechanism` | `65` requests：A seed/reuse/backup、`pressure_B=16`、C prefetch seed/reuse。 | lookup、insert、load_back、evict、prefetch、write、lock 基础覆盖。 |
| `I1_capacity_pressure` | `81` requests：同 I0，但 `pressure_B=32`。 | clean eviction、低容量 L1/L2、write-back+capacity 组合。 |
| `I2_page_split` | `65` requests：`shared-prefix-repeat=192`、`unique-suffix-repeat=20`、`pressure_B=16`。 | page64 target identity、radix split、leaf group eviction、target suffix、prefetch completion rekey。 |
| `I3_prefetch_timing` | `43` requests：A 阶段缩小、`pressure_B=8`、`prefetch_seed=8`、`prefetch_reuse=10`、`phase_wait=4s`。 | timeout/suppressed timing、terminal progress、同输入分布跨 policy prediction。 |

### 配置集合

| config id | cache 配置 | 验证重点 |
| --- | --- | --- |
| `C0_base_timeout` | page128、write-through、timeout | 基线 self-config prediction 和 cross-config base facts。 |
| `C1_write_back` | page128、write-back、timeout | dirty、writeback、eviction 后 L2/backuped 对齐。 |
| `C2_write_through_selective` | page128、write-through-selective、timeout | hit_count、selective write、scope 隔离。 |
| `C3_page64` | page64、write-through、timeout | target page identity、radix split、lock/ref 边界。 |
| `C4_capacity` | page128、write-through、timeout、low capacity | clean eviction、capacity audit。 |
| `C5_prefetch_wait` | page128、write-through、wait_complete | planned/ready/suppressed 语义。 |
| `C6_prefetch_best_effort` | page128、write-through、best_effort | best_effort terminate 和 ready/suppressed 语义。 |
| `C7_prefetch_timeout_aggressive` | page128、write-through、timeout + explicit timeout params | aggressive timeout suppressed/ready 优先级。 |
| `C8_write_back_capacity` | page128、write-back、timeout、low capacity | dirty eviction + write-back + low capacity + prefetch transfer credit。 |

### Run Labels

| run label | input | config | workload |
| --- | --- | --- | --- |
| `20260608_052627_profiling_hicache_state_validation` | `I0` | `C0_base_timeout` | `65/65 ok` |
| `20260608_054834_profiling_hicache_state_write_back_validation` | `I0` | `C1_write_back` | `65/65 ok` |
| `20260608_061634_profiling_hicache_state_write_through_selective_validation` | `I0` | `C2_write_through_selective` | `65/65 ok` |
| `20260608_064000_profiling_hicache_state_page64_validation` | `I0` | `C3_page64` | `65/65 ok` |
| `20260608_090111_profiling_hicache_state_prefetch_wait_validation` | `I0` | `C5_prefetch_wait` | `65/65 ok` |
| `20260608_092411_profiling_hicache_state_prefetch_best_effort_validation` | `I0` | `C6_prefetch_best_effort` | `65/65 ok` |
| `20260608_095511_profiling_hicache_state_prefetch_timeout_aggressive_validation` | `I0` | `C7_prefetch_timeout_aggressive` | `65/65 ok` |
| `20260608_072559_profiling_hicache_state_capacity_base_validation` | `I1` | `C0_capacity_base` | `81/81 ok` |
| `20260608_080550_profiling_hicache_state_capacity_validation` | `I1` | `C4_capacity` | `81/81 ok` |
| `20260608_102444_profiling_hicache_state_write_back_capacity_validation` | `I1` | `C8_write_back_capacity` | `81/81 ok` |
| `20260608_111412_profiling_hicache_state_i2_page_split_base_validation` | `I2` | `C0_base_timeout` | `65/65 ok` |
| `20260608_114903_profiling_hicache_state_i2_page_split_page64_validation` | `I2` | `C3_page64` | `65/65 ok` |
| `20260608_123956_profiling_hicache_state_i3_prefetch_timing_base_validation` | `I3` | `C0_base_timeout` | `43/43 ok` |
| `20260608_125130_profiling_hicache_state_i3_prefetch_timing_timeout_aggressive_validation` | `I3` | `C7_prefetch_timeout_aggressive` | `43/43 ok` |

### 历史结论

- 所有列入该矩阵的 self-config prediction 和 cross-config prediction 在历史口径下达到 `final_state_match=true`。
- 所有列入该矩阵的 validation 在历史口径下达到 `timeline match=true` 且 `model_extra_transition_count=0`。
- profile page identity coverage 全部满足 `stateful_required_events_missing_page_identity=0`。
- `l3_resident_pages` 没有 full-set oracle，因此作为 unchecked diagnostic。
- timeline `exact_match=false` 主要来自 oracle-only transient，例如 `mark_evicted` / `clear_evicted` 或 snapshot 粒度差异。
- `I2_page_split` 暴露并修复了 page64 prediction 的 prefetch ready credit 缺口。
- page size 变化下 lock/ref event delta 不是逐 event exact；final locked set 在历史矩阵里对齐为 `0/0`。
- `I3_prefetch_timing` 是 suppressed-only timing 输入，profile quality 报缺 `prefetch_transfer`，不用于证明 transfer-ready 覆盖。

### 历史能力矩阵

| 能力 | 历史证据 | 历史结论 |
| --- | --- | --- |
| base self-config prediction | `I0/C0` final、event、timeline coverage 通过。 | 历史口径已验证。 |
| write-through -> write-back prediction | `I0 C0 -> C1` final 对齐，dirty `48/48`。 | 历史口径已验证。 |
| write-back target self-config prediction | `I0/C1` final 对齐，dirty `48/48`。 | 历史口径已验证。 |
| write-through-selective prediction | `I0 C0 -> C2` final 对齐，prefetch `166/8/158`。 | 历史口径已验证。 |
| page64 target self-config prediction | `I0/C3` 和 `I2/C3` final 对齐。 | 历史口径已验证；event delta lock/ref 边界保留。 |
| page64 strict prediction | `I0 C0 -> C3`、`I2 C0 -> C3` final 对齐，timeline coverage 通过。 | 历史口径已验证；I2 暴露并修复 transfer completion rekey。 |
| capacity prediction | `I1 C0 -> C4` final 对齐，L1 `46/46`、L2 `96/96`。 | 历史口径已验证。 |
| write-back + low capacity prediction | `I1 C0 -> C8` final 对齐，dirty `38/38`、ready `8/8`。 | 历史口径已验证；需要 `write_back_prefetch_transfer_credit=true`。 |
| prefetch wait prediction | `I0 C0 -> C5` final 对齐，ready `8/8`。 | 历史口径已验证。 |
| prefetch best_effort prediction | `I0 C0 -> C6` final 对齐，timeline exact `true`。 | 历史口径已验证。 |
| aggressive timeout prediction | `I0 C0 -> C7` 与 `I3 C0 -> C7` final 对齐。 | 历史口径已验证。 |
| timeline coverage | 当前矩阵 self-config / cross-config prediction `timeline match=true` 且 `model_extra_transition_count=0`。 | 历史口径已验证。 |
| timeline exact | 多数 run `exact_match=false`，主要为 oracle-only transient。 | 未作为验收闭环。 |

## HCSV-20260609-mainline-one-manual-l1

历史定位：这是主线一 `L1_manual_phased` 输入下的 S1A/S1B 验证。该批次的 profiling quality 有价值；
self-config prediction 的历史通过结论已经被 active 文档按当前 invariant-only 口径降级。

### 配置与 Profile Quality

| 项 | `S1A_baseline_large` | `S1B_divergent_large` |
| --- | --- | --- |
| run label | `20260609_053538_profiling_hicache_state_mainline_one_manual_s1a` | `20260609_073205_profiling_hicache_state_mainline_one_manual_s1b` |
| config | page128、L1/L2 `64/145`、`write_through_selective`、`wait_complete`、ratio `2.25` | page64、L1/L2 `128/321`、`write_back`、`best_effort`、ratio `2.5` |
| profile quality | `quality_ready=true`、23/23 targets observed、missing mechanisms `[]` | `quality_ready=true`、23/23 targets observed、missing mechanisms `[]` |
| page identity coverage | stateful required `2126/2126`，missing `0` | stateful required `1804/1804`，missing `0` |
| target oracle coverage | events `15635`、snapshots `7531`、target hash nodes `87947`、target radix removed `32` events / `1072` pages | events `13840`、snapshots `6640`、target hash nodes `88944`、target radix removed `46` events / `864` pages |
| mechanism 摘要 | evict `118`、insert `336`、load_back `216`、lock_ref `1266`、lookup `504`、prefetch transfer `16`、write `294/288` | evict `112`、insert `336`、load_back `218`、lock_ref `1072`、lookup `504`、prefetch transfer `16`、write `214/198` |

### 历史 Self-Config 摘要

该表保留旧 validation 口径下的历史摘要。active 文档中的 `HCSV-20260609-code-audit-new-invariant`
已经用当前口径重新分类，不能再把下表当作当前通过证据。

| self-config prediction | 历史 validation | state counts | event delta | timeline |
| --- | --- | --- | --- | --- |
| `S1A` | pass | L1 `54/54`、L2 `106/106`、dirty `0/0`、backuped `106/106`、evicted `52/52`、locked `1/1`、prefetch `356/18/338` | match `false`、mismatch `168`，集中在 lock/ref 归因粒度 | match `true`、exact `false`、model-extra `0`、oracle-only `2924` |
| `S1B` | pass | L1 `108/108`、L2 `144/144`、dirty `72/72`、backuped `144/144`、evicted `108/108`、locked `0/0`、prefetch `742/36/706` | match `false`、mismatch `169`，集中在 lock/ref 归因粒度 | match `true`、exact `false`、model-extra `0`、oracle-only `4860` |

### 历史 Cross-Config 摘要

| prediction | 历史 validation | state counts / diff | timeline |
| --- | --- | --- | --- |
| `S1A -> S1B` | fail：`hicache_final_state_mismatch` | model/oracle：L1 `108/108`、L2 `125/144`、dirty `108/72`、backuped `125/144`、evicted `118/108`、prefetch `728/18/710` vs `742/36/706`；diff 为 L2 missing `36` / extra `17`、backuped missing `36` / extra `17`、dirty extra `36`、evicted extra `10`、prefetch planned missing `14`、ready missing `25` / extra `7`、suppressed missing `7` / extra `11` | match `false`、exact `false`、model-extra `72`、oracle-only `152` |
| `S1B -> S1A` | fail：`hicache_final_state_mismatch` | 除 `locked_pages` 外 final state 全部对齐；唯一 diff 是 model `0`、oracle `1`，missing page `1` | match `true`、exact `false`、model-extra `0`、oracle-only `36` |

### 历史逐 Trace 对比结论

- `S1A -> S1B` 的主 mismatch 不是 page identity 缺口：`missing_page_identity_events=0`。
- 主 mismatch 集中在同一组 `36` 页：这些页同时满足 `extra_dirty`、`missing_l2`、`missing_backuped`。
- 逐页对齐 `S1B` 历史 self-config prediction 后，`36/36` 页都有 `hicache_write_backup_end -> add_l2_resident + mark_backuped + clear_dirty`。
- 直接消费 source `write_backup` / `write_storage` observed movement 不能作为修复，因为 source policy 和 target policy 不等价。
- `S1B -> S1A` 的唯一缺口是 page-size what-if 下 lock/ref 被视为 non-invariant observed movement，真实 `S1A` final oracle
  还有一个 locked page。

## 早期 S1A 手工输入排查

ratio `2.0` 的 `S1A_baseline_large + L1_manual_phased` 只保留为 radix removed materialization 和显式 target config
口径排查结论，不作为当前主线一完成证据。

| 项 | 结果 |
| --- | --- |
| run label | `20260608_203828_profiling_hicache_state_mainline_one_manual_s1a` |
| workload | `83/83 ok`、`errors=0` |
| profile quality | `quality_ready=true`，23/23 Python targets observed，stateful required page identity `2130/2130`，缺失 `0` |
| 机制覆盖 | evict `122`、insert `336`、load_back `216`、lock_ref `1266`、lookup `504`、prefetch transfer `16`、write `294/288` |
| radix removed materialization | runner 收尾从同一次 insert start/end validation snapshot materialize `2` 个 `hicache_insert_end`，每个 `13` 页，合计 `26` 页 |
| 排查结论 | operation-level radix materialization 后 final state 可对齐；该结果只说明当时的 radix fact 缺口已经定位 |
| timeline diagnostic | `match=true`、`model_extra_transition_count=0`、`exact_match=false`、`oracle_extra_transition_count=2946` |
| event delta | `match=false`、`mismatch_count=168`，剩余差异集中在 lock/ref transition 粒度 |

## 历史关键修复

### Aggressive Timeout Lock/Ref 非不变量

显式 `best_effort` 或 aggressive `timeout` 会改变 prefetch 终止点。base policy 下观测到的 prefetch lock/ref
start/end 对数不是 target timeline 不变量。历史修复是在 target 为 `best_effort` 或带 timeout 参数的 `timeout`
时跳过 base lock/ref movement。

### Write-Back + Low Capacity Transfer Credit

普通 `write_through -> write_back` prediction 不能消费 base write-through 下的 L3->L2 prefetch transfer；
但 `write_back + low capacity` target 需要 transfer ready evidence。历史修复新增：

```json
"write_back_prefetch_transfer_credit": true
```

该字段只应在明确需要 transfer credit 的组合中启用。

### Page64 Prefetch Completion Rekey

`I2 C0 -> C3_page64` 曾失败，prefetch ready `26/40`、suppressed `536/522`。原因是 page size what-if 下，
base transfer 的 `target_page_identity` 可能因为 base prefetch 的 last_hash 或 parent context 与 target schedule identity
不一致。历史修复改成：只要 transfer source pages 已覆盖同 request 的 schedule source pages，就用 target schedule pages
作为 completion credit。

## 历史 Timeline Exact 判断

历史 validation 都强调：

```text
timeline match=true
model_extra_transition_count=0
```

`exact_match=false` 的主要来源：

- raw snapshot 不是完整状态日志，而是稀疏采样；
- 多进程场景中，某个 cache object 长时间未被采样时，下一次 HiCache 调用的 snapshot 可能暴露之前发生的状态变化；
- oracle-only `mark_evicted` / `clear_evicted` transient 常见；
- write-through dirty transient 可能在 completed snapshot 中不可见；
- page64 lock/ref 父链归因无法用当前 snapshot/probe 粒度逐 event exact 对齐。

历史判断：`exact_match=false` 不是直接的 state model 错误；真正要验证 operation-level exact，需要新增 ordered transition oracle。
