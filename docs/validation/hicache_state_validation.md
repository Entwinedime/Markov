# HiCache State Validation

维护方式：本文是 HiCache state validation 的 active 文档，直接维护当前有效口径、最新结果、仍未证明的风险和复现入口。历史实验只保留能解释当前边界的压缩结论；不再维护单独的短期计划或临时诊断文档。

## 目标

本阶段验证的问题是：

```text
base profiling invariant facts + explicit target cache config
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
| invariant coverage | `hicache_state.invariant_coverage_ready=true`。 |
| missing invariant facts | `hicache_state.missing_invariant_facts=[]` 或 `{}`。 |
| illegal usage | `hicache_state.non_invariant_fact_usage=[]`。 |
| validation | `validation_ready=true` 且 `validation_errors=[]`。 |
| final state | 有 oracle 时必须 `hicache_state.final_state_match=true` 才能称该场景 state 通过。 |
| DAG | state-only 阶段 `dag_mutations=0` 是预期，不代表 DAG patch 已实现。 |

只要 `non_invariant_fact_usage` 非空，即使 final state 偶然对齐，也不能宣称 invariant-only prediction 通过。

## 当前输入契约

HiCache state 主线使用 target-level atomic fact contract。

- C++ backend 只消费 `model_input=true && fact_class=invariant_state && fact_granularity=atomic`，且 role 属于已知 atomic invariant。
- 当前正常 state model input 是 5 个 role：
  `request_bound_match_anchor`、`request_lifecycle_anchor`、`request_admission`、`prefetch_decision`、`prefetch_check_point`。
- `source_actual`、`timing_observation`、`oracle_state`、`debug_quality` 不更新 target state；它们只用于 token dictionary 水合、provenance、质量检查、target oracle 抽取和审计。
- 旧 mixed/source-control role 不再是 normal input：`request_tokens`、`lookup_path`、`request_cache_lifecycle`、`insert_path`、`capacity_request`、`lock_scope_delta`、`maintenance_checkpoint` 都不能进入 normal mutation path。
- source matched result、source admission return、actual victim、actual movement、actual async completion、node remove result、storage hit result、host ref delta 等 source 已发生结果不得作为 `invariant_state` 字段混入。
- raw `request_id` 只是单次运行内 correlation id；cross audit 必须使用 request-normalized canonical fact，不把 raw id 当跨配置 invariant。
- `page_identity`、`target_page_identity`、`target_page_identity_page<page_size>` 不再是 state model 主输入；target page identity 由 token dictionary/span、`hash_algo`、`cache_scope` 和 target `page_size` 推导。

## 当前模型边界

当前 active C++ state model 已完成的 target-derived 机制：

| 机制 | 当前语义 |
| --- | --- |
| token/path | `HiCacheTokenPathStore` 收集 dictionary/span，`HiCacheTargetPager` 按 target page size 生成完整 page hash。 |
| canonical radix | 每个 `cache_scope` 一棵 canonical token/page radix tree；device/host/storage/ref 是 node state，不再维护 device tree 与 host tree 两套事实源。 |
| device allocator | `request_admission` 构造 `ExtendAllocationIntent`；eviction gate 使用 `DeviceAllocatorLedger.available_pages()`，不从 radix occupancy 反推。 |
| request lifecycle | finished / unfinished 在 handler 内恢复 committed token path，插入 radix，并释放 duplicate / tail / overallocated KV 到 allocator ledger。 |
| capacity index | `HiCacheCapacityIndex` mutation-driven 维护 device/host leaf、occupied pages、reserved host pages、victim choice 和 audit trace。 |
| ref ledger | request / writeback / loadback / storage / prefetch owner 级 acquire/release，输出 ref mutation 和 tree ref audit。 |
| storage directory | 区分 materialized page record 与 backend-readable hash record；prefetch storage hit query 只保留连续 readable prefix。 |
| prefetch policy | wait-complete / best-effort / timeout 共用 operation lifecycle，planned path、hit prefix、reservation、anchor ref 和 apply/revoke/late/suppressed 分离。 |
| host cleanup | host allocation 失败按 SGLang request budget cleanup；victim 是 host-visible、evicted、无 ref 保护且无 backuped child 的 host radix leaf。 |
| write policy | write-through / selective / write-back 共享 host backup / storage readable / dirty clear / cleanup helper；ACK 时序当前按同步或 target control boundary 近似。 |

当前仍属于妥协或中长期缺口的部分记录在 `docs/validation/hicache_state_model_limitations.md`，包括 batch-level allocation intent、loadback intent / mem_quota、transition exactness 和异步 ACK / host release 的近似边界。

## 当前有效结果

### HCSV-20260618-5x3-matrix-after-reconstruction

这是当前 active final-state 结论。验证基于 5 个 HiCache config、3 个 manual input 的 full Python probe matrix；bench-generated input 已清理，不作为当前结果口径。

当前结果目录：

```text
data/profile_runs/sglang/20260618_204416_profiling_hicache_state_config_space_python_probe/modeling/hicache_state_matrix_validation_after_reconstruction
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

final-state matrix：

| 项 | 结果 |
| --- | ---: |
| self prediction | `15 / 15` pass |
| cross prediction，含 self 对角线 | `75 / 75` pass |
| validation ready | `75 / 75` ready |
| final-state pass rate | `1.0` |

按 input：

| input | self | cross |
| --- | ---: | ---: |
| `manual_phased_fast` | `5 / 5` | `25 / 25` |
| `manual_pressure_prefetch` | `5 / 5` | `25 / 25` |
| `manual_deeper_pressure_prefetch` | `5 / 5` | `25 / 25` |

按 input × target config 的 cross final-state 结果全部为 `5 / 5`：

| input | c0 | c1 | c2 | c3 | c4 |
| --- | ---: | ---: | ---: | ---: | ---: |
| `manual_phased_fast` | `5/5` | `5/5` | `5/5` | `5/5` | `5/5` |
| `manual_pressure_prefetch` | `5/5` | `5/5` | `5/5` | `5/5` | `5/5` |
| `manual_deeper_pressure_prefetch` | `5/5` | `5/5` | `5/5` | `5/5` | `5/5` |

结论：

- 当前 5x3 manual matrix 的 active final state 已闭环。
- 该闭环不依赖 `source_actual`、`timing_observation`、`oracle_state` 或 target run result 回写。
- bench-generated input 不在当前结果口径中；恢复 bench 前必须先证明 workload 输入跨配置稳定。
- final-state pass 不表示 transition exactness、operation intent 或 DAG patch 已通过。

### Transition Exactness 当前结果

transition exactness 使用同一 matrix 目录生成：

```text
transition_exactness_cross_matrix.json
```

当前结果：

| 层级 | 结果 |
| --- | ---: |
| prediction count | `75` |
| oracle / model self-check ready | `75 / 75` |
| T0 final-state exact | `75 / 75` |
| T1 transition-count exact | `25 / 75` |
| T2 page-lifecycle multiset exact | `25 / 75` |

失败分类：

| classification | count |
| --- | ---: |
| `matched` | `25` |
| `transition_semantic_or_snapshot_observability_mismatch` | `50` |

按 target config：

| target config | exact | T0 | T1 | T2 |
| --- | ---: | ---: | ---: | ---: |
| `c0_wt_timeout_p128_balanced` | `10 / 15` | `15 / 15` | `10 / 15` | `10 / 15` |
| `c1_wts_wait_p128_low_l1` | `0 / 15` | `15 / 15` | `0 / 15` | `0 / 15` |
| `c2_wb_best_effort_p64_low_l1` | `0 / 15` | `15 / 15` | `0 / 15` | `0 / 15` |
| `c3_wt_best_effort_p32_low_host` | `0 / 15` | `15 / 15` | `0 / 15` | `0 / 15` |
| `c4_wb_timeout_p64_low_capacity` | `15 / 15` | `15 / 15` | `15 / 15` | `15 / 15` |

按 input：

| input | exact | T0 | T1 | T2 |
| --- | ---: | ---: | ---: | ---: |
| `manual_phased_fast` | `10 / 25` | `25 / 25` | `10 / 25` | `10 / 25` |
| `manual_pressure_prefetch` | `10 / 25` | `25 / 25` | `10 / 25` | `10 / 25` |
| `manual_deeper_pressure_prefetch` | `5 / 25` | `25 / 25` | `5 / 25` | `5 / 25` |

解释：

- T0 已经是 final-state hard pass。
- T1/T2 的 50 个 mismatch 不是 final-state failure，而是 transition 语义或 snapshot 可观测性层面的差异。
- `locked_pages` 仍参与 T0 final-state 检查，但暂不参与 T1/T2 transient exactness；真实 lock/ref inc/dec 来自 `source_actual` evidence，按约束不能作为 state model input。
- `operation-intent` 当前只是 DAG patch 前的 scaffold，不能直接作为 patch 输入。

## 验证脚本职责

当前 active HiCache validation scripts：

| 脚本 | 职责 |
| --- | --- |
| `scripts/internal/hicache_state_cross_input_audit.py` | 跨配置 normal atomic invariant input contract 审计。 |
| `scripts/internal/hicache_state_matrix.py` | matrix profile discovery、target config 推导、quality/self/cross 共用编排逻辑。 |
| `scripts/internal/hicache_state_matrix_validation.py` | 执行 final-state quality / self / cross matrix validation；S1A/B 一对一是矩阵特例。 |
| `scripts/internal/hicache_state_provenance.py` | 基于 validation / predicted trace / oracle snapshot 的 mismatch 页面证据汇总，不回写模型。 |
| `scripts/internal/hicache_transition_exactness.py` | 只读 transition exactness 链路：model self-check、target oracle extraction、self/cross compare、operation intent scaffold。 |

这些脚本都不能生成 synthetic `model_input=true` 事件，不能修改 profiling trace，也不能把 `source_actual` / `timing_observation` /
`oracle_state` 写回 target state。

## 复现命令

基础检查：

```bash
scripts/run.sh modeling -- bash -lc \
  'cmake -S . -B build/modeling -G Ninja && cmake --build build/modeling --target trace_graph -j2'
scripts/run.sh modeling -- bash -lc \
  'python3 -m py_compile scripts/internal/model_runner.py scripts/internal/hicache_state_cross_input_audit.py scripts/internal/hicache_state_matrix.py scripts/internal/hicache_state_matrix_validation.py scripts/internal/hicache_state_provenance.py scripts/internal/hicache_transition_exactness.py'
find configs -name '*.json' -print0 | xargs -0 -n1 jq empty
git diff --check
```

跑 3 个 manual input 下的 5x5 final-state matrix：

```bash
python3 scripts/internal/hicache_state_matrix_validation.py \
  --profile-run-dir data/profile_runs/sglang/20260618_204416_profiling_hicache_state_config_space_python_probe \
  --output-dir data/profile_runs/sglang/20260618_204416_profiling_hicache_state_config_space_python_probe/modeling/hicache_state_matrix_validation_after_reconstruction \
  --stages quality,self,cross \
  --input manual_phased_fast \
  --input manual_pressure_prefetch \
  --input manual_deeper_pressure_prefetch
```

只跑某个 targeted 格子：

```bash
python3 scripts/internal/hicache_state_matrix_validation.py \
  --profile-run-dir <profile_run_dir> \
  --output-dir <profile_run_dir>/modeling/<targeted_output_dir> \
  --stages self \
  --input <input_id> \
  --source-config <config_id> \
  --target-config <config_id> \
  --force \
  --max-predictions 1
```

Transition exactness：

```bash
python3 scripts/internal/hicache_transition_exactness.py \
  --mode model-self-check \
  --prediction-dir <prediction_dir>

python3 scripts/internal/hicache_transition_exactness.py \
  --mode extract-target-oracle \
  --target-manifest <target_profile_manifest.json> \
  --output <matrix_dir>/observed_target_transitions/<input_id>/<config_id>.observed_target_transition_trace.json

python3 scripts/internal/hicache_transition_exactness.py \
  --mode compare-self \
  --prediction-dir <prediction_dir> \
  --observed-target-trace <observed_target_transition_trace.json>

python3 scripts/internal/hicache_transition_exactness.py \
  --mode compare-cross-matrix \
  --matrix-dir <hicache_state_matrix_validation_dir>
```

矩阵模式默认复用已生成的 target oracle；`--force` 会重建 full Python probe oracle，耗时明显更高，仅在脚本或 oracle 抽取逻辑变化后使用。

结果摘要读取：

```bash
jq '{prediction_count,
     validation_ready_count,
     final_state_match_count,
     final_state_pass_rate,
     by_input}' \
  <matrix_dir>/final_state_cross_5x4.json

jq '{prediction_count,
     ready_count,
     exact_count,
     t0_final_state_exact_count,
     t1_transition_count_exact_count,
     t2_page_lifecycle_multiset_exact_count,
     failure_classification_counts,
     by_input,
     by_target_config}' \
  <matrix_dir>/transition_exactness_cross_matrix.json
```

`final_state_self_5x4.json` 和 `final_state_cross_5x4.json` 是脚本沿用的历史文件名；当前 active 结果口径以文件内
`prediction_count`、`by_input` 和本文记录为准。

## 已关闭机制缺口

这些结论来自已迁移的临时诊断文档，不再作为单独文档维护。

### Allocator / Lifecycle

- device eviction gate 不再使用 `occupied_device_pages + reservation_pages > capacity` 这类 radix occupancy 反推；
- gate 对齐 SGLang `allocator.available_size() < requested_tokens`，其中 available 来自 allocator ledger；
- eviction budget 使用完整 allocation request，而不是只清理 deficit；
- request lifecycle 在 finished / unfinished 时释放 duplicate、tail 和 overallocated KV；
- `request_bound_match_anchor` 的 loadback allocation 当前只做 opportunistic promotion；需要 eviction 的 loadback 等待新的 loadback intent。

该机制关闭了早期 c2 self prediction 的 L1 mismatch。batch-level allocator 仍以 `batch_size=1` 为短期合同，详见限制文档。

### L2 / Host / Storage

- `backuped` 只表示 host copy，不把 storage-readable 直接当成 backuped；
- host cleanup 删除 host leaf subtree，并更新 parent/child topology 与 capacity index；
- timeout prefetch 不再因为 storage directory hit 就直接落 host，必须等 target policy 的 completed/apply 边界；
- target host/device capacity 从 SGLang server command 推导，包括 host pool page 对齐和 prefetch capacity limit；
- prefetch revoke / timeout incomplete 的 host reservation 不立即释放，而是保留 deferred release 近似；
- write-through backup ACK 前持有普通 lock ref，并在下一条 target control fact 近似 drain。

这些机制共同关闭了 15 个 manual self prediction 中的 L2/backuped/evicted/locked final-state mismatch，并在当前 75 个 prediction 中保持 final-state pass。

## 历史阶段摘要

### HCSV-20260613-host-release-policy-final3

- S1A/S1B 33-target atomic profile 四向 prediction 全部 final-state pass；
- S1B 早期 L2/backuped/evicted `70/55` residual 已通过 target-derived host release / cleanup policy 关闭；
- 该阶段证明 host release budget 必须来自 SGLang allocation request，而不是超容量拟合预算。

### HCSV-20260612-atomic-input-contract

- 旧 mixed roles 被拆掉：`request_tokens`、`lookup_path`、`request_cache_lifecycle` 不再是 normal role；
- source/control-flow 事实降级为 evidence：`insert_path`、`capacity_request`、`lock_scope_delta`、`maintenance_checkpoint` 等不更新 target state；
- 双向 cross audit 证明 atomic invariant contract 可以作为 state model 输入边界。

### HCSV-20260612-target-resource-mechanism

- request-derived device lock/ref、admission reservation、target L1 capacity pressure 和 dynamic device radix leaf victim 初步闭合；
- S1A target self/cross 已 final-state pass；
- S1B 剩余差异集中在 host/L2/storage/prefetch visibility，推动后续 host/device/async 边界重构。

## 下一步

当前不再围绕 final-state count 做局部补丁。后续优先级：

1. 针对 50 个 T1/T2 transition mismatch 做 family-level 逐 trace 诊断，优先 `c1` dirty oscillation、`c2` write-back/eviction 交错、`c3` low-host cleanup 和 deeper prefetch evicted marker。
2. 把 raw transition 聚合成 stable `CacheIntentLog`，作为 DAG patch 的输入层，而不是直接把 page-level transition 转成 graph mutation。
3. 为 loadback intent、batch-level allocation intent、maintenance/ACK boundary 设计 target-independent invariant；只有现有 target-derived 机制无法表达时才新增 profiling target。
4. 继续保持 `source_actual` / `timing_observation` / `oracle_state` 只做 evidence，不回到 normal state mutation。
