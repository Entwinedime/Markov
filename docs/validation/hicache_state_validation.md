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

### HCSV-20260610-four-way-s1a-s1b

目的：S1A 和 S1B profiling 均完成后，用同一批 token-invariant facts 做四个方向的 HiCache state prediction，
并用目标场景 oracle final state 验证模型正确性。

输入：

| 项 | 值 |
| --- | --- |
| profiling config | `configs/experiments/hicache_state/profiling_hicache_state_mainline_one_matrix.json` |
| S1A run | `20260610_073946_profiling_hicache_state_mainline_one_matrix/01_s1a_manual` |
| S1B run | `20260610_073946_profiling_hicache_state_mainline_one_matrix/03_s1b_manual` |
| S1A modeling config | `configs/modeling/hicache_state/modeling_hicache_state_mainline_one_prediction_s1a.json` |
| S1B modeling config | `configs/modeling/hicache_state/modeling_hicache_state_mainline_one_prediction_s1b.json` |
| S1A target config | page128, L1/L2 capacity `64/145`, `write_through_selective`, `wait_complete` |
| S1B target config | page64, L1/L2 capacity `128/321`, `write_back`, `best_effort` |

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
| S1A self | 32/54, missing 22 | 78/106, missing 28 | 78/106, missing 28 | 0/0 match | 46/52, missing 28, extra 22 | 11/11 match |
| S1B self | 62/108, missing 46 | 172/144, missing 36, extra 64 | 172/144, missing 36, extra 64 | 62/72, missing 10 | 172/108, extra 64 | 0/22, missing 22 |
| S1A -> S1B | 64/108, missing 44 | 164/144, missing 40, extra 60 | 164/144, missing 40, extra 60 | 64/72, missing 8 | 164/108, missing 4, extra 60 | 22/22 match |
| S1B -> S1A | 15/54, missing 39 | 79/106, missing 41, extra 14 | 79/106, missing 41, extra 14 | 0/0 match | 50/52, missing 41, extra 39 | 0/11, missing 11 |

Raw model behavior highlights：

| prediction | state trace events | model transitions | skipped non-invariant | notable raw final state |
| --- | ---: | ---: | ---: | --- |
| S1A self | 7375 | 18935 | 416 | L1 64, L2 145, L3 712, evicted 81, locked 22, prefetch ready 712 |
| S1B self | 6867 | 27941 | 296 | L1 124, L2 321, dirty 124, evicted 321, locked 0, prefetch suppressed 1484 |
| S1A -> S1B | 7375 | 27465 | 416 | L1 128, L2 321, dirty 128, evicted 321, locked 44, prefetch suppressed 1452 |
| S1B -> S1A | 6867 | 19305 | 296 | L1 30, L2 147, L3 756, evicted 89, locked 0, prefetch ready 728 |

结论：

- 新采集契约有效：S1A/S1B profile quality、token dictionary、seq order 和 invariant coverage 都通过。
- 后端输入分流有效：四个方向均无 `missing_invariant_facts` 和 `non_invariant_fact_usage`。
- 四个方向全部 final state mismatch，因此当前不能宣称 self-config 或 cross-config state prediction 通过。
- 失败是模型缺陷而不是采集目标范围缺失的直接证据；已知缺陷记录在 `hicache_state_model_defects.md`。
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

短期不应重新采集 profiling。当前采集已经足够暴露模型错误，应先做逐 page provenance：

1. 对 S1A 的 L1 missing / evicted extra 组，列出模型 transition trace 和 oracle final membership。
2. 对 S1B 的 raw L2/backuped/evicted 被推到 capacity 321 的路径，确认 capacity enforcement 和 write-back dirty eviction 是否过强。
3. 对 S1B dirty missing 组，确认 flush、dirty clear、backuped 标记和 eviction 的顺序。
4. 对 S1B self 与 S1B->S1A 中 locked 全丢的问题，追 lock/ref chain 如何从 source token path 映射到 target radix parent chain。
5. 对 S1A/S1B 的 prefetch ready/suppressed 极端行为，确认它是否影响 resident/L2 结果，而不是只作为附加状态。
6. 每修一类状态规则后重跑四向矩阵，不能只看单个 self-config。

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
