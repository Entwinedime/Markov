# HiCache 状态验证

维护方式：本文是 HiCache state validation 的 active 文档，直接维护当前有效口径、当前保留基线、仍未证明的风险和复现入口。历史实验只保留能解释当前边界的压缩结论；尚未完成的短期根因分析可暂存于 `docs/tmp/`。

## 目标

本阶段验证的问题是：

```text
base profiling state-model facts + explicit target cache config
  -> C++ HiCache state model
  -> predicted target cache state / transition trace
  -> compare with target oracle evidence
```

它分成两个层级：

| 层级 | 目标 | 当前地位 |
| --- | --- | --- |
| final state alignment | L1/L2/L3/dirty/backuped/evicted/locked 等最终集合对齐 | hard gate。 |
| transition exactness | 中间状态变化、operation lifecycle、policy/ref/capacity 账本可与 target run 证据分层比较 | diagnostic / next gate。 |

它不是 DAG patch 验收，也不是 E2E 性能预测验收。`prediction.json.predicted_e2e_ns` 只能作为 runner / DAG sanity check，不能证明 HiCache state 正确。

## 硬门槛

HiCache state prediction 必须同时满足：

| 门槛 | 要求 |
| --- | --- |
| state-model fact readiness | `hicache_state.state_model_fact_ready=true`。 |
| missing state-model facts | `hicache_state.missing_state_model_facts=[]` 或 `{}`。 |
| validation | `validation_ready=true` 且 `validation_errors=[]`。 |
| final state | 有 oracle 时必须 `hicache_state.final_state_match=true` 才能称该场景 state 通过。 |
| DAG | state-only 阶段 `dag_mutations=0` 是预期，不代表 DAG patch 已实现。 |

只要 `state_model_fact_ready=false`，即使 final state 偶然对齐，也不能宣称 state-model prediction 通过。

## 当前输入契约

HiCache state 主线使用 catalog-level fact contract。

- C++ backend 只消费 `fact.consumers` 包含 `hicache_state_model` 且 phase 满足该 role 合同的 fact，并要求
  `fact.class/fact.role` 属于已知 state-model 组合。workload/policy duration fact 使用 end-phase，
  `runtime_model_checkpoint` 使用 instant phase。
- 当前正常 state model fact 是：
  `workload_identity/request_bound_match_anchor`、`workload_identity/request_lifecycle_anchor`、
  `workload_identity/request_admission`、`target_policy_input/prefetch_decision`、
  `runtime_model_checkpoint/prefetch_check_point` 和 `runtime_model_checkpoint/storage_control_drain_boundary`。
- 启用 `hicache_state_model` 的 run 必须具备完成态 `storage_control_drain_boundary` coverage；缺少该 role 时按当前合同缺口处理。
- token dictionary 只从 completed state-model path fact 水合。`source_actual`、`timing_observation`、
  `oracle_state`、`debug_quality` 不更新 target state，也不能为 state model 补 token；它们只用于 provenance、质量检查、target oracle
  抽取和审计。
- path-bearing state-model fact 必须自足：`token_dictionary`、`full_path_span` 和每个 referenced `path_id`
  至少一次对应的 `token_ids` 都必须来自 state-model fact，不能只在 diagnostic/source_actual 侧出现。
- source/control-flow role 不是 state-model input：`insert_path`、`capacity_request`、`lock_scope_delta`、
  `maintenance_checkpoint` 等都不能进入 mutation path。
- source matched result、source admission return、actual victim、actual movement、actual async completion、node remove result、storage hit result、host ref delta 等 source 已发生结果不得混入 state-model fact。
- raw `request_id` 只是单次运行内 correlation id；cross audit 必须使用 request-normalized canonical fact，不把 raw id 当跨配置 workload identity。
- `page_identity`、`target_page_identity`、`target_page_identity_page<page_size>` 不再是 state model 主输入；target page identity 由 token dictionary/span、`hash_algo`、`cache_scope` 和 target `page_size` 推导。
- `request_lifecycle_anchor` 是 path-bearing workload identity fact，必须携带当前 lifecycle committed/fill path；
  `request_lifecycle_path_observed` 仍是 `source_actual` 诊断证据，不能回流到 state model。

## Forced Token 跨配置门禁

跨配置 prediction 的前提不是“prompt 一样”，而是同一 logical request 在 source/target 中看到的 token path 一样。
如果 generated output token 已经分叉，后续 radix key、page hash、prefetch candidate、finished insert 和 writeback 对象都会一起分叉，
此时 mismatch 不能归因到 C++ state model。

forced token profiling 是当前用于关闭该前提的输入门：

| 检查 | 要求 |
| --- | --- |
| workload report | `forced_token.enabled=true` 的 replay run 必须 `all_actual_outputs_match_plan=true`。 |
| bundle | replay 必须显式记录 `trace_sim.hicache.forced_token_bundle.v1`、bundle hash/id，并覆盖 selected input。 |
| bundle plan | bundle entry 的 plan hash、workload id/fingerprint、request count 必须与实际 plan 一致。 |
| plan schema | `forced_token.plan_schema=trace_sim.hicache.forced_token_plan.v1`。 |
| plan hash | 同 input 下所有 replay run 的 `forced_token.plan_sha256` 必须一致。 |
| output check | `unchecked_count=0`、`mismatch_count=0`、`prompt_mismatch_count=0`。 |
| workload signature | forced plan 一致后，HiCache matrix 模块仍必须证明 same-input canonical workload signature match。 |

profile quality entrypoint 把 forced replay 或 bundle provenance mismatch 视为 profiling input contract 错误；
HiCache matrix 模块把 bundle signature、plan signature 和 canonical workload signature 并列作为 quality gate。C++ state
model 不读取 bundle、`forced_output_ids` 或 capture provenance。
单 run quality 分别输出 `plan_ready`、`bundle_ready` 和总 `ready`，避免 bundle 缺失污染 plan-hash 一致性诊断。

run config 声明 forced capture/replay 时，workload report 缺失或 mode 不一致也属于合同错误。workflow 每次按当前代码重新审计
manifest，不复用旧 quality cache；因此旧 run 不能靠历史 `profile_quality.json` 绕过新 gate。

HiCache workflow entrypoint 会把该检查压缩到 `workflow_summary.json.input_contracts`：同 input 下
`signature_match=true`、`forced_token_plan_signature_match=true` 且 `forced_token_bundle_signature_match=true` 时，
`input_contract_ready=true`；`workflow_summary.json.forced_token_bundles` 记录 capture bundle 到 replay/validation 的依赖链。

## 当前模型边界

当前 active C++ state model 已完成的 target-derived 机制：

| 机制 | 当前语义 |
| --- | --- |
| token/path | `HiCacheTokenDirectory` 保存 fact-local path snapshot 和 request timeline；resolver 按 match/admission/lifecycle/prefetch 语义显式取 path，`HiCacheTargetPager` 按 target page size 生成完整 page hash。 |
| canonical radix | 每个 `cache_scope` 一棵 canonical token/page radix tree；device/host/storage/ref 是 node state，不再维护 device tree 与 host tree 两套事实源。 |
| device allocator | `request_admission` 构造 `ExtendAllocationIntent`；eviction gate 使用 `DeviceAllocatorLedger.available_pages()`，不从 radix occupancy 反推。 |
| request lifecycle | finished / unfinished 只消费 anchor 自带 committed/fill path，插入 radix，并释放 duplicate / tail / overallocated KV 到 allocator ledger。 |
| capacity index | `HiCacheCapacityIndex` mutation-driven 维护 device/host leaf、occupied pages、reserved host pages、victim choice 和 audit trace。 |
| ref ledger | request / writeback / loadback / storage / prefetch owner 级 acquire/release，输出 ref mutation 和 tree ref audit。 |
| storage directory | 区分 materialized page record 与 backend-readable hash record；prefetch storage hit query 只保留连续 readable prefix。 |
| prefetch policy | wait-complete / best-effort / timeout 共用 operation lifecycle，planned path、hit prefix、reservation、anchor ref 和 apply/revoke/late/suppressed 分离。 |
| host cleanup | host allocation 失败按 SGLang request budget cleanup；victim 是 host-visible、evicted、无 ref 保护且无 backuped child 的 host radix leaf。 |
| write policy | write-through / selective / write-back 共享 host backup / storage readable / dirty clear / cleanup helper；ACK / ref lifetime 仍按 target control boundary 近似，具体风险维护在限制文档。 |

当前仍属于妥协或中长期缺口的部分记录在 `docs/validation/hicache_state_model_limitations.md`，包括 batch-level allocation intent、loadback intent / mem_quota、transition exactness 和异步 ACK / host release 的近似边界。

## 当前保留基线与 active validation 状态

### HCSV-20260627-forced-bundle-self-regression

这是接入 `runtime_model_checkpoint/storage_control_drain_boundary` 并收紧当前 trace 合同后，用重新 profiling 数据跑出的
3 input x 5 config self 对角线回归。该 run 只覆盖 self prediction，不覆盖 cross prediction；因此它是当前 self
回归结论，不替代后续 full self/cross 矩阵。

结果目录：

```text
data/profile_runs/sglang/20260627_134659_profiling_hicache_state_forced_replay/modeling/hicache_state_workflow_manual_3inputs
```

workflow 摘要：

| 项 | 结果 |
| --- | ---: |
| workflow mode | `forced_token_replay` |
| prediction scope | `self` |
| replay runs | `15` |
| input contract ready | `3 / 3` |
| state quality ready | `15 / 15` |
| strict profile quality ready | `12 / 15` |
| final-state self exact | `15 / 15` |
| transition-count exact | `13 / 15` |

quality 说明：

- `quality_ready=true`，所有 state-model 输入合同均 ready；
- 3 个 strict profile coverage failure 仍是 `expected_hicache_mechanisms_missing`，缺少的机制都是 `prefetch_transfer`；
- 这 3 个格子的 `hicache_state_model_fact_coverage.ready=true`，没有 missing required roles、missing fields、route error 或
  unknown state-model role，因此不影响本次 state model 验收。

final-state 结论：

- 15 个 self prediction 全部 `validation_ready=true` 且 final state exact；
- 这证明 storage-control drain boundary 新合同没有造成 self final-state 回归；
- 2026-06-26 full artifact 中的 Case A oracle snapshot 误报和 Case B best-effort revoke host-release mismatch，在当前 self
  对角线上均已关闭。

transition 结论：

| input / config | final state | transition family | 差异 |
| --- | --- | --- | --- |
| `manual_deeper_pressure_prefetch / c0_wt_timeout_p128_balanced` | exact | `evicted_marker_oscillation` | 模型多 11 次 `mark_evicted` / `clear_evicted` marker lifecycle |
| `manual_deeper_pressure_prefetch / c1_wts_wait_p128_low_l1` | exact | `dirty_evicted_marker_oscillation` | 模型多 2 次 `mark_evicted` / `clear_evicted` marker lifecycle |

这两个 transition mismatch 都不改变 final state。`c0` 被 gate 分类为 `state_marker_only` / `drop`；`c1` 被分类为
`transition_grouping` / `diagnostic_only`，后续若要追求 transition exactness，应单独诊断 write-through-selective dirty/evicted
marker 分组边界，而不是回退 storage-control drain 语义。

### HCSV-20260626-forced-bundle-5x3-artifact

这是当前 forced-token bundle workflow 的保留 full-matrix 产物，用于记录最新完整 5 config x 3 input replay 上发现的问题。
该产物生成后，Case A 的 oracle snapshot 选择问题已经通过 targeted 单格验证修复，Case B 也已由
`storage_control_drain_boundary` 新合同在 2026-06-27 self 回归中关闭；因此本节只把该 full run 当作问题发现证据，
不能把它的 5x3 数值直接当作当前修复后的全矩阵结论。

结果目录：

```text
data/profile_runs/sglang/20260626_062641_profiling_hicache_state_forced_replay/modeling/hicache_state_workflow_manual_3inputs
```

workflow 摘要：

| 项 | 结果 |
| --- | ---: |
| workflow mode | `forced_token_replay` |
| replay runs | `15` |
| input contract ready | `3 / 3` |
| state quality ready | `15 / 15` |
| strict profile quality ready | `12 / 15` |

final-state 摘要：

| 范围 | prediction | ready / match | pass rate |
| --- | ---: | ---: | ---: |
| self 对角线 | `15` | `13 / 15` | `0.8667` |
| cross（不含 self） | `60` | `52 / 60` | `0.8667` |
| full self/cross | `75` | `65 / 75` | `0.8667` |

transition 摘要：

| 项 | 结果 |
| --- | ---: |
| prediction count | `75` |
| oracle / model self-check ready | `65 / 75` |
| final-state exact | `65 / 75` |
| transition-count exact | `60 / 75` |
| page-lifecycle multiset exact | `60 / 75` |

failure classification：

| classification | count |
| --- | ---: |
| `matched` | `60` |
| `real_semantic_mismatch_or_final_state_regression` | `10` |
| `transition_semantic_or_snapshot_observability_mismatch` | `5` |

2026-06-27 的 targeted 复查结论：

- Case A：`manual_deeper_pressure_prefetch / c1_wts_wait_p128_low_l1` 的 self mismatch 是 validation oracle snapshot 选择误报；
  当前 `latest_derived_state()`、timeline delta oracle 和 profiling catalog 已限制到 `HiRadixCache.*` state snapshot。
- Case A 单格已用当前代码重跑通过：`validation_ready=true`、`final_state_match=true`。
- Case B：`manual_pressure_prefetch / c2_wb_best_effort_p64_low_l1` 的 mismatch 根因集中在 best-effort prefetch revoke 后
  host release drain 的 target-control 近似边界。粗粒度地把 deferred release drain 前移到 admission / host allocation 前只对
  CaseB 局部有效，随后会让其它格子提前释放 host reservation，因此已被新契约替换。
- 当前代码改为消费 `runtime_model_checkpoint/storage_control_drain_boundary`，只在
  `HiRadixCache.drain_storage_control_queues()` 边界释放 target-derived deferred prefetch host reservation。
- 2026-06-27 self 对角线重新 profiling 已确认 15/15 final-state exact；完整 self/cross full matrix 仍需要重新 profiling 后才能刷新本节数值。

### HCSV-20260624-pre-bundle-5x3-baseline

这是 bundle workflow 落地前的最新模型回归基线。它基于 5 个 HiCache config、3 个 manual input 的 forced-token full Python
probe matrix，并通过当时的 HiCache workflow 执行 quality、final-state self/cross 和 transition exactness。

该 run 使用仓库固定 plan，不携带 bundle provenance。当前代码重新审计时会得到
`forced_token_bundle_signature_match=false` 和 `input_contract_ready=false`，因此不能作为新 workflow 的 active 输入合同验收。
下面数值只用于保留旧模型 failure set，不代表当前 active full-matrix 结论。

结果目录：

```text
data/profile_runs/sglang/20260624_150913_profiling_hicache_state_config_space_forced_python_probe/modeling/hicache_state_workflow_manual_3inputs
```

参与的 target configs：

| config | 主要覆盖 |
| --- | --- |
| `c0_wt_timeout_p128_balanced` | write-through + timeout prefetch + page size 128 + balanced capacity。 |
| `c1_wts_wait_p128_low_l1` | write-through-selective + wait-complete prefetch + low L1。 |
| `c2_wb_best_effort_p64_low_l1` | write-back + best-effort prefetch + page size 64 + low L1。 |
| `c3_wt_best_effort_p32_low_host` | write-through + best-effort prefetch + page size 32 + low host。 |
| `c4_wb_timeout_p64_low_capacity` | write-back + timeout prefetch + page size 64 + low overall capacity。 |

参与的 manual inputs：

| input | 主要覆盖 |
| --- | --- |
| `manual_phased_fast` | 基础 seed/reuse/pressure/prefetch phase。 |
| `manual_pressure_prefetch` | 更强 capacity pressure 与 prefetch 交错。 |
| `manual_deeper_pressure_prefetch` | 更深 host/storage/prefetch pressure。 |

旧 workflow 当时记录的 quality：

| 项 | 结果 |
| --- | ---: |
| replay runs | `15` |
| `state_quality_ready` | `15 / 15` |
| strict `profile_quality_ready` | `12 / 15` |
| pre-bundle input contract ready | `3 / 3` |
| canonical signature match | `3 / 3` |
| forced-token plan signature match | `3 / 3` |

3 个 strict profile coverage failure 都是 `expected_hicache_mechanisms_missing`，具体缺少 `prefetch_transfer`。此外，
当前 bundle gate 会额外阻塞全部 3 个 input；这与模型状态是否匹配无关，而是旧 run 缺少新 provenance 合同。

final-state matrix：

| 范围 | prediction | ready / exact | pass rate |
| --- | ---: | ---: | ---: |
| self 对角线 | `15` | `14 / 15` | `0.9333` |
| cross（不含 self） | `60` | `56 / 60` | `0.9333` |
| full self/cross | `75` | `70 / 75` | `0.9333` |

按 input：

| input | full prediction | final-state exact |
| --- | ---: | ---: |
| `manual_phased_fast` | `25` | `25 / 25` |
| `manual_pressure_prefetch` | `25` | `25 / 25` |
| `manual_deeper_pressure_prefetch` | `25` | `20 / 25` |

5 个 final-state failure 具有同一目标：

```text
input:  manual_deeper_pressure_prefetch
target: c1_wts_wait_p128_low_l1
source: 任意 c0..c4，包括 c1 self
```

该旧基线的 failure 只在 `locked_pages`：模型保留 10 个额外 prefix ancestor ordinary lock。当时按
write-through-selective ACK 阶段没有完整表达处理：

```text
ordinary write lock
  -> storage host protection
  -> ordinary lock release
```

L1/L2/dirty/backuped/evicted 已对齐。该结论只描述 2026-06-24 pre-bundle 旧基线；当前 forced-bundle Case A 的
oracle 误报结论以 `HCSV-20260626-forced-bundle-5x3-artifact` 和临时根因文档为准。

### 该基线的 Transition Exactness 结果

旧基线结果：

| 层级 | 结果 |
| --- | ---: |
| prediction count | `75` |
| oracle / model self-check ready | `70 / 75` |
| final-state exact | `70 / 75` |
| transition-count exact | `65 / 75` |
| page-lifecycle multiset exact | `65 / 75` |

失败分类：

| classification | count |
| --- | ---: |
| `matched` | `65` |
| `transition_semantic_or_snapshot_observability_mismatch` | `5` |
| `real_semantic_mismatch_or_final_state_regression` | `5` |

按 target config：

| target config | exact | final-state | transition-count | page-lifecycle |
| --- | ---: | ---: | ---: | ---: |
| `c0_wt_timeout_p128_balanced` | `10 / 15` | `15 / 15` | `10 / 15` | `10 / 15` |
| `c1_wts_wait_p128_low_l1` | `10 / 15` | `10 / 15` | `10 / 15` | `10 / 15` |
| `c2_wb_best_effort_p64_low_l1` | `15 / 15` | `15 / 15` | `15 / 15` | `15 / 15` |
| `c3_wt_best_effort_p32_low_host` | `15 / 15` | `15 / 15` | `15 / 15` | `15 / 15` |
| `c4_wb_timeout_p64_low_capacity` | `15 / 15` | `15 / 15` | `15 / 15` | `15 / 15` |

按 input：

| input | exact | final-state | transition-count | page-lifecycle |
| --- | ---: | ---: | ---: | ---: |
| `manual_phased_fast` | `25 / 25` | `25 / 25` | `25 / 25` | `25 / 25` |
| `manual_pressure_prefetch` | `25 / 25` | `25 / 25` | `25 / 25` | `25 / 25` |
| `manual_deeper_pressure_prefetch` | `15 / 25` | `20 / 25` | `15 / 25` | `15 / 25` |

解释：

- 65 个 prediction 已同时满足 final state、transition count 和 page lifecycle multiset exact；
- 5 个 target=`c0`、input=`manual_deeper_pressure_prefetch` 的 prediction 只差
  `mark_evicted` / `clear_evicted` marker oscillation，final state 和其它 residency/backup lifecycle exact；
- 在该旧基线中，5 个 target=`c1`、input=`manual_deeper_pressure_prefetch` 的 prediction 被 final-state locked-pages
  mismatch 阻塞，不能解释为 transition-only mismatch；
- `locked_pages` 仍参与 final-state 检查，但暂不参与 transition-count / page-lifecycle transient exactness；真实 lock/ref inc/dec
  来自 `source_actual` evidence，按约束不能作为 state model input。
- transition patch gate artifact 已达到 schema/coverage/filter readiness，但 `patch_allowed=false`；它只证明诊断 gate 完整，
  不代表 DAG patch 已可执行。

token directory 重构已通过该基线回归：原 `c3/manual_deeper_pressure_prefetch` 缺失的 8 个 lifecycle page transition 已消失，
该旧基线中 `c2`、`c3`、`c4` 均为 `15 / 15` transition exact。剩余 failure 不再归因于 lifecycle path fallback。

## 验证脚本职责

当前 active HiCache validation entrypoints/modules：

| 脚本 | 职责 |
| --- | --- |
| `scripts/internal/entrypoints/hicache_cross_input_audit.py` | 跨配置 workload identity input contract 审计；比较前先检查 path-bearing state-model fact 是否可被 C++ token parser 消费。 |
| `scripts/internal/markov_internal/hicache/matrix_types.py` | matrix 中的 profile run、prediction spec 和 slug 类型。 |
| `scripts/internal/markov_internal/hicache/matrix_discovery.py` | profile discovery、config/input 过滤和 prediction spec 展开。 |
| `scripts/internal/markov_internal/hicache/matrix_quality.py` | profile quality gate、workload signature 和输入合同汇总。 |
| `scripts/internal/markov_internal/hicache/matrix_prediction.py` | target config 写出、prediction 输出路径和 final-state matrix summary。 |
| `scripts/internal/entrypoints/hicache_workflow.py` | profiling 后 HiCache validation 主入口；编排 quality、final-state self/cross 和 transition exactness。 |
| `scripts/internal/entrypoints/hicache_provenance.py` | 基于 validation / predicted trace / oracle snapshot 的 mismatch 页面证据汇总，不回写模型。 |
| `scripts/internal/entrypoints/hicache_transition.py` | 只读 transition exactness CLI：model self-check、target oracle extraction、self/cross/matrix compare。 |
| `scripts/internal/markov_internal/hicache/oracle_state.py` | state snapshot 读取、HiRadixCache-only final-state 派生和 state set diff。 |
| `scripts/internal/markov_internal/hicache/oracle_delta.py` | event / timeline delta oracle 与 predicted transition 覆盖诊断。 |
| `scripts/internal/markov_internal/hicache/transition_taxonomy.py` | transition family 分类、DAG patch gate 字段和 evidence 摘要 helper；不提供 CLI。 |
| `scripts/internal/markov_internal/hicache/transition_catalog.py` | transition mismatch catalog JSON/Markdown 和 family sample 生成；不提供 CLI。 |
| `scripts/internal/markov_internal/hicache/transition_gate.py` | diagnostic operation gate payload、coverage 和 scoreboard 生成；不提供 CLI。 |

这些脚本都不能生成 synthetic state-model fact，不能修改 profiling trace，也不能把 `source_actual` / `timing_observation` /
`oracle_state` 写回 target state。

## 复现命令

基础检查：

```bash
scripts/run.sh modeling -- bash -lc \
  'cmake -S . -B build/modeling -G Ninja && cmake --build build/modeling --target trace_graph -j2'
scripts/run.sh modeling -- bash -lc \
  'python3 -m py_compile $(find scripts/internal/entrypoints scripts/internal/markov_internal -name "*.py" -print)'
find configs -name '*.json' -print0 | xargs -0 -n1 jq empty
git diff --check
```

跑 3 个 manual input 下的 forced replay final-state matrix：

```bash
scripts/profile.sh \
  configs/experiments/hicache_state/profiling_hicache_state_forced_capture.json \
  --inputs manual_phased_fast,manual_pressure_prefetch,manual_deeper_pressure_prefetch

CAPTURE_BUNDLE=data/profile_runs/sglang/<capture_suite>/forced_token_bundle.json
RUN_DIR=<forced_replay_suite_dir>

scripts/profile.sh \
  configs/experiments/hicache_state/profiling_hicache_state_forced_replay.json \
  --inputs manual_phased_fast,manual_pressure_prefetch,manual_deeper_pressure_prefetch \
  --forced-token-bundle "$CAPTURE_BUNDLE"

python3 scripts/internal/entrypoints/hicache_workflow.py \
  --profile-run-dir "$RUN_DIR" \
  --output-dir "$RUN_DIR/modeling/hicache_state_workflow_manual_3inputs" \
  --stages quality,final-state,transition \
  --prediction-scope self,cross \
  --inputs manual_phased_fast,manual_pressure_prefetch,manual_deeper_pressure_prefetch \
  --emit-transition-catalog \
  --emit-transition-gates
```

common suite 使用 `profiling_hicache_state_common.json`，只允许 `--prediction-scope self`；cross-config workflow 会拒绝
没有 forced bundle contract 的 common run。

只跑某个 targeted 格子：

```bash
python3 scripts/internal/entrypoints/hicache_workflow.py \
  --profile-run-dir <profile_run_dir> \
  --output-dir <profile_run_dir>/modeling/<targeted_output_dir> \
  --stages quality,final-state \
  --prediction-scope self \
  --input <input_id> \
  --source-config <config_id> \
  --target-config <config_id> \
  --force \
  --max-predictions 1
```

Transition exactness：

```bash
python3 scripts/internal/entrypoints/hicache_transition.py \
  --mode model-self-check \
  --prediction-dir <prediction_dir>

python3 scripts/internal/entrypoints/hicache_transition.py \
  --mode extract-target-oracle \
  --target-manifest <target_profile_manifest.json> \
  --output <matrix_dir>/observed_target_transitions/<input_id>/<config_id>.observed_target_transition_trace.json

python3 scripts/internal/entrypoints/hicache_transition.py \
  --mode compare-self \
  --prediction-dir <prediction_dir> \
  --observed-target-trace <observed_target_transition_trace.json>

python3 scripts/internal/entrypoints/hicache_transition.py \
  --mode compare-matrix \
  --matrix-dir <hicache_state_workflow_dir> \
  --emit-catalog \
  --emit-gates
```

矩阵模式默认复用已生成的 target oracle；`--force` 会重建 full Python probe oracle，耗时明显更高，仅在脚本或 oracle 抽取逻辑变化后使用。
在统一 workflow 中请求 transition 时，`--stages` 必须同时包含 `final-state`；独立只读诊断仍使用上面的
`scripts/internal/entrypoints/hicache_transition.py` CLI。

结果摘要读取：

```bash
jq '{workflow_mode,
     quality,
     input_contracts,
     final_state_self,
     final_state_cross,
     transition}' \
  <matrix_dir>/workflow_summary.json

jq '{prediction_count,
     validation_ready_count,
     final_state_match_count,
     final_state_pass_rate,
     by_input}' \
  <matrix_dir>/final_state_cross.json

jq '{prediction_count,
     ready_count,
     exact_count,
     final_state_exact_count,
     transition_count_exact_count,
     page_lifecycle_multiset_exact_count,
     failure_classification_counts,
     by_input,
     by_target_config}' \
  <matrix_dir>/transition_exactness_matrix.json
```

`final_state_self.json`、`final_state_cross.json` 和 `transition_exactness_matrix.json` 是当前 workflow 的规模无关输出；
矩阵规模以文件内 `prediction_count`、`by_input` 和 `workflow_summary.json` 为准。

## 已关闭机制缺口

这些结论来自已迁移的临时诊断文档，不再作为单独文档维护。

### 分配器 / 生命周期

- device eviction gate 不再使用 `occupied_device_pages + reservation_pages > capacity` 这类 radix occupancy 反推；
- gate 对齐 SGLang `allocator.available_size() < requested_tokens`，其中 available 来自 allocator ledger；
- eviction budget 使用完整 allocation request，而不是只清理 deficit；
- request lifecycle 在 finished / unfinished 时释放 duplicate、tail 和 overallocated KV；
- `request_bound_match_anchor` 的 loadback allocation 当前只做 opportunistic promotion；需要 eviction 的 loadback 等待新的 loadback intent。

该机制关闭了早期 c2 self prediction 的 L1 mismatch。batch-level allocator 仍以 `batch_size=1` 为短期合同，详见限制文档。

### L2 / Host / Storage 语义

- `backuped` 只表示 host copy，不把 storage-readable 直接当成 backuped；
- host cleanup 删除 host leaf subtree，并更新 parent/child topology 与 capacity index；
- timeout prefetch 不再因为 storage directory hit 就直接落 host，必须等 target policy 的 completed/apply 边界；
- target host/device capacity 从 SGLang server command 推导，包括 host pool page 对齐和 prefetch capacity limit；
- prefetch revoke / timeout incomplete 的 host reservation 不立即释放，而是保留 deferred release，并只在
  `storage_control_drain_boundary` 上按 target-derived async table 全量 drain；
- write-through backup ACK 前持有普通 lock ref，并在下一条 target control fact 近似 drain。

这些机制关闭了旧矩阵中的多类 L2/backuped/evicted/locked mismatch。write-through ACK / ordinary lock lifetime 仍是近似边界，
但不再把 Case A 的 `c1/manual_deeper_pressure_prefetch` self mismatch 归因为模型 lock regression；该问题已确认是
oracle snapshot 选择误报。

## 历史阶段摘要

### HCSV-20260613-host-release-policy-final3

- S1A/S1B 33-target atomic profile 四向 prediction 全部 final-state pass；
- S1B 早期 L2/backuped/evicted `70/55` residual 已通过 target-derived host release / cleanup policy 关闭；
- 该阶段证明 host release budget 必须来自 SGLang allocation request，而不是超容量拟合预算。

### HCSV-20260612-atomic-input-contract

- 旧 mixed roles 被拆掉：`request_tokens`、`lookup_path`、`request_cache_lifecycle` 不再是 normal role；
- source/control-flow 事实降级为 evidence：`insert_path`、`capacity_request`、`lock_scope_delta`、`maintenance_checkpoint` 等不更新 target state；
- 双向 cross audit 证明 workload identity contract 可以作为 state model 输入边界。

### HCSV-20260612-target-resource-mechanism

- request-derived device lock/ref、admission reservation、target L1 capacity pressure 和 dynamic device radix leaf victim 初步闭合；
- S1A target self/cross 已 final-state pass；
- S1B 剩余差异集中在 host/L2/storage/prefetch visibility，推动后续 host/device/async 边界重构。

## 下一步

后续优先级：

1. 用当前代码重新 profiling 并重跑 5x3 forced-token self/cross workflow，刷新 storage-control drain boundary 重构后的 full-matrix
   数值。当前只有 2026-06-27 self 对角线是 active 通过结论。
2. 审查 strict profile quality 中 `prefetch_transfer` 期望机制覆盖是否仍应作为 hard profile-quality failure；当前这 3 个 failure
   不影响 state-model fact coverage。
3. 单独诊断 `manual_deeper_pressure_prefetch/c1` 的 dirty/evicted marker transition grouping；当前它不是 final-state bug，也不是
   storage-control boundary 回归。
4. 保留 `manual_deeper_pressure_prefetch/c0` 的 `mark_evicted` / `clear_evicted` oscillation 为 state-marker-only，不直接进入 DAG patch。
5. 把 exact transition 聚合成 stable `CacheIntentLog`，同时保持 patch gate 的 `patch_allowed=false`，直到 source attribution、
   duration 和 remaining semantic boundary 都具备证据。
6. 继续保持 `source_actual` / `timing_observation` / `oracle_state` 只做 evidence，不回到 normal state mutation。
