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
  `fact.class/fact.role` 属于已知 state-model 组合。`cache_extend_input` 使用 start-phase，其它当前 workload identity fact 使用
  end-phase。
- 当前正常 state model fact 是：
  `workload_identity/cache_lookup_input`、`workload_identity/cache_extend_input`、
  `workload_identity/cache_lifecycle_commit`、`workload_identity/prefetch_candidate_anchor`。
- `drain_storage_control_queues()` 不再作为 profiling runtime checkpoint 采集；HiCache profile audit 和 transition validator
  不依赖 source scheduler round boundary。
- token dictionary 只从 completed state-model path fact 水合。`source_actual`、`timing_observation` 和 `oracle_state`
  不更新 target state，也不能为 state model 补 token；它们只用于 provenance、质量检查、target oracle 抽取和审计。
- `source_actual` / `timing_observation` 的当前字段只保留 operation-level correlation，例如 `cache_scope`、run-local
  `request_id` 和异步 `operation_id`；transition page/state label 必须来自 `oracle_state/state_snapshot` 或模型自身 trace。
- path-bearing state-model fact 必须自足：`token_dictionary`、`full_path_span` 和每个 referenced `path_id`
  至少一次对应的 `token_ids` 都必须来自 state-model fact，不能只在 diagnostic/source_actual 侧出现。
- source/control-flow role 不是 state-model input：`capacity_request`、`capacity_result_observed`、`lock_scope_delta`、
  `request_admission_observed` 等都不能进入 mutation path。
- source matched result、source admission return、actual victim、actual movement、actual async completion、node remove result、storage hit result、host ref delta 等 source 已发生结果不得混入 state-model fact。
- raw `request_id` 只是单次运行内 correlation id；cross audit 必须使用 request-normalized canonical fact，不把 raw id 当跨配置 workload identity。
- `page_identity`、`target_page_identity`、`target_page_identity_page<page_size>` 不再是 state model 主输入；target page identity 由 token dictionary/span、`hash_algo`、`cache_scope` 和 target `page_size` 推导。
- `cache_lifecycle_commit` 是 path-bearing workload identity fact，必须携带当前 lifecycle committed/fill path；当前 catalog
  不再采集单独的 lifecycle source path observed target。

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
| workload signature | forced plan 一致后，HiCache validation preflight 仍必须证明 same-input canonical workload signature match。 |

HiCache validation preflight 把 forced replay 或 bundle provenance mismatch 视为 profiling input contract 错误；
它把 bundle signature、plan signature 和 canonical workload signature 并列作为 workflow input gate。C++ state model
不读取 bundle、`forced_output_ids` 或 capture provenance。
单 run quality 分别输出 `plan_ready`、`bundle_ready` 和总 `ready`，避免 bundle 缺失污染 plan-hash 一致性诊断。

run config 声明 forced capture/replay 时，workload report 缺失或 mode 不一致也属于合同错误。workflow 每次按当前代码重新审计
manifest，不复用旧 audit/quality cache；因此旧 run 不能靠历史审计 JSON 绕过新 gate。

Unified modeling workflow 的 HiCache validation preflight 会把该检查压缩到 `preflight_summary.json` 和
`workflow_summary.json`：同 input 下 `signature_match=true`、`forced_token_plan_signature_match=true` 且
`forced_token_bundle_signature_match=true` 时，workflow input 才能 ready。

## 当前模型边界

当前 active C++ state model 已完成的 target-derived 机制：

| 机制 | 当前语义 |
| --- | --- |
| token/path | `HiCacheTokenDirectory` 保存 fact-local path snapshot 和 request timeline；resolver 按 lookup/extend/lifecycle/prefetch 语义显式取 path，`HiCacheTargetPager` 按 target page size 生成完整 page hash。 |
| canonical radix | 每个 `cache_scope` 一棵 canonical token/page radix tree；device/host/storage/ref 是 node state，不再维护 device tree 与 host tree 两套事实源。 |
| device allocator | `cache_extend_input` 构造 batch-level `CacheExtendBatchIntent`；eviction gate 使用 `DeviceAllocatorLedger.available_pages()`，不从 radix occupancy 反推。 |
| request lifecycle | finished / unfinished 只消费 `cache_lifecycle_commit` 自带 committed/fill path，插入 radix，并释放 duplicate / tail / overallocated KV 到 allocator ledger。 |
| capacity index | `HiCacheCapacityIndex` mutation-driven 维护 device/host leaf、occupied pages、reserved host pages、victim choice 和 audit trace。 |
| ref ledger | request / writeback / loadback / storage / prefetch owner 级 acquire/release，输出 ref mutation 和 tree ref audit。 |
| storage directory | 区分 materialized page record 与 backend-readable hash record；prefetch storage hit query 只保留连续 readable prefix。 |
| prefetch policy | wait-complete / best-effort / timeout 共用 operation lifecycle，planned path、hit prefix、reservation、anchor ref 和 apply/revoke/late/suppressed 分离。 |
| host cleanup | host allocation 失败按 SGLang request budget cleanup；victim 是 host-visible、evicted、无 ref 保护且无 backuped child 的 host radix leaf。 |
| write policy | write-through / selective / write-back 共享 host backup / storage readable / dirty clear / cleanup helper；ACK / ref lifetime 仍按 target control boundary 近似，并在 finalize 收敛尾部 write-through pending ACK，具体风险维护在限制文档。 |

当前仍属于妥协或中长期缺口的部分记录在 `docs/validation/hicache_state_model_limitations.md`，包括 batch-level allocation intent、loadback intent / mem_quota、backend I/O / transition timeline 和异步 ACK / host release 的近似边界。

## 当前合同状态与保留验证基线

说明：2026-07-02 完成的 unified `modeling_workflow` Python 重构已经通过 Ruff、`compileall` 和两条 dry-run smoke，
但尚未产生新的 full-matrix modeling 结果。因此当前 state/transition 正确性仍以
`HCSV-20260701-forced-bundle-full-matrix` 的旧 workflow 结果为保留基线；新 workflow 的 full-run 结果生成后再更新本节。

### HCSV-20260701-forced-bundle-full-matrix

这是当前 active profiling 合同下的 full-matrix 基线。该 run 使用
`cache_lookup_input` / `cache_extend_input` / `cache_lifecycle_commit` / `prefetch_candidate_anchor`
作为 state-model 输入，不采集 runtime prefetch checkpoint、per-request admission 或 storage-control drain checkpoint。

该基线包含本轮回归修复后的三项收口：

- canonical workload signature 使用 role/signature multiset 作为 hard gate，raw event sequence 只作为可显示诊断；
- transition schema / replay / taxonomy 统一到当前 C++ transition kind，例如 `cache_extend_acquire_request_ref`；
- write-through backup ACK 的尾部普通 lock ref 在 `finalize()` boundary 收敛，避免 trace 尾部 pending ACK 污染 final state。

结果目录：

```text
data/profile_runs/sglang/20260701_060552_profiling_hicache_state_forced_replay/modeling/hicache_state_workflow_manual_3inputs_first_fix
```

workflow 摘要：

| 项 | 结果 |
| --- | ---: |
| workflow mode | `forced_token_replay` |
| replay runs | `15` |
| input contract ready | `3 / 3` |
| workflow input ready | `15 / 15` |
| state-model input ready | `15 / 15` |
| artifact ready | `15 / 15` |
| strict diagnostic coverage ready | `12 / 15` |
| workload sequence diagnostic match | `2 / 3` |
| final-state self exact | `15 / 15` |
| final-state cross exact | `60 / 60` |
| full final-state exact | `75 / 75` |
| transition exact | `75 / 75` |
| transition-count exact | `75 / 75` |
| page-lifecycle multiset exact | `75 / 75` |

input contract 状态：

- `workflow_input_ready=true`，所有 state-model 输入合同均 ready；
- 3 个 input 的 forced-token plan signature、bundle signature 和 canonical workload signature 均一致，bundle id 为
  `sha256_json:dc1e389c53fc49face219d27bc4b264c19ccd097a33cd66660f84217b0cfcb03`；
- `manual_deeper_pressure_prefetch` 的 raw sequence signature 有 3 种，`sequence_match=false`，但 canonical workload
  signature 仍为 1 种；这说明当前跨配置 hard gate 检查的是规范化 workload multiset，不把 source runtime event 排序误当成
  target-independent workload 差异；
- 当前 trace / catalog 合同中没有 storage-control drain boundary、runtime prefetch checkpoint 或 `check_kind`；
  storage-control release 时机由 target-derived 模型近似负责。

quality 说明：

- 3 个 strict profile coverage failure 仍是 `expected_hicache_mechanisms_missing`，缺少的机制都是 `prefetch_transfer`；
- 这 3 个格子的 `hicache_state_model_fact_coverage.ready=true`，没有 missing required roles、missing fields、route error 或
  unknown state-model role；
- 因此 strict `strict_diagnostic_coverage_ready=12/15` 是 source/timing 诊断覆盖率，不阻塞本次 state-model validation。

结论：

- 当前 5x3 manual matrix 的 self/cross final-state 已全部对齐；
- 当前 75 个 transition prediction 也全部 exact，transition-count 和 page-lifecycle multiset 也全部 exact；
- 这关闭了 2026-06-26 forced-bundle 产物发现的 Case A oracle final snapshot 误报、Case B host-side release boundary
  mismatch、2026-06-27 self 对角线中残留的 transition marker mismatch，以及 2026-07-01 重构后暴露的
  workload sequence hard-gate、transition kind schema drift 和尾部 write-through ACK lock lifetime 回归；
- 上述通过只证明当前 manual matrix 和当前近似边界成立，不等价于完整 SGLang scheduler / backend I/O / rank-synced queue
  exactness，长期近似仍维护在限制文档。

### HCSV-20260628-forced-bundle-full-matrix（历史上一版合同）

这是上一版 HiCache profiling 合同下的 full-matrix 基线。该 run 使用当时最新 profiling 合同重新采集 forced-token replay，覆盖
3 个 manual input、5 个 HiCache config、self/cross final-state 和 transition exactness。

结果目录：

```text
data/profile_runs/sglang/20260628_154748_profiling_hicache_state_forced_replay/modeling/hicache_state_workflow_manual_3inputs
```

历史结果是：input contract `3 / 3` ready，workflow input `15 / 15` ready，strict diagnostic coverage `12 / 15`
ready，final-state self `15 / 15` exact，final-state cross `60 / 60` exact，transition exact / transition-count exact /
page-lifecycle multiset exact 均为 `75 / 75`。

该 run 证明上一版不采集、不消费 `storage_control_drain_boundary` 的合同下，5x3 manual matrix 曾经完整对齐。由于当前
state-model 输入和 workflow 质量口径已经收紧，它只保留为历史对照；当前 active exactness 结论以
`HCSV-20260701-forced-bundle-full-matrix` 为准。

### HCSV-20260627-forced-bundle-self-regression（历史 self 回归）

这是 2026-06-27 重新 profiling 数据跑出的 3 input x 5 config self 对角线回归。该 run 只覆盖 self prediction，不覆盖 cross prediction；因此它是历史 self
回归证据，不替代当前 2026-07-01 full self/cross 矩阵。

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
| state-model input ready | `15 / 15` |
| strict diagnostic coverage ready | `12 / 15` |
| final-state self exact | `15 / 15` |
| transition-count exact | `13 / 15` |

quality 说明：

- 所有 state-model 输入合同均 ready；
- 3 个 strict profile coverage failure 仍是 `expected_hicache_mechanisms_missing`，缺少的机制都是 `prefetch_transfer`；
- 这 3 个格子的 `hicache_state_model_fact_coverage.ready=true`，没有 missing required roles、missing fields、route error 或
  unknown state-model role，因此不影响本次 state model 验收。

final-state 结论：

- 15 个 self prediction 全部 `validation_ready=true` 且 final state exact；
- 这证明当时的 post-extend prefetch release 近似没有造成 self final-state 回归；
- 2026-06-26 full artifact 中的 Case A oracle snapshot 误报和 Case B best-effort revoke host-release mismatch，在当前 self
  对角线上均已关闭。

历史 transition 说明：

- 该 run 曾暴露 2 个 self transition marker mismatch，final state 均 exact；
- 后续 2026-06-28 全矩阵在当时不采集、不消费 `storage_control_drain_boundary` 的合同下达到 `75 / 75` transition exact；
- 因此这两个 marker mismatch 不再作为 active limitation 或下一步修复项维护。

### HCSV-20260626-forced-bundle-5x3-artifact

这是 forced-token bundle workflow 的历史 full-matrix 产物，用于记录 2026-06-26 这次 5 config x 3 input replay 上发现的问题。
该产物生成后，Case A 的 oracle snapshot 选择问题已经通过 targeted 单格验证修复，Case B 也已由
后续 post-extend prefetch release 近似在 2026-06-27 self 回归中关闭；因此本节只把该 full run 当作问题发现证据，
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
| state-model input ready | `15 / 15` |
| strict diagnostic coverage ready | `12 / 15` |

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
  host release drain 的 target-control 近似边界。粗粒度地把 deferred release drain 前移到 extend / host allocation 前只对
  CaseB 局部有效，随后会让其它格子提前释放 host reservation，因此已被新契约替换。
- 当前 profiling 合同不采集 `drain_storage_control_queues()` checkpoint。terminal prefetch 后的剩余 host reservation
  保留到同 request 的 `cache_extend_input` side effect 完成后，再做 request-local post-extend release drain。
- 2026-06-28 forced replay 全矩阵曾在上一版合同下刷新结论：self/cross final-state `75 / 75` exact，transition
  `75 / 75` exact。2026-06-30 合同收紧后，该 run 只保留为历史问题发现证据；当前 active exactness 结论以
  `HCSV-20260701-forced-bundle-full-matrix` 为准。

### HCSV-20260624-pre-bundle-5x3-baseline

这是 bundle workflow 落地前的最新模型回归基线。它基于 5 个 HiCache config、3 个 manual input 的 forced-token full Python
probe matrix，并通过当时的旧 HiCache workflow 执行 quality、final-state self/cross 和 transition exactness。

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
| 旧 state ready 字段 | `15 / 15` |
| 旧 strict profile ready 字段 | `12 / 15` |
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
oracle 误报结论以 `HCSV-20260626-forced-bundle-5x3-artifact` 和本文“已关闭诊断问题”中的 Case A 结论为准。

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
| `scripts/internal/entrypoints/modeling_workflow.py` | profiling 后统一 modeling workflow 主入口；通过 validation object 选择 HiCache final-state / transition。 |
| `scripts/internal/markov_internal/modeling_workflow/` | preflight、model-run planner、runner adapter、validation object、进度输出和顶层 summary。 |
| `scripts/internal/markov_internal/modeling_workflow/planning/profile_runs.py` | unified profile run discovery、prediction ref、slug 类型和 config/input 过滤。 |
| `scripts/internal/markov_internal/modeling_workflow/execution/runner_adapter.py` | 写出 Python runner config；C++ backend narrow config 仍由 modeling runner 生成。 |
| `scripts/internal/markov_internal/modeling_workflow/validations/hicache/core/` | HiCache fact 解析、consumer 路由和 token dictionary/span 辅助工具。 |
| `scripts/internal/markov_internal/modeling_workflow/validations/hicache/preflight/` | HiCache workflow input gate、state fact coverage、workload signature、sequence 诊断、forced-token 和 strict diagnostic coverage。 |
| `scripts/internal/markov_internal/modeling_workflow/validations/hicache/prediction_rows.py` | final-state validation row 提取和 summary。 |
| `scripts/internal/markov_internal/modeling_workflow/validations/hicache/input_contract/` | workload identity fact 抽取、token/path 合同、canonical signature 和 source/target report。 |
| `scripts/internal/markov_internal/modeling_workflow/validations/hicache/oracle/` | state snapshot 读取、HiRadixCache-only final-state 派生、capacity、coverage、delta、records 和 mismatch provenance。 |
| `scripts/internal/markov_internal/modeling_workflow/validations/hicache/transition/` | transition path/catalog 产物、model self-check/replay schema、target oracle、prediction-set compare、taxonomy 和 gate 输出。 |

这些脚本都不能生成 synthetic state-model fact，不能修改 profiling trace，也不能把 `source_actual` / `timing_observation` /
`oracle_state` 写回 target state。

## 复现命令

基础检查：

```bash
scripts/run.sh modeling -- bash -lc \
  'cmake -S src/modeling/trace_graph -B build/modeling/trace_graph-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DTRACE_GRAPH_DEBUG=OFF && cmake --build build/modeling/trace_graph-release --target trace_graph -j2'
scripts/run.sh modeling -- bash -lc \
  'cmake -S src/modeling/trace_graph -B build/modeling/trace_graph-validation -G Ninja -DCMAKE_BUILD_TYPE=Release -DTRACE_GRAPH_DEBUG=ON && cmake --build build/modeling/trace_graph-validation --target trace_graph -j2'
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

python3 scripts/internal/entrypoints/modeling_workflow.py \
  --profile-run-dir "$RUN_DIR" \
  --output-dir "$RUN_DIR/modeling/modeling_workflow_hicache_state_manual_3inputs" \
  --validations hicache_final_state,hicache_transition \
  --prediction-scope self,cross \
  --inputs manual_phased_fast,manual_pressure_prefetch,manual_deeper_pressure_prefetch \
  --emit-transition-catalog \
  --emit-transition-gates
```

common suite 使用 `profiling_hicache_state_common.json`，只允许 `--prediction-scope self`；cross-config workflow 会拒绝
没有 forced bundle contract 的 common run。

只跑某个 targeted 格子：

```bash
python3 scripts/internal/entrypoints/modeling_workflow.py \
  --profile-run-dir <profile_run_dir> \
  --output-dir <profile_run_dir>/modeling/<targeted_output_dir> \
  --validations hicache_final_state \
  --prediction-scope self \
  --input <input_id> \
  --source-config <config_id> \
  --target-config <config_id> \
  --force \
  --max-predictions 1
```

Transition exactness 不再维护 standalone `hicache_transition.py` CLI。选择 `hicache_transition` validation 时，
workflow 会复用同一次 cache-state model run 和 validation artifact，按需抽取 target oracle、写出 transition exactness payload、
catalog 和 gate scoreboard。需要重建这些产物时对 unified workflow 使用 `--force`。

结果摘要读取：

```bash
jq '{selected_validations,
     profile_run_count,
     model_run_count,
     model_run_ok_count,
     model_run_skipped_count,
     preflight_ready,
     validations}' \
  <workflow_output>/workflow_summary.json

jq '{prediction_count,
     validation_ready_count,
     final_state_match_count,
     final_state_pass_rate,
     by_input}' \
  <workflow_output>/artifacts/validations/hicache_final_state/summary.json

jq '{prediction_count,
     ready_count,
     exact_count,
     final_state_exact_count,
     transition_count_exact_count,
     page_lifecycle_multiset_exact_count,
     failure_classification_counts,
     by_input,
     by_target_config}' \
  <workflow_output>/artifacts/validations/hicache_transition/summary.json
```

`workflow_summary.json`、`preflight_summary.json`、`artifacts/model_runs_summary.json` 和
`artifacts/validations/<name>/summary.json` 是当前 unified workflow 的规模无关输出；矩阵规模以文件内
`prediction_count`、`by_input` 和 `workflow_summary.json` 为准。

## 已关闭诊断问题

这些问题来自 `docs/tmp/` 中已闭环的临时诊断。主线只保留当前仍成立的结论，不迁移后来被回退的实现方案。

| 问题 | 当前结论 |
| --- | --- |
| Case A：`manual_deeper_pressure_prefetch/c1` oracle final 为空 | 这是 validation oracle snapshot 选择误报。final oracle、timeline delta oracle 和 profiling catalog 均已限制到 `HiRadixCache.*` cache-tree snapshot；`HiCacheController.*` 等辅助对象 snapshot 不参与 final cache-tree 派生。 |
| Case B：`manual_pressure_prefetch/c2` host-side final-state mismatch | 这是模型 release 边界问题，不是 oracle 误报。曾经尝试把 `drain_storage_control_queues()` 做成 state-model checkpoint，但该方案已回退；当前不采集也不消费 `storage_control_drain_boundary`，而是在 terminal prefetch 后保留 pending host reservation，并在同 request `cache_extend_input` side effect 后做 request-local post-extend drain。 |
| 2026-06-27 self transition marker mismatch | 历史 self run 曾有 2 个 marker-only transition mismatch；2026-06-28 全矩阵在上一版合同下达到 `75 / 75` transition exact，不再作为 active 修复项维护。 |

注意：`drain_storage_control_queues()` 是 source scheduler round boundary。它不能重新包装成 cross-config state-model input，
也不能作为 HiCache profile audit 或 transition validator 的必需 checkpoint。后续如果要精确建模 rank-synced release queue，
需要新增真正 target-independent 的 queue / scheduler intent，而不是复用 source runtime boundary。

## 已关闭机制缺口

这些结论来自已迁移的临时诊断文档，不再作为单独文档维护。

### 分配器 / 生命周期

- device eviction gate 不再使用 `occupied_device_pages + reservation_pages > capacity` 这类 radix occupancy 反推；
- gate 对齐 SGLang `allocator.available_size() < requested_tokens`，其中 available 来自 allocator ledger；
- eviction budget 使用完整 allocation request，而不是只清理 deficit；
- request lifecycle 在 finished / unfinished 时释放 duplicate、tail 和 overallocated KV；
- `cache_lookup_input` 的 loadback allocation 当前只做 opportunistic promotion；需要 eviction 的 loadback 等待新的 loadback intent。

该机制关闭了早期 c2 self prediction 的 L1 mismatch。batch-level allocator 仍以 `batch_size=1` 为短期合同，详见限制文档。

### L2 / Host / Storage 语义

- `backuped` 只表示 host copy，不把 storage-readable 直接当成 backuped；
- host cleanup 删除 host leaf subtree，并更新 parent/child topology 与 capacity index；
- timeout prefetch 不再因为 storage directory hit 就直接落 host，必须等 target policy 的 completed/apply 边界；
- target host/device capacity 从 SGLang server command 推导，包括 host pool page 对齐和 prefetch capacity limit；
- prefetch revoke / timeout incomplete 的 host reservation 不立即释放，而是保留 pending release；同 request cache extend 完成后
  做 request-local release drain，finalize 只兜底释放没有后续 cache extend 的残留 reservation；
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
- source/control-flow 事实降级为 evidence：`capacity_request`、`capacity_result_observed`、`lock_scope_delta`、
  `request_admission_observed` 等不更新 target state；
- 双向 cross audit 证明 workload identity contract 可以作为 state model 输入边界。

### HCSV-20260612-target-resource-mechanism

- request-derived device lock/ref、cache extend reservation、target L1 capacity pressure 和 dynamic device radix leaf victim 初步闭合；
- S1A target self/cross 已 final-state pass；
- S1B 剩余差异集中在 host/L2/storage/prefetch visibility，推动后续 host/device/async 边界重构。

## 下一步

后续优先级：

1. 审查 strict diagnostic coverage 中 `prefetch_transfer` 期望机制覆盖是否仍应作为 hard diagnostic failure；当前这 3 个 failure
   不影响 state-model fact coverage。
2. 把 exact transition 聚合成 stable `CacheIntentLog`，同时保持 patch gate 的 `patch_allowed=false`，直到 source attribution、
   duration 和 remaining semantic boundary 都具备证据。
3. 继续保持 `source_actual` / `timing_observation` / `oracle_state` 只做 evidence，不回到 normal state mutation。
4. 如果后续要支持更强 workload、TP/rank exactness 或 backend I/O exactness，应新增 target-independent scheduler /
   queue / async progress intent，而不是恢复 source `drain_storage_control_queues()` checkpoint。
