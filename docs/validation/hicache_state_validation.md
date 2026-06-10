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

截至 2026-06-10：

- HiCache profiling 使用 token dictionary/span、`cache_scope`、`seq_no` 和 `fact_class` 分类；
- C++ backend 只消费 `fact_class=invariant_state && state_model_input=true`；
- known invariant roles 是 `request_tokens`、`lookup_path`、`cache_config_observed`、`insert_path`、
  `prefetch_intent`、`prefetch_check_point`、`capacity_request`、`lock_scope_delta`；
- target pages 由 C++ 按 token path 和 target `page_size` 生成，不再消费 `target_page_identity_page64/128`；
- oracle page key 默认用 `oracle_page_key_mode=strip_scope` 和 raw snapshot hash 对比；
- HiCacheModule 仍是 state-only，不修改 DAG。

## 当前有效结果

### HCSV-20260610-token-backend-s1a

目的：验证 token-invariant profiling + C++ token backend 在 S1A self-config 上是否能与真实 oracle state 对齐。

输入：

| 项 | 值 |
| --- | --- |
| profiling config | `configs/experiments/hicache_state/profiling_hicache_state_mainline_one_matrix.json` |
| experiment | `01_s1a_manual` |
| run label | `20260610_073946_profiling_hicache_state_mainline_one_matrix/01_s1a_manual` |
| modeling config | `configs/modeling/hicache_state/modeling_hicache_state_mainline_one_prediction_s1a.json` |
| output label | `modeling/token_backend_s1a` |
| target config | page128, L1/L2 capacity `64/145`, `write_through_selective`, `wait_complete` |

Profile quality：

| 指标 | 值 |
| --- | ---: |
| `quality_ready` | true |
| `profiling_ready` | true |
| invariant events | 6960 |
| required end events | 3480 |
| token dictionary paths | 172 |
| token dictionary paths with ids | 172 |
| token span refs | 172 |
| missing token dictionary refs | 0 |
| seq order errors | 0 |
| route errors | 0 |

Model summary：

| 指标 | 值 |
| --- | ---: |
| `input_hicache_events` | 7376 |
| `processed_hicache_events` | 3480 |
| `skipped_non_invariant_events` | 416 |
| `state_transition_count` | 18935 |
| `dag_mutations` | 0 |
| `missing_invariant_facts` | `{}` |
| `non_invariant_fact_usage` | `[]` |

Validation：

| 指标 | 值 |
| --- | --- |
| `validation_ready` | false |
| `validation_errors` | `["hicache_final_state_mismatch"]` |
| `oracle_state_validation_required` | true |
| `oracle_page_key_mode` | `strip_scope` |
| `final_state_match` | false |
| `raw_final_state_match` | false |
| `invariant_coverage_ready` | true |

Normalized final-state diff：

| set | model | oracle | mismatch |
| --- | ---: | ---: | --- |
| `l1_resident_pages` | 32 | 54 | missing 22, extra 0 |
| `l2_resident_pages` | 78 | 106 | missing 28, extra 0 |
| `dirty_pages` | 0 | 0 | match |
| `backuped_pages` | 78 | 106 | missing 28, extra 0 |
| `evicted_pages` | 46 | 52 | missing 28, extra 22 |
| `locked_pages` | 11 | 11 | match |

Raw model counts before `strip_scope` normalization：

| set | count |
| --- | ---: |
| `l1_resident_pages` | 64 |
| `l2_resident_pages` | 145 |
| `l3_resident_pages` | 712 |
| `backuped_pages` | 145 |
| `evicted_pages` | 81 |
| `locked_pages` | 22 |
| `dirty_pages` | 0 |
| `prefetch_planned_pages` | 712 |
| `prefetch_ready_pages` | 712 |

结论：

- 新采集契约有效：profile quality 和 invariant coverage 通过。
- 后端不再需要大量分支猜 source/oracle/invariant：主入口已经用 `fact_class + state_model_input` 分流。
- state model 仍不正确：resident/backuped under-predict，evicted 同时有 missing 和 extra。
- dirty 和 locked final sets 已经对齐，但不能掩盖 resident/evicted mismatch。
- 当前不能宣称 S1A self-config prediction 通过。

首个 L1 normalized missing page：

```text
08c4433f3c8ddb201c1d2b54e9045b63308a491a20f6b8b6b6e4686b6cfd39be
```

该 page 也是 evicted extra sample，说明模型把部分 oracle-final L1 resident 页错误地留在 evicted 集合。

## 复现命令

Profile quality：

```bash
python3 scripts/internal/profile_quality.py \
  --manifest <run_dir>/profile_manifest.json \
  --output /tmp/profile_quality_s1a_token_backend.json
```

Modeling：

```bash
python3 scripts/internal/model_runner.py \
  --config configs/modeling/hicache_state/modeling_hicache_state_mainline_one_prediction_s1a.json \
  --profile-manifest <run_dir>/profile_manifest.json \
  --output-dir <run_dir>/modeling/token_backend_s1a \
  --mode cache_state \
  --emit-module-summary \
  --emit-validation
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
  <run_dir>/modeling/token_backend_s1a/validation.json
```

## 下一步

短期不应重新采集 profiling。当前采集已经足够暴露模型错误，应先做逐 page provenance：

1. 对 `08c4433f...` 这类 L1 missing / evicted extra page，列出模型 transition trace 和 oracle final membership。
2. 对 L2/backuped missing 的 28 页，确认它们来自 lookup/load-back、insert/write-through-selective、prefetch ready 还是容量回收。
3. 对 evicted missing/extra 分组，判断是 target radix leaf group、touch order、capacity request、locked/evictable predicate 还是 write-through hit count。
4. 修 C++ state model 后重跑 S1A。
5. S1A self-config 通过后，再跑 S1B self-config。
6. 两个 self-config 都通过后，再进入 S1A->S1B / S1B->S1A cross-config。

只有逐 trace 证明现有 invariant facts 无法区分某类真实机制时，才进入下一轮集中重采。

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
