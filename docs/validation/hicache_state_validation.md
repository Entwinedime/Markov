# HiCache State Validation

维护方式：本文是 HiCache state validation 的唯一 active 文档，直接维护当前有效口径、最新结果、仍未证明的风险和复现入口。历史实验只保留能解释当前边界的压缩结论；不再维护单独的缺陷清单文件。

## 目标

本阶段只验证一个问题：

```text
base profiling invariant facts + explicit target cache config
  -> C++ HiCache state model
  -> predicted target cache state
  -> compare with oracle state snapshot
```

它不是 DAG patch 验收，也不是 E2E 性能预测验收。`prediction.json.predicted_e2e_ns` 只能作为 runner / DAG sanity check，不能证明 HiCache state 正确。

## 硬门槛

HiCache state prediction 必须同时满足：

| 门槛 | 要求 |
| --- | --- |
| invariant coverage | `hicache_state.invariant_coverage_ready=true`。 |
| missing invariant facts | `hicache_state.missing_invariant_facts=[]` 或 `{}`。 |
| illegal usage | `hicache_state.non_invariant_fact_usage=[]`。 |
| validation | `validation_ready=true` 且 `validation_errors=[]`。 |
| final state | 有 oracle 时必须 `hicache_state.final_state_match=true` 才能称该场景 state 通过。 |
| DAG | state-only 阶段 `dag_mutations=0` 是预期，不代表 DAG patch 已实现。 |

只要 `non_invariant_fact_usage` 非空，即使 final state 偶然对齐，也不能宣称 invariant-only prediction 通过。

## 当前输入契约

截至 2026-06-13，HiCache state 主线使用 target-level atomic fact contract。

- C++ backend 只消费 `model_input=true && fact_class=invariant_state && fact_granularity=atomic`，且 role 属于已知 atomic invariant。
- 当前 33-target suite 的正常 state model input 是 7 个 target / 5 个 role：
  `request_bound_match_anchor`、`request_lifecycle_anchor`、`request_admission`、`prefetch_decision`、`prefetch_check_point`。
- `source_actual`、`timing_observation`、`oracle_state`、`debug_quality` 不更新 target state；它们只用于 token dictionary 水合、provenance、质量检查和审计。
- 旧 mixed/source-control role 不再是 normal input：`request_tokens`、`lookup_path`、`request_cache_lifecycle`、`insert_path`、`capacity_request`、`lock_scope_delta`、`maintenance_checkpoint` 都不能进入 normal mutation path。
- source matched result、source admission return、actual victim、actual movement、actual async completion、node remove result、storage hit result、host ref delta 等 source 已发生结果不得作为 `invariant_state` 字段混入。
- raw `request_id` 只是单次运行内 correlation id；cross audit 必须使用 request-normalized canonical fact，不把 raw id 当跨配置 invariant。
- `page_identity`、`target_page_identity`、`target_page_identity_page<page_size>` 不再是 state model 主输入；target page identity 由 token dictionary/span、`hash_algo`、`cache_scope` 和 target `page_size` 推导。

## 当前模型边界

C++ state model 当前已经拆成三条状态线：

- `DeviceCacheState`：device radix、L1、request device lock/ref、active admission reservation。
- `HostCacheState`：host radix、host ref/protection、storage-known、ready-but-not-visible、host-visible。
- `AsyncState`：prefetch work 的 pending / ready / applied / suppressed / late lifecycle，并区分 `requested_host_pages` 和 `reserved_host_pages`。

当前已实现的 target-derived 机制：

- Request/admission：
  - `request_bound_match_anchor` 做 target lookup / touch / loadback；
  - `request_admission` 根据 target radix match 派生 active device lock/ref 和 admission pressure；
  - `request_lifecycle_anchor` 在 unfinished / finished 上 insert、迁移或释放 active request lock/ref。
- Device capacity：
  - capacity pressure 来自 target config、target page size、request state 和 active reservation；
  - victim 由 modeled radix leaf group、LRU/touch order 和 active lock/ref eligibility 推导；
  - 不消费 source `capacity_request`、source victim 或 source evicted token count。
- Host/device/async：
  - L2/backuped/host-visible mutation 只能通过 `add_host_visible_page()` / `remove_host_visible_page()`；
  - `add_resident()` / `remove_resident()` 不再对 L2 隐式写 host topology、backuped 或 host-visible；
  - request lookup 分别维护 device radix match、host radix match 和 target-visible prefix，避免把 device resident、host-visible、storage-known 混成一个 `matched_pages`。
- SGLang-derived host release / cleanup：
  - host prefetch allocation 失败时按本次 page-aligned prefetch request 调用 host eviction，对齐 SGLang `evict_host(prefetch_length)`；
  - cleanup budget 不按最终 L2 count、deficit 或 fallback reserved count 反推；
  - storage prefetch threshold / capacity limit 来自显式 target config；未配置时分别按 SGLang
    `max(prefetch_threshold=256, page_size)` tokens 和 `floor(0.8 * (host_pool_pages - device_pool_pages))` 源码公式投影；
  - rate-limit 判断保持 SGLang `prefetch_tokens_occupied >= prefetch_capacity_limit` 语义，capacity limit 为 0 时不被当作无限制；
  - `prefetch_decision` 中 `planned_pages` 表示 page-aligned prefetch key，`pages` 表示 storage hit query 后保留的连续命中前缀；
  - prefetch anchor 在 work 生命周期内纳入 modeled host ref/protection，terminate、revoke、late 或 finalize 时释放；
  - host victim 必须是 host radix leaf、host-visible、`evicted`、无 host ref / lock protection，且不能有 backuped child；
  - alloc 第二次仍不足时按 host pool available pages 降级，低于 prefetch threshold 则放弃；
  - `prefetch_check_point` 对齐 SGLang `check_prefetch_progress()` 生命周期边界：wait_complete 完成后 apply ready pages，
    best_effort 在 checkpoint terminate，timeout 在 completed/timeout 后 terminate 或 late；未 ready / revoked work suppress，
    未发出 work 不产生 prefetch state，并释放 reservation 与 anchor protection。
- Write-back：
  - 不再保留错误的 page-level `ensure_host_pages_for_write()` 前置释放路径；
  - write-back 当前把 ACK 时序折叠为同步 enqueue/complete，只建模 dirty clear、host-visible 和 storage-readable 结果，不把 source writeback ack 当 normal state input。

## 当前有效结果

### HCSV-20260613-host-release-policy-final3

这是当前 active 结论。它基于同一组 33-target atomic S1A/S1B profile：

```text
data/profile_runs/sglang/20260612_053153_profiling_hicache_state_mainline_one_matrix
```

输入与质量：

| 项 | S1A | S1B |
| --- | --- | --- |
| run | `01_s1a_manual` | `03_s1b_manual` |
| suite status | `failures=[]` | `failures=[]` |
| profiling ready | true | true |
| Python probe traces | 2 | 2 |
| Python probe events | 13146 | 12924 |
| completed normal model-input facts | 350 | 350 |
| cross audit `model_input_contract_ready` | true | true |

四向 prediction 输出：

| prediction | output |
| --- | --- |
| S1A self | `01_s1a_manual/modeling/host_release_policy_final3_s1a_self` |
| S1B self | `03_s1b_manual/modeling/host_release_policy_final3_s1b_self` |
| S1A -> S1B | `01_s1a_manual/modeling/host_release_policy_final3_s1a_to_s1b` |
| S1B -> S1A | `03_s1b_manual/modeling/host_release_policy_final3_s1b_to_s1a` |

硬门槛结果：

| prediction | validation | invariant coverage | missing invariant facts | non-invariant usage | final |
| --- | --- | --- | --- | --- | --- |
| S1A self | ready, errors `[]` | true | none | `[]` | pass |
| S1B self | ready, errors `[]` | true | none | `[]` | pass |
| S1A -> S1B | ready, errors `[]` | true | none | `[]` | pass |
| S1B -> S1A | ready, errors `[]` | true | none | `[]` | pass |

normalized final active sets：

| target | source profile | L1 | dirty | L2 | backuped | evicted | locked | 结论 |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| S1A | S1A | `25/25` | `0/0` | `67/67` | `67/67` | `42/42` | `0/0` | final match |
| S1A | S1B | `25/25` | `0/0` | `67/67` | `67/67` | `42/42` | `0/0` | final match |
| S1B | S1B | `28/28` | `28/28` | `55/55` | `55/55` | `55/55` | `0/0` | final match |
| S1B | S1A | `28/28` | `28/28` | `55/55` | `55/55` | `55/55` | `0/0` | final match |

补充状态：

- target=S1A model final 还包含 `l3_resident_pages=74`、`prefetch_planned_pages=74`、`prefetch_ready_pages=0`、`prefetch_suppressed_pages=74`。
- target=S1B model final 还包含 `l3_resident_pages=13`、`prefetch_planned_pages=150`、`prefetch_ready_pages=13`、`prefetch_suppressed_pages=137`。
- 当前 validation 的 unchecked model state keys 仍包括 `l3_resident_pages`、`prefetch_planned_pages`、`prefetch_ready_pages`、`prefetch_suppressed_pages`；这些不是本轮 active final-state oracle 的通过条件。

结论：

- 当前 S1A/S1B self-config 和 cross-config 的 active final state 已闭环。
- S1B 之前的 L2/backuped/evicted `70/55` residual 已关闭；最终收敛为 `55/55`。
- 该闭环不依赖 source_actual、timing_observation、oracle result 或 node remove/source completion shortcut。
- 不再把 `HCSM-D11`、`70/55` 或 `56/55` 作为当前 active defect；这些仅保留为历史阶段结果。

## S1B Host Residual 审计结论

修复前的 S1B host residual 审计回答了两个问题：

- missing 13：全部是 best-effort prefetch ready-but-not-visible 候选，不能单独用 ready->L2 特化规则处理。
- extra 28：主要是缺 host release / cleanup；若只把 ready pages apply 到 L2，会在已有 `70/55` 基础上继续增加 host-visible pages。

当前实现将两者放到同一个 target-derived transaction：

1. best-effort `prefetch_check_point` 判断 ready work；
2. ready work 通过 `HostVisibilityApply` 插入 host radix、写 L2/backuped/L3，并保持 `evicted`；
3. checkpoint 释放 prefetch reservation；
4. host capacity / alloc-fail cleanup 按 SGLang `evict_host(prefetch_length)` 预算清理 host-visible evicted leaf；
5. write-back cleanup 不在 page-level 前置路径中提前释放。

这就是 S1B 从 host-device-boundary 阶段 `70/55` 收敛到 final3 `55/55` 的机制边界。

## 当前剩余风险

当前 final-state pass 不等于完整 SGLang HiCache 仿真。仍未证明或仍为近似的部分如下：

| 风险 | 当前状态 | 影响 |
| --- | --- | --- |
| Async prefetch exact progress | storage hit / revoke / anchor protection 已按 target-derived state 建模，但 transfer completion 仍不消费 source `completed_tokens` / completion pages | 多个 overlapping prefetch、partial completion 或复杂 terminate 条件下，`prefetch_ready/suppressed` 与中间 L2 时序可能不精确。 |
| Host ref / protection node lifetime | prefetch anchor 已纳入 page-level host ref/protection，但仍不是 SGLang `TreeNode.host_ref_counter` 的节点级完整等价 | ongoing prefetch / backup / loadback 交错时，host eviction eligibility 可能偏。 |
| Host victim ordering | host cleanup candidate 已对齐，但 priority heap、node last access、parent promotion tie-break 仍是投影 | 更复杂 radix split/delete 或同优先级 victim 下，victim identity 仍需逐 trace 证明。 |
| Write-back batch state machine | 已折叠为同步 completion 语义，并与 `write_through` / `write_through_selective` 共享同一组 host backup / eviction helper | 仍不追踪真实 ack 异步时序；write-back 与 prefetch、host release、device eviction 交错时的 transition exactness 仍需后续 targeted validation。 |
| Transition exact oracle | 当前主要证明 final normalized active sets，不证明每一步 transition 与 oracle 同序同因 | 后续做 DAG patch 或性能归因前，需要 transition provenance / exact oracle。 |
| Scope-normalized comparison | validation 默认看 `strip_scope` page hash union，C++ 内部按 `cache_scope` 隔离 | 多 scope 同 hash 或 scope-local victim 问题可能被 normalized final 掩盖。 |
| State-to-DAG patch | `HiCacheModule` 仍是 state-only，`dag_mutations=0` 是预期 | state 通过后仍需要单独设计 cache state -> DAG mutation。 |

## 历史阶段摘要

这些结果只用于解释当前设计，不作为 active 失败结论。

### HCSV-20260612-atomic-input-contract

- 33-target atomic profile 完成，S1A/S1B normal model input 都是 350 个 completed atomic invariant facts。
- 双向 cross audit `model_input_contract_ready=true`，blocking roles 为空。
- 旧 mixed roles 被拆掉：`request_tokens`、`lookup_path`、`request_cache_lifecycle` 不再是 normal role。
- source/control-flow 事实降级为 evidence：`insert_path`、`capacity_request`、`lock_scope_delta`、`maintenance_checkpoint` 等不更新 target state。

### HCSV-20260612-backend-atomic-refactor

- C++ backend 改为 router enum dispatch，删除 legacy handlers：`apply_maintenance_checkpoint`、`apply_capacity_request`、`apply_lock_scope_delta`。
- 四向 prediction 都达到 `invariant_coverage_ready=true`、`missing_invariant_facts=[]`、`non_invariant_fact_usage=[]`。
- 当时 final 尚未通过：S1A target 主要是 L1 extra / evicted missing；S1B target 是 L2/backuped/evicted `70/55`。
- 该阶段证明问题不在输入没有进入 C++，而在 target-derived resource / host lifecycle mechanism 尚未实现。

### HCSV-20260612-target-resource-mechanism

- 实现 request-derived device lock/ref、admission reservation、target L1 capacity pressure 和 dynamic device radix leaf victim。
- S1A self/cross 收敛到 final match：L1 `25/25`、L2/backuped `67/67`、evicted `42/42`。
- S1B target 从 `70/55` 收敛到 `56/55`，剩余集中在 host/L2。
- 该阶段关闭了 device-side resource mechanism 的主要缺口，但没有闭合 host/prefetch/storage visibility。

### HCSV-20260613-host-device-boundary-refactor

- 拆出 `DeviceCacheState`、`HostCacheState`、`AsyncState`；
- 删除 L2 mutation 的隐式 side effect；
- best-effort ready 保留为 ready-but-not-visible，避免 premature ready->L2 污染 device pressure。
- 当时 S1B target 仍为 `70/55`，这是故意暴露 host release / cleanup 缺口的中间态，不是当前最终结果。

### HCSV-20260611 / HCSV-20260610 retained audits

- 31-target / 12-target 旧结果只作为历史诊断，不代表当前 33-target atomic 输入契约。
- 这些 audit 证明过 source `capacity_request`、`lock_scope_delta`、`maintenance_checkpoint`、`request_cache_lifecycle` 不是 cross-safe normal input。
- 旧 async-elision injection 只用于定位 async/input-boundary，不是 normal prediction 的可消费输入。

## 复现命令

基础检查：

```bash
scripts/run.sh modeling -- bash -lc \
  'cmake -S . -B build/modeling -G Ninja && cmake --build build/modeling --target trace_graph -j2'
scripts/run.sh modeling -- bash -lc \
  'python3 -m py_compile scripts/internal/model_runner.py scripts/internal/hicache_state_cross_input_audit.py scripts/internal/hicache_state_provenance.py'
find configs -name '*.json' -print0 | xargs -0 -n1 jq empty
git diff --check
```

S1A self：

```bash
scripts/model.sh \
  --config configs/modeling/hicache_state/modeling_hicache_state_mainline_one_prediction_s1a.json \
  --profile-manifest data/profile_runs/sglang/20260612_053153_profiling_hicache_state_mainline_one_matrix/01_s1a_manual/profile_manifest.json \
  --output-dir data/profile_runs/sglang/20260612_053153_profiling_hicache_state_mainline_one_matrix/01_s1a_manual/modeling/host_release_policy_final3_s1a_self \
  --mode cache_state \
  --emit-module-summary \
  --emit-validation
```

S1B self：

```bash
scripts/model.sh \
  --config configs/modeling/hicache_state/modeling_hicache_state_mainline_one_prediction_s1b.json \
  --profile-manifest data/profile_runs/sglang/20260612_053153_profiling_hicache_state_mainline_one_matrix/03_s1b_manual/profile_manifest.json \
  --output-dir data/profile_runs/sglang/20260612_053153_profiling_hicache_state_mainline_one_matrix/03_s1b_manual/modeling/host_release_policy_final3_s1b_self \
  --mode cache_state \
  --emit-module-summary \
  --emit-validation
```

S1A -> S1B：

```bash
scripts/model.sh \
  --config configs/modeling/hicache_state/modeling_hicache_state_mainline_one_prediction_s1b.json \
  --profile-manifest data/profile_runs/sglang/20260612_053153_profiling_hicache_state_mainline_one_matrix/01_s1a_manual/profile_manifest.json \
  --hicache-oracle-trace data/profile_runs/sglang/20260612_053153_profiling_hicache_state_mainline_one_matrix/03_s1b_manual/trace/python_probe/python_probe_trace.rankunknown.pid2488.json \
  --hicache-oracle-trace data/profile_runs/sglang/20260612_053153_profiling_hicache_state_mainline_one_matrix/03_s1b_manual/trace/python_probe/python_probe_trace.rankunknown.pid2489.json \
  --output-dir data/profile_runs/sglang/20260612_053153_profiling_hicache_state_mainline_one_matrix/01_s1a_manual/modeling/host_release_policy_final3_s1a_to_s1b \
  --mode cache_state \
  --emit-module-summary \
  --emit-validation
```

S1B -> S1A：

```bash
scripts/model.sh \
  --config configs/modeling/hicache_state/modeling_hicache_state_mainline_one_prediction_s1a.json \
  --profile-manifest data/profile_runs/sglang/20260612_053153_profiling_hicache_state_mainline_one_matrix/03_s1b_manual/profile_manifest.json \
  --hicache-oracle-trace data/profile_runs/sglang/20260612_053153_profiling_hicache_state_mainline_one_matrix/01_s1a_manual/trace/python_probe/python_probe_trace.rankunknown.pid417.json \
  --hicache-oracle-trace data/profile_runs/sglang/20260612_053153_profiling_hicache_state_mainline_one_matrix/01_s1a_manual/trace/python_probe/python_probe_trace.rankunknown.pid418.json \
  --output-dir data/profile_runs/sglang/20260612_053153_profiling_hicache_state_mainline_one_matrix/03_s1b_manual/modeling/host_release_policy_final3_s1b_to_s1a \
  --mode cache_state \
  --emit-module-summary \
  --emit-validation
```

结果摘要：

```bash
jq '{validation_ready,
     validation_errors,
     final_state_match: .hicache_state.final_state_match,
     counts: .hicache_state.normalized_model_final_state_counts,
     oracle: .hicache_state.normalized_oracle_final_state_counts,
     diff: .hicache_state.sets_diff_by_tier}' \
  <output_dir>/validation.json
```

## 下一步

当前不需要继续围绕 S1B final count 打补丁。后续优先级：

1. 做 transition-level / provenance audit，确认 final-state pass 下的中间时序误差范围。
2. 若要进入 DAG patch，先定义 cache state -> DAG mutation 的可验证接口和最小验收场景。
3. 对 async prefetch exactness、host node ref lifetime、write-back batch 时序分别设计 targeted validation；只有现有 target-derived 机制无法表达时，才讨论新增 target-independent atomic metadata。
4. 保持 source_actual / timing_observation / oracle 只做 evidence，不回到 normal state mutation。
