# HiCache State 验证与跨配置预测记录

## 当前进展

本文件记录 HiCache state 验证的长期结论、关键 run 索引和后续计划。当前文档已按
2026-06-08 02:12 +0800 的项目状态核对。
最新结论是：同配置 state replay 已经闭环，write policy、capacity、prefetch policy 等多组跨配置
prediction 已通过；strict page64 prediction 的 final-state 也已闭环，但 timeline delta 仍未完全对齐，
因此后续若要继续收敛，应优先用 prediction state trace 和真实 state trace 排查 transition granularity，
再决定是否进入 state-to-DAG patch 设计。

本文中的 `data/profile_runs/...` 和 `data/modeling_runs/...` 路径是历史验证索引和复现入口名称。
这些目录属于可再生运行产物，允许在本地清理；长期事实以本文档、主线开发文档和仓库内配置为准。

当前完成项：

- Python probe 已支持 validation-only `state_snapshot`，且通过 `model_input=false` 与性能 DAG 隔离。
- C++ `HiCacheModule` 已重构为 state-only 面向对象实现，能维护 resident、dirty、backuped、evicted、
  locked、prefetch planned/ready/suppressed 等状态，当前保持 `dag_mutations=0`。
- base 同配置 replay 已通过：`20260607_144832_profiling_hicache_state_validation/modeling/cache_state_replay_after_prefix_v1`
  的 final state 对齐，L1 `56/56`、L2 `121/121`、backuped `121/121`、evicted `65/65`、
  prefetch planned/ready/suppressed `166/166`、`8/8`、`158/158`。
- page64 target 同配置 replay 已通过：
  `20260607_133143_profiling_hicache_state_page64_validation/modeling/cache_state_replay_pagehash_concat_v1`
  的 final state 对齐，L1 `111/111`、L2 `250/250`、backuped `250/250`、evicted `139/139`、
  prefetch planned/ready/suppressed `364/364`、`17/17`、`347/347`。
- capacity / policy 事实已经能随 validation-only state snapshot 采集，并在 profile quality、
  validation 和推荐配置输出中作为审计证据使用。
- `input.target_experiment_config` 已能从目标实验配置的 SGLang 启动参数和 `modeling.hicache`
  显式字段生成 C++ target config；capacity 只做审计，除非目标配置显式提供有效 page budget。

当前未完成项：

- strict page64 的 final-state 已对齐到 oracle：L1 `111/111`、L2 `250/250`、backuped `250/250`、
  evicted `139/139`、prefetch planned/ready/suppressed `364/364`、`17/17`、`347/347`；
  最新 strict 输出目录 `predict_page64_state_strict_lock_skip_v1` 已把 validation gate 跑通：
  `validation_ready=true`、`final_state_match=true`、`timeline_delta_validation.match=true`。
- 这次闭环的关键点已经落在代码里：
  - `lock_ref_inc` / `lock_ref_dec` 在 page-size mismatch 下被视为 non-invariant observed facts；
  - HiCache 默认不再输出 transition digest，只在显式 `emit_state_digests=true` 时保留；
  - 这让 strict 结果从此前的 GB 级输出降到可重验的 77MB 级别。
- 目前只剩 `exact_match=false` 的诊断差异：
  - `model_extra_transition_count=0`、`oracle_extra_transition_count=34`；
  - `mark_evicted` / `clear_evicted` 仍有 17 个 page 的 oracle-only transient；
  - 这部分不再阻塞 validation gate，更像是 state trace granularity 差异。
- 这意味着 page64 已经不再是 final-state / timeline 失败项；若后续还要继续收敛，只需要把
  exact mismatch 再下钻，或者直接转入 state-to-DAG patch 设计。
- state-to-DAG patch、cache patch 下的 E2E what-if 性能预测仍未实现，不属于当前已完成能力。

下一步优先级：

1. 若还要把 `exact_match=true` 也收掉，就继续下钻 `mark_evicted` / `clear_evicted` 的 17 个 oracle-only
   transient；否则可以把 page64 作为 validation gate 完成项转入 state-to-DAG patch。
2. 把这次 `l3_to_l2_transfer` / `prefetch_progress` 的尾页补齐逻辑整理成稳定规则，避免同类 page size
   mismatch 再次出现 ready/suppressed 反转。
3. state-to-DAG patch 仍建议在 page64 诊断结论稳定后再开始，避免用 E2E 误差掩盖 state trace 问题。

| 状态 | 内容 | 结果 |
| --- | --- | --- |
| 已完成 | `sglang.hicache` Python probe 增加 `hicache_state:self`，由 `profiling.python_probe.state_trace.enabled=true` 显式开启。 | 默认不采 state snapshot，验证配置可采。 |
| 已完成 | `state_snapshot` 标记 `model_input=false`，C++ Chrome reader 不将其放入性能 DAG。 | validation-only 事件与业务执行事件隔离。 |
| 已完成 | C++ `HiCacheModule` 重构为 `HiCacheFactParser`、`HiCacheState`、`HiCacheStateModel` 等面向对象骨架。 | 当前只维护 state，`dag_mutations=0`。 |
| 已完成 | `model_runner.py` 输出 `predicted_target_cache_state_trace.json` 和 `validation.json.hicache_state`。 | 支持同配置 replay 和后续跨配置 prediction diff。 |
| 已完成 | 新增 `configs/experiments/hicache_state/profiling_hicache_state_validation.json`、`configs/experiments/hicache_state/profiling_hicache_state_write_back_validation.json` 和 `configs/modeling/hicache_state/modeling_hicache_state_validation.json`。 | state validation 和 write-back target oracle 有固定入口。 |
| 已完成 | 完整机制 base profiling run：`data/profile_runs/sglang/20260606_185311_profiling_hicache_state_validation`。 | `profile_quality.quality_ready=true`，lookup/insert/load_back/evict/prefetch/write 全部出现，必须带 page identity 的 `522` 个状态事件全部有 page identity。 |
| 已完成 | 完整机制 base 同配置 replay：`data/profile_runs/sglang/20260606_185311_profiling_hicache_state_validation/modeling/cache_state_replay`。 | `validation_ready=true`、`final_state_match=true`、`missing_page_identity_events=0`。 |
| 已完成 | write-back target profiling run：`data/profile_runs/sglang/20260606_191934_profiling_hicache_state_write_back_validation`。 | `profile_quality.quality_ready=true`，write-back 下不强制要求 `prefetch_transfer`。 |
| 已完成 | write-back target 同配置 replay：`data/profile_runs/sglang/20260606_191934_profiling_hicache_state_write_back_validation/modeling/cache_state_replay`。 | `validation_ready=true`、`final_state_match=true`，dirty page oracle 按 `L1 resident && !backuped` 派生。 |
| 已完成 | validation oracle 修正为按进程取最终 `state_snapshot.nodes` 并 union。 | 避免用单个最后 snapshot 或旧 `derived` 摘要误判多进程 state。 |
| 已完成 | C++ state 中 L2 resident 与 SGLang `TreeNode.backuped` 语义对齐。 | L2 resident、backuped、dirty eviction writeback 的 synthetic fixture 和真实 replay 均通过。 |
| 已完成 | 第一组真实跨配置 state prediction。 | `write_through` base -> `write_back` target 已通过，final L1/L2/dirty/backuped/evicted sets 全部对齐。 |
| 已完成 | validation 增加 `transition_coverage` 摘要。 | 可按 transition kind、operation kind、source event、page 覆盖定位缺失解释；目前仍不是逐 transition oracle。 |
| 已完成 | `page_hashes:` source 支持裸字面量 target page size。 | 可采集 `target_page_identity`，例如 `page_hashes:arg:tokens,64`；已通过 fixture，并已用于真实 page64 prediction。 |
| 已完成 | page size 变化时跳过缺少 target page identity 的 base movement。 | `load_back/write/transfer/remove_page` 等观测 movement 不再污染 target state，并输出 `skipped_non_invariant_events`。 |
| 已完成 | 新增 page64 state prediction 配置入口。 | `configs/modeling/hicache_state/modeling_hicache_state_prediction_page64.json` 固定 target `page_size=64`；已使用带 `target_page_identity` 的 base profile 完成真实 oracle 对比。 |
| 已完成 | 新增 page64 target profiling 配置并完成真实 target replay。 | `20260606_195957_profiling_hicache_state_page64_validation` 的同配置 replay 已通过，L1/L2/backuped/evicted final set 全部对齐。 |
| 部分完成 | page64 真实跨配置 prediction。 | 旧 core/ignored-debug 口径下 base `20260606_194950` -> target `20260606_195957` 曾对齐 L1/L2/dirty/backuped/evicted；当前 strict 口径下最新 base `20260607_170641` -> target `20260607_133143` 已对齐 final-state，但 timeline 仍有诊断差异。 |
| 已完成 | capacity target profiling、同配置 replay 和真实跨配置 prediction。 | base `20260606_212731` -> target `20260606_211101` 已通过，L1/L2/dirty/backuped/evicted final set 全部对齐；建模 config 使用有效 L1 page budget `46`。 |
| 已完成 | 新一轮完整机制 workload profiling 和同配置 replay。 | base `20260607_031354_profiling_hicache_state_validation` 已通过，lookup/insert/load_back/evict/prefetch/write 全部出现；`validation_ready=true`、`final_state_match=true`。 |
| 已完成 | prefetch wait target profiling、同配置 replay 和跨配置 prediction。 | target `20260607_032622_profiling_hicache_state_prefetch_wait_validation` 已通过；base -> wait_complete prediction 已通过，`prefetch_planned_pages=166/166`、`prefetch_ready_pages=8/8`、`prefetch_suppressed_pages=158/158`。 |
| 已完成 | prefetch best_effort target profiling、同配置 replay 和跨配置 prediction。 | target `20260607_035602_profiling_hicache_state_prefetch_best_effort_validation` 已通过；base -> best_effort prediction 已通过，`prefetch_planned_pages=166/166`、`prefetch_ready_pages=8/8`、`prefetch_suppressed_pages=158/158`。 |
| 已完成 | prefetch aggressive timeout target profiling、同配置 replay 和跨配置 prediction。 | target `20260607_041431_profiling_hicache_state_prefetch_timeout_aggressive_validation` 已通过；base -> timeout prediction 已通过，`prefetch_planned_pages=166/166`、`prefetch_ready_pages=8/8`、`prefetch_suppressed_pages=158/158`。 |
| 已完成 | 完整 write policy final-set model。 | `write_through`、`write_back`、`write_through_selective` 都已有真实 replay/prediction；selective 修复了按 TP/cache scope 维护 hit_count、request_id 缺失时用同 scope 最近 lookup path 配对，以及显式 write policy 下消费 CPU_PINNED remove_page 清理 L2。 |
| 已完成 | transition oracle coverage。 | 已新增 event delta validation 和 timeline delta validation。真实 selective replay 中 shared exclusive event key `242` 个，`event_delta_validation.match=true`；timeline coverage 口径下 `model_transition_covered=true`、`model_extra_transition_count=0`，同时保留 `exact_match=false` 和 `oracle_extra_transition_count=348` 作为多进程稀疏 snapshot 暴露的 oracle-only 震荡诊断。 |
| 已完成 | lock / evictable state replay 和跨配置 prediction。 | C++ state 已维护 `locked_pages` 和按 scope+page 的 lock ref count；state validation 配置已采集 `inc_lock_ref` / `dec_lock_ref`；新 base run `20260607_053949` 与带 `object_id` 的 selective target `20260607_063721` 均完成真实 replay，并完成 base -> `write_through_selective` target prediction，final state 全部对齐、`locked_pages=0/0`、`missing_page_identity_events=0`、timeline coverage 通过。 |
| 已完成 | `write_back + low capacity` 组合 profiling、replay 和跨配置 prediction。 | 新 target run `20260607_073450` 与新 base run `20260607_083213` 均通过 profile quality 和同配置 replay；fresh base -> target prediction 已通过，L1 `46/46`、L2 `88/88`、dirty `38/38`、backuped `88/88`、evicted `80/80`、prefetch ready `8/8`、locked `0/0`。 |
| 已完成 | validation oracle 修正 lock/ref 尾部缺失 snapshot。 | final oracle 在有 `lock_ref_inc` / `lock_ref_dec` facts 时用 refcount 覆盖 `locked_pages`；timeline oracle 对尾部缺失 end snapshot 增加 `final_lock_timeline_correction`，本轮 target replay 补齐 8 个 `clear_locked`。 |
| 已完成 | validation oracle 修正 prefetch ready evidence。 | `prefetch_ready_pages` 不只来自 `prefetch_progress_state`，也来自 `l3_to_l2_transfer_end` 的完成证据；避免把真实完成的 8 页误判为 late/suppressed。 |
| 已完成 | timeline coverage 只比较 completed snapshot 可见字段。 | write-through 下 `dirty_pages` 可能只在模型内部瞬时出现，completed snapshot 不暴露；timeline validation 会把这类字段放入 `ignored_unobservable_state_keys`，final-state 和 event delta 仍正常校验。 |
| 已完成 | validation-only `state_snapshot.capacity` 采集和汇总。 | `sglang.hicache` probe 已采集 page size、write policy、prefetch policy、L1/L2 pool capacity/available、prefetch threshold/capacity limit；`profile_quality.json.hicache_capacity` 和 `validation.json.hicache_state.oracle_capacity_summary` 会汇总这些证据。 |
| 已完成 | HiCache state trace 的 capacity 采集质量门槛。 | `profiling.python_probe.state_trace.enabled=true` 时，如果没有任何 `state_snapshot.capacity`，`profile_quality.quality_ready=false` 并输出 `hicache_capacity_snapshot_missing`。 |
| 已完成 | target C++ HiCache config 与 oracle capacity/policy 事实的一致性审计。 | `validation.json.hicache_state.capacity_config_audit` 会区分 match、not_configured、target_below_observed_pool、target_exceeds_observed_pool、target_below_oracle_final_count 等情况，用于定位 prediction 配置错误。 |
| 已完成 | 新 probe capacity 字段真实 SGLang run 验证。 | run `20260607_095024_profiling_hicache_state_validation` 的 `profile_quality.quality_ready=true`，capacity snapshot `4824` 个；同配置 replay `validation_ready=true`、`final_state_match=true`。 |
| 已完成 | oracle observed max state counts。 | `validation.json.hicache_state.oracle_observed_max_state_counts` 记录 state snapshot 时间线中的峰值；本轮真实 run 中 L1 raw pool `64`、observed max `62`、final `56`，L2 raw pool `129`、observed max `128`、final `121`。 |
| 已完成 | 从 target oracle capacity/policy 事实推荐 C++ HiCache config。 | `capacity_config_audit.recommended_target_config` 会推荐 page size、write policy、prefetch policy；capacity 只在 target config 已显式设置时复制，未显式设置时记录为 `not_auto_recommended`。 |
| 已完成 | 推荐 HiCache config 文件落盘。 | validation 成功生成推荐配置时，runner 会写出 `recommended_hicache_cpp_model_config.json`，格式可直接传给 C++ TraceGraph `--model-config`。 |
| 已完成 | 推荐配置语义回归验证。 | 旧规则把 observed max 当 capacity 会导致显式推荐 config replay 失败；v2 规则生成 policy-only config 后，`cache_state_replay_recommended_config_v2` 已通过，final state 全部对齐。 |
| 已完成 | 在无 target oracle 的纯 what-if 场景从 target experiment config 生成 C++ HiCache config。 | `model_runner.py` 支持 `input.target_experiment_config`；从 `server.command` 抽取 page size、write policy、prefetch policy、timeout 参数，并从 `modeling.hicache` 读取显式有效 capacity；不会按 `max-total-tokens/hicache-ratio` 粗算 capacity。 |
| 已完成 | explicit prefetch finalization。 | `best_effort` 在 trace 结束时把 planned 但未 ready 的 pages 归入 suppressed；`timeout` 只对已有 timeout/terminated 证据的 request 做 suppressed，普通 schedule-only page 不强行 suppressed。 |
| 已完成 | final-state diff 收紧和 legacy ignore 显式化。 | oracle-only state key 默认参与比较；旧 page64 / capacity / prefetch policy prediction 缺少 lock/ref model input 时，`locked_pages` 只作为 validation-only ignore，差异保留在 `ignored_sets_diff_by_tier`。 |
| 部分完成 | page64 / capacity / best_effort derived target config 回归。 | capacity、best_effort 等回归已通过；page64 在旧 core 口径通过，但 strict prefetch/final-state 口径未闭环。 |
| 未完成 | state-to-DAG patch 和 what-if E2E 性能预测。 | 不属于本阶段实现目标。 |

最新 capacity snapshot 真实验证：

| 指标 | 结果 |
| --- | --- |
| `capacity_snapshot_run_dir` | `data/profile_runs/sglang/20260607_095024_profiling_hicache_state_validation` |
| `capacity_snapshot_profile_quality.quality_ready` | `true` |
| `capacity_snapshot_profile_quality.hicache_capacity_observed` | `true`，snapshot `4824` 个，`HiRadixCache=4112`、`HiCacheController=712` |
| `capacity_snapshot_profile_quality.missing_cache_mechanisms` | `[]` |
| `capacity_snapshot_profile_quality.stateful_required_events_missing_page_identity` | `0` |
| `capacity_snapshot_observed_policy` | `write_policy=write_through`、`prefetch_policy=timeout` |
| `capacity_snapshot_observed_capacity` | L1 raw pool `64` pages；L2 raw pool `129` pages；prefetch capacity limit `52` pages |
| `capacity_snapshot_replay_dir` | `data/profile_runs/sglang/20260607_095024_profiling_hicache_state_validation/modeling/cache_state_replay_capacity_snapshot_v2` |
| `capacity_snapshot_replay.validation_ready` | `true` |
| `capacity_snapshot_replay.final_state_match` | `true` |
| `capacity_snapshot_replay.final_state_counts` | L1 `56/56`、L2 `121/121`、backuped `121/121`、evicted `65/65`、locked `0/0`、prefetch planned `166/166`、ready `8/8`、suppressed `158/158` |
| `capacity_snapshot_replay.oracle_observed_max_state_counts` | L1 max `62`、L2 max `128`、backuped max `128`、evicted max `73`、locked max `10` |
| `capacity_snapshot_replay.recommended_target_config` | `page_size=128`、`write_policy=write_through`、`prefetch_policy=timeout`；L1/L2 capacity 证据记录为 `not_auto_recommended` |
| `capacity_snapshot_replay.recommended_config_path` | `modeling/cache_state_replay_capacity_snapshot_v2/recommended_hicache_cpp_model_config.json` |
| `capacity_snapshot_recommended_config_replay_dir` | `data/profile_runs/sglang/20260607_095024_profiling_hicache_state_validation/modeling/cache_state_replay_recommended_config_v2` |
| `capacity_snapshot_recommended_config_replay.validation_ready` | `true` |
| `capacity_snapshot_recommended_config_replay.final_state_match` | `true`，L1 `56/56`、L2 `121/121`、backuped `121/121`、evicted `65/65`、prefetch planned `166/166`、ready `8/8`、suppressed `158/158` |
| `capacity_snapshot_replay.event_delta_validation.match` | `true`，shared event key `222` 个 |
| `capacity_snapshot_replay.timeline_delta_validation.match` | `true`，`model_extra_transition_count=0`、`oracle_extra_transition_count=16` |

最新 `write_back + low capacity` 组合验证结果：

| 指标 | 结果 |
| --- | --- |
| `write_back_capacity_target_run_dir` | `data/profile_runs/sglang/20260607_073450_profiling_hicache_state_write_back_capacity_validation` |
| `write_back_capacity_target_profile_quality.quality_ready` | `true`，`missing_cache_mechanisms=[]` |
| `write_back_capacity_target_replay_dir` | `data/profile_runs/sglang/20260607_073450_profiling_hicache_state_write_back_capacity_validation/modeling/cache_state_replay_v4` |
| `write_back_capacity_target_replay.validation_ready` | `true` |
| `write_back_capacity_target_replay.final_state_match` | `true`，L1 `46/46`、L2 `88/88`、dirty `38/38`、backuped `88/88`、evicted `80/80`、locked `0/0` |
| `write_back_capacity_target_replay.event_delta_validation.match` | `true`，shared event key `289` 个 |
| `write_back_capacity_target_replay.timeline_delta_validation.match` | `true`，`model_extra_transition_count=0`，`oracle_extra_transition_count=592` |
| `write_back_capacity_base_run_dir` | `data/profile_runs/sglang/20260607_083213_profiling_hicache_state_capacity_base_validation` |
| `write_back_capacity_base_profile_quality.quality_ready` | `true`，`missing_cache_mechanisms=[]` |
| `write_back_capacity_base_replay_dir` | `data/profile_runs/sglang/20260607_083213_profiling_hicache_state_capacity_base_validation/modeling/cache_state_replay_v4` |
| `write_back_capacity_base_replay.validation_ready` | `true` |
| `write_back_capacity_base_replay.final_state_match` | `true`，L1 `56/56`、L2 `126/126`、dirty `0/0`、backuped `126/126`、evicted `70/70`、prefetch ready `8/8` |
| `write_back_capacity_prediction_dir` | `data/profile_runs/sglang/20260607_083213_profiling_hicache_state_capacity_base_validation/modeling/predict_write_back_capacity_state_20260607_073450_l2cap88` |
| `write_back_capacity_prediction.validation_ready` | `true` |
| `write_back_capacity_prediction.final_state_match` | `true`，L1 `46/46`、L2 `88/88`、dirty `38/38`、backuped `88/88`、evicted `80/80`、prefetch ready `8/8`、locked `0/0` |
| `write_back_capacity_prediction.timeline_delta_validation.match` | `true`，跨配置 event key 不可比，因此 event delta 不作为硬门槛 |
| `write_back_capacity_prediction_derived_config_dir` | `data/profile_runs/sglang/20260607_083213_profiling_hicache_state_capacity_base_validation/modeling/predict_write_back_capacity_state_derived_target_experiment_v2` |
| `write_back_capacity_prediction_derived_config.cpp_model_config` | 从 `configs/experiments/hicache_state/profiling_hicache_state_write_back_capacity_validation.json` 派生：`write_policy=write_back`、`prefetch_policy=timeout`、timeout `10/0/10`、`page_size=128`、L1 `46`、L2 `88` |
| `write_back_capacity_prediction_derived_config.validation_ready` | `true` |
| `write_back_capacity_prediction_derived_config.final_state_match` | `true`，L1 `46/46`、L2 `88/88`、dirty `38/38`、backuped `88/88`、evicted `80/80`、prefetch planned `326/326`、ready `8/8`、suppressed `318/318` |

本轮新增结论：

- `write_back + low capacity` target 的有效 L2 page budget 是 `88`，不是之前沿用的 `96`；
  新增的 `state_snapshot.capacity` 会在之后的新 profiling run 中暴露 L1/L2 pool 容量、可用量和
  policy 参数，用于减少手工配置。
- 当前 capacity/policy 事实只是 validation/debug 输出；它不会进入 C++ DAG 输入，也不会自动覆盖
  target config。后续需要用真实 run 验证 `profile_quality.hicache_capacity` 与
  `validation.hicache_state.oracle_capacity_summary` 是否足以解释有效 budget。
- lock/ref final state 不能只相信最后一个 completed snapshot；尾部可能有 `dec_lock_ref_end`
  facts 但没有对应 end snapshot，validation 必须使用 lock facts 做最终 refcount 修正。
- prefetch ready evidence 应优先相信完成的 `l3_to_l2_transfer_end`；仅从
  `prefetch_progress_state` 推断会把已经 transfer 完成的 page 误分到 late/suppressed。
- timeline coverage 是 snapshot 可见字段上的覆盖检查；对于 completed snapshot 不暴露的瞬时字段，
  例如 write-through 下的 dirty transient，应记录为 `ignored_unobservable_state_keys`，不作为模型错误。

最近一次完整验证结果：

| 指标 | 结果 |
| --- | --- |
| `base_run_dir` | `data/profile_runs/sglang/20260606_185311_profiling_hicache_state_validation` |
| `target_run_dir` | `data/profile_runs/sglang/20260606_191934_profiling_hicache_state_write_back_validation` |
| `cross_prediction_dir` | `data/profile_runs/sglang/20260606_185311_profiling_hicache_state_validation/modeling/predict_write_back_state_20260606_191934` |
| `base_profile_quality.quality_ready` | `true` |
| `target_profile_quality.quality_ready` | `true` |
| `base_replay.validation_ready` | `true` |
| `target_replay.validation_ready` | `true` |
| `cross_prediction.validation_ready` | `true` |
| `cross_prediction.final_state_match` | `true` |
| `cross_prediction.l1_resident_pages` | `56 / 56` 对齐 |
| `cross_prediction.l2_resident_pages` | `118 / 118` 对齐 |
| `cross_prediction.dirty_pages` | `48 / 48` 对齐 |
| `cross_prediction.backuped_pages` | `118 / 118` 对齐 |
| `cross_prediction.evicted_pages` | `110 / 110` 对齐 |

当前 page64 核对结果：

| 指标 | 结果 |
| --- | --- |
| `page64_base_run_dir` | `data/profile_runs/sglang/20260607_144832_profiling_hicache_state_validation` |
| `page64_target_run_dir` | `data/profile_runs/sglang/20260607_133143_profiling_hicache_state_page64_validation` |
| `page64_base_replay_dir` | `data/profile_runs/sglang/20260607_144832_profiling_hicache_state_validation/modeling/cache_state_replay_after_prefix_v1` |
| `page64_base_replay.validation_ready` | `true` |
| `page64_base_replay.final_state_match` | `true`，L1 `56/56`、L2 `121/121`、backuped `121/121`、evicted `65/65`、prefetch planned/ready/suppressed `166/166`、`8/8`、`158/158` |
| `page64_target_replay_dir` | `data/profile_runs/sglang/20260607_133143_profiling_hicache_state_page64_validation/modeling/cache_state_replay_pagehash_concat_v1` |
| `page64_target_replay.validation_ready` | `true` |
| `page64_target_replay.final_state_match` | `true`，L1 `111/111`、L2 `250/250`、backuped `250/250`、evicted `139/139`、prefetch planned/ready/suppressed `364/364`、`17/17`、`347/347` |
| `page64_strict_prediction_dir` | `data/profile_runs/sglang/20260607_170641_profiling_hicache_state_validation/modeling/predict_page64_state_strict_lock_skip_v1` |
| `page64_strict_prediction.validation_ready` | `true` |
| `page64_strict_prediction.final_state_match` | `true` |
| `page64_strict_prediction.core_counts` | L1 `111/111`、L2 `250/250`、dirty `0/0`、backuped `250/250`、evicted `139/139`、locked `0/0` |
| `page64_strict_prediction.prefetch_counts` | planned `364/364`、ready `17/17`、suppressed `347/347` |
| `page64_strict_prediction.timeline_delta_validation` | `match=true`、`exact_match=false`、`model_extra_transition_count=0`、`oracle_extra_transition_count=34` |

结论：当前 page64 同配置 replay 和 strict prediction 的 validation gate 都已闭环；剩余 only-oracle 的
evicted transient 只影响 `exact_match`，不再阻塞 page64 作为完成项进入后续 patch 阶段。

历史 page64 实验结果（旧 core 口径，不能代表当前 strict 状态）：

| 指标 | 结果 |
| --- | --- |
| `page64_base_run_dir` | `data/profile_runs/sglang/20260606_194950_profiling_hicache_state_validation` |
| `page64_target_run_dir` | `data/profile_runs/sglang/20260606_195957_profiling_hicache_state_page64_validation` |
| `page64_base_replay_dir` | `data/profile_runs/sglang/20260606_194950_profiling_hicache_state_validation/modeling/cache_state_replay_rerun_leaf_group` |
| `page64_base_replay.validation_ready` | `true` |
| `page64_base_replay.final_state_match` | `true` |
| `page64_base_replay.l1_resident_pages` | `56 / 56` 对齐 |
| `page64_base_replay.l2_resident_pages` | `121 / 121` 对齐 |
| `page64_base_replay.backuped_pages` | `121 / 121` 对齐 |
| `page64_base_replay.evicted_pages` | `65 / 65` 对齐 |
| `page64_target_replay_dir` | `data/profile_runs/sglang/20260606_195957_profiling_hicache_state_page64_validation/modeling/cache_state_replay_rerun_leaf_group` |
| `page64_target_replay.validation_ready` | `true` |
| `page64_target_replay.final_state_match` | `true` |
| `page64_target_replay.l1_resident_pages` | `111 / 111` 对齐 |
| `page64_target_replay.l2_resident_pages` | `250 / 250` 对齐 |
| `page64_target_replay.backuped_pages` | `250 / 250` 对齐 |
| `page64_target_replay.evicted_pages` | `139 / 139` 对齐 |
| `page64_prediction_dir` | `data/profile_runs/sglang/20260606_194950_profiling_hicache_state_validation/modeling/predict_page64_state_core_v5_transfer_identity` |
| `page64_prediction.validation_ready` | `true` |
| `page64_prediction.final_state_match` | `true` |
| `page64_prediction.l1_resident_pages` | `111 / 111` 对齐 |
| `page64_prediction.l2_resident_pages` | `250 / 250` 对齐 |
| `page64_prediction.dirty_pages` | `0 / 0` 对齐 |
| `page64_prediction.backuped_pages` | `250 / 250` 对齐 |
| `page64_prediction.evicted_pages` | `139 / 139` 对齐 |
| `page64_prediction.ignored_sets_diff_by_tier` | `locked_pages=0/17`；prefetch planned `366/364`、ready `16/17` 仍作为诊断输出。 |

page64 strict prefetch 诊断：

| 指标 | 结果 |
| --- | --- |
| `page64_strict_prefetch_dir` | `data/profile_runs/sglang/20260606_194950_profiling_hicache_state_validation/modeling/predict_page64_state_strict_prefetch_v2_transfer_identity` |
| `page64_strict_prefetch.validation_ready` | `false`，因为 hard diff 中的 prefetch debug 集合未完全对齐。 |
| `page64_strict_prefetch.final_state_match` | `false`，只因为 prefetch / lock debug 集合未完全对齐。 |
| `page64_strict_prefetch.core_state` | L1 `111/111`、L2 `250/250`、dirty `0/0`、backuped `250/250`、evicted `139/139` 已对齐。 |
| `page64_strict_prefetch.prefetch_planned_pages` | `366/364` |
| `page64_strict_prefetch.prefetch_ready_pages` | `16/17` |
| `page64_strict_prefetch.diagnosis` | 旧 base trace 的部分 target prefetch identity 仍来自 base page size 的 `last_hash` 链，缺少完整 target prefix token path。 |

page64 prediction 本轮修复点：

- base run 里的 observed movement 即使带 `target_page_identity`，也不能直接当作 target movement；
- 已补最小 target radix prefix skeleton：lookup 记录 target page path，page size 变化时 insert 使用
  target 已知 prefix 计算 suffix；
- page64 prediction 的 L1/L2 capacity 来自 target experiment config 的
  `modeling.hicache.l1_capacity_pages=128` / `l2_capacity_pages=256`，不是由
  `--max-total-tokens` 或 `--hicache-ratio` 自动粗算；
- C++ state model 已记录 target insert suffix 形成的 leaf group，eviction 按 leaf group
  释放，而不是逐 page 裁剪到容量上限；
- L2 host eviction 已按 SGLang 语义清理 `evicted` 状态，避免把已经从 host 删除的 page
  继续留在最终 evicted set；
- page size mismatch 下带 `target_page_identity` 的 `l3_to_l2_transfer` 会作为
  prefetch ready evidence 消费；旧 base run 已把 page64 ready 从 `0/17` 推进到 `16/17`；
- profiling 已新增 `page_hashes_after_prefix:<prefix_tokens>,<tokens>,<page_size>`，后续新 run
  会用 `prefix_keys` 在 target page size 下重算 parent hash，并且只输出
  `new_input_tokens` 对应的 suffix pages，避免继续使用 base page size 的 `last_hash`，
  也避免把 prefix pages 混入 prefetch planned set；
- 当前 diff 已能证明 page size 变化下的 target radix split、capacity eviction 和 final state
  可以由 base 不变量 facts + target config 推导。

本轮导致 state replay 无法对齐的关键问题和修复：

- validation 旧逻辑只取全局最后一个 `derived` snapshot；真实 run 有两个进程，应按进程取最终
  snapshot 后 union。
- `derived` 是 probe 侧调试摘要，曾出现与同一 snapshot 中 `nodes` 原始字段不一致；validation
  已改为从 `nodes` 重新派生 oracle 集合。
- C++ state 旧逻辑把 L2 resident 和 `backuped` 当成可分离状态；SGLang `TreeNode.backuped`
  实际等价于 `host_value is not None`，因此 L2 resident 必须同步 backuped。
- dirty eviction 写回 host 后必须产生 L2 resident；否则 backuped 和 resident 集合会不一致。
- SGLang snapshot 中 `dirty` 字段当前不能直接表达 HiCache write-back 的未备份页；validation
  oracle 改为用 `has_device_value && !backuped && !evicted` 派生 dirty。
- 跨配置 prediction 不能消费 base trace 中的 L2 host eviction；显式 target policy 下这些事件
  不是不变量，当前在 state prediction 中跳过，后续由 target capacity/policy 模型接管。

最新 capacity 实验结果：

| 指标 | 结果 |
| --- | --- |
| `capacity_base_run_dir` | `data/profile_runs/sglang/20260606_212731_profiling_hicache_state_capacity_base_validation` |
| `capacity_target_run_dir` | `data/profile_runs/sglang/20260606_211101_profiling_hicache_state_capacity_validation` |
| `capacity_base_profile_quality.quality_ready` | `true` |
| `capacity_target_profile_quality.quality_ready` | `true` |
| `capacity_base_replay.final_state_match` | `true`，L1 `56 / 56`、L2 `126 / 126`、backuped `126 / 126`、evicted `70 / 70` |
| `capacity_target_replay.final_state_match` | `true`，L1 `46 / 46`、L2 `96 / 96`、backuped `96 / 96`、evicted `50 / 50` |
| `capacity_prediction_dir` | `data/profile_runs/sglang/20260606_212731_profiling_hicache_state_capacity_base_validation/modeling/predict_capacity_state_20260606_211101_rerun7_validation_visibility` |
| `capacity_prediction.final_state_match` | `true` |
| `capacity_prediction.l1_resident_pages` | `46 / 46` 对齐 |
| `capacity_prediction.l2_resident_pages` | `96 / 96` 对齐 |
| `capacity_prediction.dirty_pages` | `0 / 0` 对齐 |
| `capacity_prediction.backuped_pages` | `96 / 96` 对齐 |
| `capacity_prediction.evicted_pages` | `50 / 50` 对齐 |

capacity prediction 本轮修复点：

- 显式 `write_through` target 会跳过 base trace 中 observed write movement，因此 `insert`
  必须同步补出 L2、L3 readable set 和 backuped 状态；否则后续 lookup / load 无法从 L3 推导。
- 同 page size 的 target capacity eviction 不应沿用 page-size mismatch 的 leaf group 整组驱逐；
  page size 不变时按 page 精确驱逐，page size 变化时才按 target insert suffix group 驱逐。
- insert 前的 capacity pre-allocation 不能按整个 insert page 数驱逐，必须只驱逐
  `current + new_unique - capacity` 的超额部分；否则存在已 resident page 时会过驱逐。
- 本次低容量 target 的 SGLang 有效 L1 page budget 是 `46`，不是
  `max_total_tokens / page_size = 6144 / 128 = 48` 的粗算值；`l2_capacity_pages=96`
  与 target oracle 对齐。后续 profiling 应采集真实有效 capacity 字段，避免手工配置。

最新 prefetch wait 实验结果：

| 指标 | 结果 |
| --- | --- |
| `prefetch_base_run_dir` | `data/profile_runs/sglang/20260607_031354_profiling_hicache_state_validation` |
| `prefetch_wait_target_run_dir` | `data/profile_runs/sglang/20260607_032622_profiling_hicache_state_prefetch_wait_validation` |
| `prefetch_base_profile_quality.quality_ready` | `true` |
| `prefetch_wait_profile_quality.quality_ready` | `true` |
| `prefetch_wait_profile_quality.missing_cache_mechanisms` | `[]` |
| `prefetch_base_replay.final_state_match` | `true`，L1 `56 / 56`、L2 `121 / 121`、backuped `121 / 121`、evicted `65 / 65` |
| `prefetch_base_replay.prefetch_planned_pages` | `166 / 166` 对齐 |
| `prefetch_base_replay.prefetch_ready_pages` | `8 / 8` 对齐 |
| `prefetch_wait_target_replay.final_state_match` | `true`，L1 `56 / 56`、L2 `121 / 121`、backuped `121 / 121`、evicted `65 / 65` |
| `prefetch_wait_target_replay.prefetch_planned_pages` | `166 / 166` 对齐 |
| `prefetch_wait_target_replay.prefetch_ready_pages` | `8 / 8` 对齐 |
| `prefetch_wait_prediction_dir` | `data/profile_runs/sglang/20260607_031354_profiling_hicache_state_validation/modeling/predict_prefetch_wait_state_20260607_032622` |
| `prefetch_wait_prediction.final_state_match` | `true` |
| `prefetch_wait_prediction.prefetch_planned_pages` | `166 / 166` 对齐 |
| `prefetch_wait_prediction.prefetch_ready_pages` | `8 / 8` 对齐 |
| `prefetch_wait_prediction.unchecked_model_state_keys` | `l3_resident_pages` |

prefetch wait 当前结论：

- `prefetch_schedule` 只表示 policy planned pages，不能直接当作 ready；ready 必须来自
  `check_prefetch_progress` / operation progress evidence，或已完成的 `l3_to_l2_transfer_end`。
- 旧的 `wait_complete => all scheduled pages ready` 假设已被修正。真实 target 中 planned
  pages 为 `166`，但可由 progress evidence 证明 ready 的 pages 是 `8`。
- validation 现在只在 oracle trace 暴露 `operation_hash_pages` 时验证 ready/late/suppressed；
  只有 schedule、没有 progress operation pages 的 request 只能验证 planned。
- 当前 workload 已能验证 planned/ready/late/suppressed。best_effort 和 aggressive timeout
  target 都已产生可验证的 late/suppressed oracle，并完成跨配置 prediction。

最新 transition oracle 进展：

| 指标 | 结果 |
| --- | --- |
| `selective_replay_timeline_v4_dir` | `data/profile_runs/sglang/20260607_063721_profiling_hicache_state_write_through_selective_validation/modeling/cache_state_replay_timeline_v4` |
| `final_state_match` | `true` |
| `missing_page_identity_events` | `0` |
| `event_delta_validation.ready` | `true` |
| `event_delta_validation.comparable` | `true` |
| `event_delta_validation.match` | `true`，shared exclusive event key 上 page delta 对齐。 |
| `event_delta_validation.shared_event_key_count` | `242` |
| `timeline_delta_validation.ready` | `true` |
| `timeline_delta_validation.match` | `true`，表示模型输出的 transition 全部被 raw snapshot timeline 覆盖。 |
| `timeline_delta_validation.exact_match` | `false`，raw snapshot timeline 还有 oracle-only transient oscillation。 |
| `timeline_delta_validation.model_extra_transition_count` | `0` |
| `timeline_delta_validation.oracle_extra_transition_count` | `348`，主要来自多进程 cache object 稀疏采样暴露的 lock/evicted 短暂重复翻转。 |
| `timeline_delta_validation.predicted_transition_count` | `361` |
| `timeline_delta_validation.oracle_transition_count` | `381` |

event delta validation 当前口径：

- inclusive oracle 保留每个 start/end snapshot 的包围差分，用于观察真实调用包含了哪些状态变化；
- exclusive oracle 只比较没有嵌套 state snapshot 的调用，避免把外层函数包含的内层
  HiCache 状态变化误判成模型归因错误；
- mismatch 只在 predicted 和 oracle 都有 shared exclusive event key 时比较 page delta；
- 跨配置 prediction 因 base run 和 target run 时间戳不同，不能用 exact event key 比较，
  仍以 final-state、policy oracle 和 transition coverage 为主；
- `locked_pages` 只有在模型也输出 lock transition 时才参与 event delta 比较，否则作为
  `ignored_state_keys_without_predicted_transition` 暴露。

timeline delta validation 当前口径：

- raw timeline 沿 `state_snapshot.object_id` 排序，并对所有 HiRadixCache 对象状态做 union；
- `exact_match=true` 表示模型 transition 与 raw snapshot timeline 的 kind/page multiset 完全相等；
- `match=true` 表示模型 transition 全部被 raw snapshot timeline 覆盖，即
  `model_extra_transition_count=0`；
- raw snapshot 不是完整状态日志，多进程场景下某个 cache object 长时间未被采样时，下一次任意
  HiCache 调用的 snapshot 可能暴露之前已经发生的状态变化，因此 oracle-only transition 作为
  `oracle_extra_transition_count` 诊断，不直接判定 state model 错误；
- 同配置 replay 必须同时满足 final-state match、event delta match 和 timeline coverage match。

最新 lock-enabled replay 结果：

| 指标 | 结果 |
| --- | --- |
| `lock_enabled_run_dir` | `data/profile_runs/sglang/20260607_053949_profiling_hicache_state_validation` |
| `profile_manifest.profiling_ready` | `true` |
| `profile_quality.quality_ready` | `true` |
| `profile_quality.observed_cache_mechanisms.lock_ref` | `872` |
| `profile_quality.stateful_required_events_missing_page_identity` | `0` |
| `lock_replay_dir` | `data/profile_runs/sglang/20260607_053949_profiling_hicache_state_validation/modeling/cache_state_replay_lock_v2` |
| `validation_ready` | `true` |
| `final_state_match` | `true` |
| `missing_page_identity_events` | `0` |
| `locked_pages` | `0 / 0` 对齐 |
| `lock_state_events` | `708` |
| `mark_locked` / `clear_locked` | `918 / 918` |
| `event_delta_validation.match` | `true` |
| `event_delta_validation.shared_event_key_count` | `224` |
| `event_delta_validation.ignored_state_keys_without_predicted_transition` | `[]` |

本轮修复点：

- `HiRadixCache.inc_lock_ref` / `dec_lock_ref` 会沿父链更新到 root；
- root 节点没有 page identity，且 `lock_delta=0` 时是 no-op 观测；
- C++ parser 和 profile quality 都将这类 no-op lock 事件从 strict page identity 缺失中排除；
- 非 root lock events 仍必须携带 page identity，否则 state replay 不能通过。

最新 lock-enabled selective replay / prediction 结果：

| 指标 | 结果 |
| --- | --- |
| `lock_selective_target_run_dir` | `data/profile_runs/sglang/20260607_063721_profiling_hicache_state_write_through_selective_validation` |
| `target_profile_manifest.profiling_ready` | `true` |
| `target_profile_quality.quality_ready` | `true` |
| `target_profile_quality.observed_cache_mechanisms.lock_ref` | `872` |
| `target_profile_quality.stateful_required_events_missing_page_identity` | `0` |
| `target_replay_dir` | `data/profile_runs/sglang/20260607_063721_profiling_hicache_state_write_through_selective_validation/modeling/cache_state_replay_timeline_v4` |
| `target_replay.final_state_match` | `true` |
| `target_replay.locked_pages` | `0 / 0` 对齐 |
| `target_replay.event_delta_validation.match` | `true` |
| `target_replay.event_delta_validation.shared_event_key_count` | `242` |
| `target_replay.timeline_delta_validation.match` | `true` |
| `target_replay.timeline_delta_validation.exact_match` | `false`，`oracle_extra_transition_count=348` |
| `prediction_dir` | `data/profile_runs/sglang/20260607_053949_profiling_hicache_state_validation/modeling/predict_write_through_selective_state_20260607_063721_timeline_v4` |
| `prediction.final_state_match` | `true` |
| `prediction.locked_pages` | `0 / 0` 对齐 |
| `prediction.missing_page_identity_events` | `0` |
| `prediction.event_delta_validation.comparable` | `false`，跨配置 run 时间戳不同，不能用 exact event key 比较。 |
| `prediction.timeline_delta_validation.match` | `true` |
| `prediction.timeline_delta_validation.model_extra_transition_count` | `0` |
| `prediction.timeline_delta_validation.oracle_extra_transition_count` | `348` |

## 目标

本阶段的目标不是做 DAG patch，也不是用一次真实运行的 E2E 时间证明模型“看起来还行”。
本阶段只解决一个问题：**HiCache cache state 是否能由 profiling 采集到的不变量和目标 cache
配置正确推导出来**。

这样做的直接目的，是为下一阶段 DAG patch 切开责任边界：

- 如果下一阶段 E2E 预测不准，应优先怀疑 state-to-DAG 映射、DAG anchor、duration 或
  dependency，而不是反复怀疑 cache state 是否推错。
- 如果 cache state 推导不准，本阶段必须能在 state validation 中直接定位到 request、
  operation、page、transition，而不是等到 E2E 残差里混合暴露。

因此本阶段要证明两件事：

1. profiling 采集到了一组在 cache 配置变化下仍然成立的不变量；
2. 这些不变量加上目标 cache 配置，足以让 C++ HiCache state model 推导出目标配置下真实会
   发生的 cache state transition。

当前已完成四类验证：

- 同配置 replay：用同一条真实 trace 中的 HiCache facts 推导 state，并用同一 run 中的 SGLang
  state snapshot 做 oracle。
- write/page-size/capacity 跨配置 prediction：用 base run 的不变量 facts 和 target cache
  config 推导 target state，再用真实 target run 的 state snapshot 验证。
- prefetch policy 跨配置 prediction：wait_complete 的 planned/ready 已验证，
  best_effort 和 aggressive timeout 的 late/suppressed 已验证。

下一步仍要保留 transition oracle coverage，但重心应转向两件事：第一，构造更完整的组合
workload，覆盖低容量 write-back dirty eviction、page size 变化下的 lock/eviction 交互等
组合机制；第二，在 state 已闭环的前提下，开始设计 state-to-DAG patch 的输入边界和验证方法。

这里的“目标 trace”不是指直接复制真实 target run 中观测到的 cache 操作，而是指：

```text
base profiling/merged trace 中的不变量 facts + target cache config
  -> C++ HiCache state model
  -> predicted target cache state trace
```

然后再用真实 target run 采集到的 state trace 做验证。

## 总体分层

HiCache 验证分三层，越往下定位能力越强。

| 层级 | 验证对象 | 作用 |
| --- | --- | --- |
| Summary 验证 | page count、final resident set、transition count | 快速发现明显错误，但不能作为主要验收。 |
| 逐 trace 验证 | request / operation / page / state transition | 本阶段主要验收方法，用于定位 state 推导错误。 |
| E2E 验证 | 端到端预测时间 | 本阶段只作为旁路 sanity check，真正 E2E 验收属于 DAG patch 阶段。 |

本阶段优先使用逐 trace 验证。原因是 cache state 错误可能不立即体现在 E2E 上，而 E2E
错误也可能来自 DAG 映射、带宽、同步边或 duration，不能用来判断 state 本身是否正确。

## Trace 输入边界

State validation 不能改变 modeling 主线对 trace 的定义。

长期架构中，`faithful_replay` 必须消费完整真实 merged trace。完整真实执行事件包括 torch
事件、LD_PRELOAD 事件，以及 Python probe 采到的 HiCache / CPUInfer 等真实执行路径事件。
关闭子模块只表示不做 DAG patch，不表示过滤某类真实事件。

本计划中提到“state-only 实验可关闭 torch”，只是一条降低实验开销的快速验证路径：

- 它只能用于验证 HiCache state model 是否能根据 HiCache facts 推导状态；
- 它不能替代 faithful replay 验证；
- 它不能作为 trace merger 或 DAG builder 的过滤规则；
- 当目标是验证 base DAG 或 cache patch 时，仍应采集并消费完整真实执行 trace。

需要从性能 DAG 隔离的是非执行类验证事件：

- `state_snapshot`
- oracle state
- probe 内部 debug
- validation diff
- profiling quality 证据

这些事件不能作为默认性能 DAG 节点。推荐输出到独立 state/debug trace；如果必须放入 Chrome
trace sidecar，则必须标记 `model_input=false`，并使用非执行 event kind，使 TraceGraph 构建
DAG 时跳过它们，只允许 validation/debug 读取。

## Cache 如何建模

### 模型输入

C++ HiCache state model 的输入分三类：

1. **profiling 不变量事实**
   - 来自 base profiling trace；进入 C++ 后通常表现为 trace merger 生成的 complete merged trace。
   - 在 cache 配置变化后仍然成立。
   - 用于预测 target state。

2. **target cache config**
   - page size；
   - L1/L2/L3 capacity；
   - write policy；
   - prefetch policy；
   - eviction policy；
   - storage backend / timeout / threshold；
   - 带宽、固定延迟等后续 DAG patch 需要的参数。

3. **oracle facts**
   - 只用于 replay validation 或 target validation 对比。
   - 不能参与 what-if state 推导。
   - 例如真实 SGLang state snapshot、base hit/miss、base node id、base trace 中实际
     load_back 次数。

### 状态对象

HiCache state model 需要维护以下对象。

| 状态 | 含义 |
| --- | --- |
| `radix_tree` | target page size 下的 radix tree，维护 prefix、split、node/page 映射。 |
| `l1_resident_pages` | device KV cache 中当前 resident pages。 |
| `l2_resident_pages` | host KV cache 中当前 resident pages。 |
| `l3_evidence_pages` | storage 中被写入或查询命中的 pages；不要求枚举全量 L3。 |
| `dirty_pages` | 已生成或修改，但尚未观察到备份的 pages。 |
| `backuped_pages` | 已备份到 L2 或 L3 的 pages。 |
| `evicted_pages` | 被从 L1 或 L2 释放的 pages。 |
| `locked_pages` | 被 request、prefetch、backup 或 load 保护，不能被 eviction 选择的 pages。 |
| `touch_order` | 用于 LRU-like eviction 的访问顺序。 |
| `prefetch_planned_pages` | policy 计划预取的 pages。 |
| `prefetch_ready_pages` | 已完成并可用于后续 hit 的 prefetch pages。 |
| `prefetch_late_pages` | 计划过但未赶上使用点的 prefetch pages。 |

### 状态转移

每次 cache 行为都应被表达为统一 state transition：

```text
RequestFact / CacheOperationFact / StorageEvidence
  -> PolicyDecision
  -> CacheStateTransition
  -> PredictedCacheOperation
```

必须覆盖的 transition：

- `lookup`
  - 根据 target page size 重新计算 page set；
  - 在 target radix tree 和 resident sets 中判断 L1/L2/L3/miss；
  - 更新 touch order 和 hit count。

- `insert`
  - 将新生成 KV page 插入 target radix tree；
  - 标记 L1 resident；
  - 根据 write policy 标记 dirty 或触发 write。

- `load`
  - L2 hit 时生成 L2->L1 load；
  - L3 hit 且需要 foreground load 时生成 L3->L2，再生成 L2->L1；
  - load 完成后更新 resident sets。

- `prefetch`
  - 根据 target prefetch policy、threshold、timeout、capacity 判断 planned/ready/late/suppressed；
  - prefetch ready 时更新 L2 resident；
  - late 或 suppressed 必须记录原因，不能静默丢弃。

- `write`
  - `write_through`：insert 后触发 L1->L2 和必要的 L2->L3；
  - `write_through_selective`：根据 hit count / backuped / policy threshold 判断；
  - `write_back`：insert 后只标 dirty，eviction 或 flush 才写回。

- `evict`
  - 根据 capacity 和 locked state 选择候选；
  - clean eviction 只更新 resident；
  - dirty eviction 必须先生成 writeback transition，再释放 resident。

### 面向对象结构

当前 `hicache_model.cpp` 中匿名函数过多，需要重构为面向对象结构。

建议对象边界：

- `HiCacheFact`：从 `TraceEvent` 提取出的稳定事实，包含 role、phase、page set、tier、
  request/operation id、timestamp、缺失字段状态。
- `HiCacheFactParser`：只负责识别 HiCache event、解析 role/page/tier，不修改状态。
- `HiCacheState`：维护 resident、dirty、backuped、evicted、prefetch、lock、touch order。
- `HiCachePolicyModel`：根据 target config 做 write / prefetch / eviction / load 决策。
- `HiCacheStateTransition`：记录一次状态转移的 before/after 摘要、触发 fact、transition kind、
  page set。
- `HiCacheStateModel`：按 trace 顺序消费 facts，调用 state 和 policy，输出 predicted state trace。
- `HiCacheSummaryBuilder`：只负责 JSON summary，避免状态机直接拼 JSON。

保留外部入口：

```cpp
HiCacheSummary apply_hicache_model(DagGraph& graph, const HiCacheConfig& config);
```

本阶段行为边界：

- `dag_mutations = 0`
- 不修改 node duration
- 不新增或删除 edge
- 不做 state-to-DAG patch
- 不使用 observed replay fallback
- 验证逻辑不能进入默认业务路径；state trace、state diff、debug 输出必须显式开启。

## 验证逻辑与业务代码隔离

HiCache state validation 是验证工具链，不是业务推理路径的一部分。实现时必须把验证逻辑和真正
业务代码主体分离，避免为了验证而污染默认 profiling、modeling 或 SGLang 运行行为。

隔离原则：

- 默认 SGLang 运行不输出 state snapshot；
- 默认 `python_probe` 只采配置中声明的普通 facts，不采完整 state trace；
- 默认 C++ `HiCacheModule` 只执行功能 state model，不输出逐 transition debug；
- 默认 `prediction.json` 仍只包含 `predicted_e2e_ns`；
- state validation 的 trace、diff、coverage、mismatch 都只能在显式验证模式下输出。

建议开关：

| 层级 | 开关 | 行为 |
| --- | --- | --- |
| Python probe | `TRACE_SIM_HICACHE_STATE_TRACE=1` | 允许 `sglang.hicache` probe 采集 `state_snapshot`。 |
| Profiling config | `profiling.python_probe.state_trace.enabled=true` | 为 HiCache targets 附加 `hicache_state:self` 字段。 |
| C++ compile | `TRACE_SIM_ENABLE_HICACHE_STATE_VALIDATION` | 可选编译开关，用于编译较重的 validation-only helper。 |
| Modeling CLI | `--emit-validation --emit-module-summary` | 生成 state validation diff 和 invariant coverage。 |
| Modeling config | `validation.hicache_state.enabled=true` | 打开 HiCache state replay / prediction validation。 |

实现约束：

- 优先使用命令行参数和 config 开关；只有会明显增加二进制依赖、编译时间或运行开销的验证 helper，
  才使用编译选项隔离。
- validation-only 代码不能被默认业务路径调用。
- validation-only 字段不能成为默认模型输入。
- SGLang third_party 源码不应加入常驻 state emitter；默认方案是在外部 `python_probe`
  中读取状态快照。
- 如果未来必须修改 SGLang 源码，必须用环境变量保护，并保证未开启时没有额外状态遍历和 IO。
- 真实实验配置中打开 state trace 是为了验证；普通 profiling 配置不得默认打开完整 state snapshot。

推荐目录/模块边界：

- Python probe 中的 state snapshot 代码放在 `sglang_hicache_callable.py` 的专用 helper 中，
  并受 `TRACE_SIM_HICACHE_STATE_TRACE` 控制。
- C++ state model 的功能代码和 validation diff 代码分开；功能代码只维护 state，validation
  代码只读取功能输出和 oracle trace 做对比。
- `scripts/internal/model_runner.py` 只在 `--emit-validation` 且 config 开启 HiCache state
  validation 时调用 diff 逻辑。

## 原始 Trace 采集什么

Profiling 必须只采事实，不做 target 行为推断。采集字段分两类：不变量字段和 oracle 字段。

### 必须采集的不变量字段

这些字段用于从 base profiling/merged trace 预测 target trace。

| 类别 | 字段 | 用途 |
| --- | --- | --- |
| request | `request_id`、request 顺序、phase、时间戳 | 建立 request 顺序和 state 更新顺序。 |
| token path | canonical token path、RadixKey 输入、prefix token ids 或等价 hash-chain 输入 | 在 target page size 下重新计算 page set。完整 token 序列只允许在显式验证模式下采集，并应设置长度上限或摘要策略。 |
| page hash input | page size、prior hash、hash chain 输入 | 重新计算 target page identity。不能只依赖 base page id。 |
| operation anchor | `operation_id`、target id、event role、timestamp | 将事实排序，并为后续 DAG patch 保留 anchor。state-only 阶段缺 DAG anchor 不应导致 state 推导失败，但必须记录 patch readiness 缺口。 |
| lookup input | lookup key、request id、prefix scope | 在 target radix tree 中重新执行 match。 |
| insert input | inserted key/value token length、priority、chunked | 推导 target insert pages 和 tree split。 |
| write policy input | hit count、backuped、dirty、write policy、threshold | 推导是否写 L2/L3。 |
| prefetch input | new input tokens、last hash、threshold、timeout、request 使用点 | 推导 planned/ready/late/suppressed。 |
| storage evidence | storage query hashes、batch hit count、transfer completion、write hashes | 判断 L3 readable/writeable evidence。 |
| capacity input | L1/L2 capacity、requested allocation size、locked/evictable 状态 | 推导 eviction。 |
| DAG anchor | request/scheduler/runtime/cache IO/storage IO 对应 trace node | 后续 state-to-DAG patch 使用。 |

### 只能用于验证的 oracle 字段

这些字段可以用来判断 replay 是否对齐，但不能用于 target what-if 推导。

| 字段 | 原因 |
| --- | --- |
| base page count | page size 变化后不成立。 |
| base page identity | 只有 target page size 和 hash 规则相同时才成立。 |
| SGLang radix node id | node split 和 target page size 变化后不稳定。 |
| base hit/miss 结果 | target resident state 变化后不成立。 |
| base load_back / prefetch / write 次数 | target policy 和 capacity 变化后不成立。 |
| base final resident set | target capacity、page size、policy 变化后不成立。 |
| SGLang state snapshot | 只能作为同配置或 target run 的 oracle。 |

### 采集渠道

| 渠道 | 采集内容 | 本阶段用途 |
| --- | --- | --- |
| `python_probe` | request、HiRadixCache、HiCacheController、state snapshot | HiCache state validation 的主输入。 |
| `ld_preload` | native runtime、sync、storage/native IO anchor | 保留后续 DAG patch anchor；state 阶段只做辅助。 |
| `torch` | runtime/kernel/copy/DAG anchor | state-only 快速验证可关闭；faithful replay、base DAG 验证和 cache patch 验证必须打开。 |

真实 state-only validation 快速配置：

```json
"channels": ["python_probe", "ld_preload"],
"torch": {"enabled": false}
```

这样可以降低真实实验开销，同时保留 state 所需事实和 native anchor。但它不是 faithful replay
配置，不能用于评价 base DAG 是否能完整重放真实运行。

完整闭环配置仍应启用：

```json
"channels": ["torch", "python_probe", "ld_preload"],
"torch": {"enabled": true}
```

该配置用于验证完整 merged trace、base DAG faithful replay，以及后续 cache patch 的端到端影响。

## SGLang State Trace 采集

当前状态：**已完成基础采集路径，并已通过完整机制 workload 验证采集覆盖。**

State trace 通过现有 `sglang.hicache` Python probe 采集，不直接修改 SGLang 源码。
完整 state snapshot 必须由显式验证开关打开，默认 profiling 不采集该字段。

`src/profiling/python_probe/trace_sim_probe/probes/sglang_hicache_callable.py`
已增加专用 source extractor：

```text
hicache_state:self
```

该 extractor 在现有 callable wrapper 的 start/end 事件中读取 `self`，生成 `state_snapshot`。
如果 `TRACE_SIM_HICACHE_STATE_TRACE` 未开启，extractor 应返回缺省未启用状态，而不是遍历 radix tree。

`state_snapshot` 是 oracle/validation 数据，不是业务执行事件。它不应作为普通 `ph=X` duration
event 进入性能 DAG。当前实现写入 Python probe sidecar，但设置
`model_input=false`、`event_kind=state_snapshot`，并确保 trace merger /
ChromeTraceIO / DagBuilder 不把它创建成 DAG node。

validation 读取 oracle 时以 `state_snapshot.nodes` 为准重新派生集合状态，不直接信任
snapshot 中旧的 `derived` 调试摘要。真实双进程运行中，validation 会按 `(trace_path, pid)`
取每个进程的最终 snapshot，再对 L1/L2/dirty/backuped/evicted 等 page set 做 union，和 C++
state model 当前聚合所有进程事件的口径保持一致。

后续如果 state snapshot 体积影响 trace 合并性能，可以改为独立 state trace 文件，并在
profile manifest 中单独记录；这不是当前阶段阻塞项。

真实执行事件，例如 lookup、load、prefetch、insert、write、evict，仍应设置 `model_input=true`
并进入 faithful replay 的完整 merged trace。

### HiRadixCache Snapshot

遍历 radix tree nodes，至少记录：

- `node_id`
- `parent_id`
- `hash_value`
- `key_token_length`
- `has_device_value`
- `has_host_value`
- `evicted`
- `backuped`
- `hit_count`
- `lock_ref`
- `host_ref_counter`
- `child_count`

由 node snapshot 派生：

- `l1_resident_pages`
- `l2_resident_pages`
- `dirty_pages`
- `backuped_pages`
- `locked_pages`

### HiCacheController Snapshot

至少记录：

- load queue 长度；
- write queue 长度；
- prefetch queue 长度；
- backup queue 长度；
- ack queue 长度；
- ongoing operation id；
- operation hash pages；
- completed tokens；
- prefetch occupied tokens；
- storage backend enabled/type。

### L3 限制

不要求从 SGLang storage backend 枚举全量 L3 page。

L3 只通过 evidence 对比：

- `write_storage`
- `_page_backup`
- `_storage_hit_query`
- `_page_transfer`

因此 validation 中 L3 不使用 final full set 对齐，而使用 read/write evidence 对齐。

## 如何从原始 Trace 预测目标 Trace

当前状态：**接口、输出骨架和第一组真实跨配置 write-back prediction 已完成。**

### 输入

预测 target trace 的输入是：

```text
base profiling/merged trace 中的不变量 facts
target cache config
```

不能读取 target actual trace，也不能直接使用 base run 中观测到的 load/write/prefetch 次数。

### 输出

输出是 predicted target cache state trace，而不是 DAG patch：

```text
predicted_target_cache_state_trace.json
```

每条记录至少包含：

- `request_id`
- `operation_id`
- `source_fact_id`
- `target_page_set`
- `decision_kind`
- `decision_reason`
- `transition_kind`
- `tier_src`
- `tier_dst`
- `before_state_digest`
- `after_state_digest`
- `predicted_operation_kind`
- `blocking_class`
- `unresolved_inputs`

### 推导流程

1. 已完成：从 merged trace 中读取 HiCache facts，并生成 `predicted_target_cache_state_trace.json`。
2. 已完成：按 event role / page identity 维护 L1/L2/L3 resident、dirty、backuped、evicted、prefetch planned/ready/late/suppressed。
3. 已完成：target config 已支持 `page_size`、L1/L2 capacity、write policy、prefetch policy；
   write-back、write_through_selective、page64 page-size、capacity、prefetch wait、prefetch best_effort 和
   aggressive timeout prediction 已通过真实跨配置验证。
4. 部分完成：已实现最小 target radix prefix skeleton，并用 target insert suffix 维护 leaf group；
   当前足以通过 page-size/capacity/selective final-state prediction。后续若要覆盖更严格
   transition oracle，还需要补 evictable lock 和完整 node split 语义。
5. 已完成：同配置 replay 和 `write_through -> write_back` 跨配置 prediction 中已验证
   dirty / backuped / writeback 基础状态。
6. 已完成：已有 LRU-like capacity 骨架、target capacity movement skip、synthetic fixture 和
   真实 target capacity prediction。
7. 已完成：prefetch planned/ready/late/suppressed 已有真实 replay 和跨配置 prediction；
   wait_complete、best_effort、timeout 三类 stop policy 都已有 target oracle。
8. 部分完成：已有 `missing_page_identity_events`、`missing_invariant_facts`、final set diff 和
   request-level coverage 输出；request/transition 时间线逐步对齐仍需增强。

### 禁止行为

预测 target trace 时禁止：

- 用 base trace 中实际 `load_back` 次数驱动 target load；
- 用 base trace 中实际 `prefetch` 次数驱动 target prefetch；
- 用 base trace 中实际 `write_storage` 次数驱动 target write；
- 用 base final resident set 初始化 target resident set；
- 用 SGLang node id 作为 target radix node identity；
- 在缺 page/token/path 事实时伪造 page identity。

缺关键事实必须暴露为 `missing_invariant_facts`。
page size 变化时，base `load_back`、write、transfer、remove/evict 等 movement 不是关键不变量；
缺少 target page identity 时必须跳过并计入 `skipped_non_invariant_events`，不能用于更新
target state。

## 验证方法

### Replay 验证：自己和自己

当前状态：**完整机制 workload 的 base、wait_complete target 和 write-back target 真实 replay 均已通过。**

Replay 验证用于证明 C++ state model 能正确解释一条真实 trace。

流程：

```text
base真实运行
  -> base profiling/merged trace + base state snapshot
  -> C++ state model 使用 base config 推导 predicted_base_state_trace
  -> predicted_base_state_trace 对比 base state snapshot / base state trace
```

Replay 验证允许使用 oracle 字段做对比，但不能让模型依赖 oracle 字段推导。

验收：

- `state_trace_ready=true`
- `final_state_match=true`
- request-level transition 顺序一致；
- page-level resident / dirty / backuped / evicted 一致；
- `missing_page_identity_events=0` 或所有缺失都有明确非状态原因；
- `non_invariant_fact_usage=[]`

Replay 不通过时，先修 fact parser、state transition 或 state snapshot 采集，不进入 prediction 验证。

已通过的真实 replay：

```text
data/profile_runs/sglang/20260607_031354_profiling_hicache_state_validation
  -> modeling/cache_state_replay

data/profile_runs/sglang/20260607_032622_profiling_hicache_state_prefetch_wait_validation
  -> modeling/cache_state_replay

data/profile_runs/sglang/20260606_191934_profiling_hicache_state_write_back_validation
  -> modeling/cache_state_replay
```

base replay 结果：

- `validation_ready=true`
- `state_trace_ready=true`
- `final_state_match=true`
- `invariant_coverage_ready=true`
- `missing_page_identity_events=0`
- `l1_resident_pages=56/56`
- `l2_resident_pages=121/121`
- `dirty_pages=0/0`
- `backuped_pages=121/121`
- `evicted_pages=65/65`
- `prefetch_planned_pages=166/166`
- `prefetch_ready_pages=8/8`

wait_complete target replay 结果：

- `validation_ready=true`
- `state_trace_ready=true`
- `final_state_match=true`
- `invariant_coverage_ready=true`
- `missing_page_identity_events=0`
- `l1_resident_pages=56/56`
- `l2_resident_pages=121/121`
- `dirty_pages=0/0`
- `backuped_pages=121/121`
- `evicted_pages=65/65`
- `prefetch_planned_pages=166/166`
- `prefetch_ready_pages=8/8`

write-back target replay 结果：

- `validation_ready=true`
- `state_trace_ready=true`
- `final_state_match=true`
- `invariant_coverage_ready=true`
- `missing_page_identity_events=0`
- `l1_resident_pages=56/56`
- `l2_resident_pages=118/118`
- `dirty_pages=48/48`
- `backuped_pages=118/118`
- `evicted_pages=110/110`

### Prediction 验证：预测 trace 和真实目标 trace

当前状态：**write-back、write_through_selective、capacity、prefetch wait、
prefetch best_effort 和 prefetch timeout 跨配置 prediction 已通过；page64 同配置 replay
已通过，但 strict 跨配置 prediction 未通过。下一阶段优先修复 page64 strict，再推进更严格的逐
transition oracle。**

Prediction 验证用于证明 base trace 中的不变量加 target config，能够推导目标配置下真实状态变化。

流程：

```text
base真实运行
  -> base profiling/merged trace 中的不变量 facts
target cache config
  -> C++ state model
  -> predicted_target_cache_state_trace

target真实运行
  -> target profiling/merged trace + target state snapshot

predicted_target_cache_state_trace
  vs target state snapshot / target state trace
```

Prediction 验证必须使用两条真实 run：

- base run：提供不变量 facts；
- target run：只作为验证 oracle。

target run 的真实 load/write/prefetch 次数不能反向喂给模型。

建议先做这些 target config 变化：

- page size：以实际 base page size 为准；最近 replay 中 base page size 为 `128`，因此先选择一个更小 page size target，再选择一个边界 page size target；
- write policy：`write_through -> write_back`、`write_through -> write_through_selective`
  已通过；
- prefetch policy：`wait_complete`、`best_effort` 和 aggressive `timeout` 已通过；
- capacity：降低 L2 capacity 触发 clean eviction 和 dirty eviction 已通过。

已通过的真实 prediction：

```text
base:
data/profile_runs/sglang/20260606_185311_profiling_hicache_state_validation

target oracle:
data/profile_runs/sglang/20260606_191934_profiling_hicache_state_write_back_validation

prediction:
data/profile_runs/sglang/20260606_185311_profiling_hicache_state_validation/modeling/predict_write_back_state_strict_diff_v3_l1cap56
```

结果：

- `validation_ready=true`
- `state_trace_ready=true`
- `final_state_match=true`
- `invariant_coverage_ready=true`
- `missing_page_identity_events=0`
- `l1_resident_pages=56/56`
- `l2_resident_pages=118/118`
- `dirty_pages=48/48`
- `backuped_pages=118/118`
- `evicted_pages=110/110`
- `ignored_sets_diff_by_tier.locked_pages=0/8`，旧 run 缺少 lock/ref model input，作为 legacy ignore。

该结果要求 target experiment config 提供有效 capacity hint：`l1_capacity_pages=56`、
`l2_capacity_pages=129`。只从 `--max-total-tokens` / `--hicache-ratio` 粗算会导致
write-back dirty eviction 少建模或过建模。

已通过的真实 write_through_selective prediction：

```text
base:
data/profile_runs/sglang/20260607_053949_profiling_hicache_state_validation

target oracle:
data/profile_runs/sglang/20260607_063721_profiling_hicache_state_write_through_selective_validation

prediction:
data/profile_runs/sglang/20260607_053949_profiling_hicache_state_validation/modeling/predict_write_through_selective_state_strict_diff_v1
```

结果：

- `prediction.validation_ready=true`
- `prediction.final_state_match=true`
- `prediction.l1_resident_pages=56/56`
- `prediction.l2_resident_pages=121/121`
- `prediction.dirty_pages=0/0`
- `prediction.backuped_pages=121/121`
- `prediction.evicted_pages=65/65`
- `prediction.locked_pages=0/0`
- `prediction.prefetch_planned_pages=166/166`
- `prediction.prefetch_ready_pages=8/8`
- `prediction.prefetch_suppressed_pages=158/158`
- `prediction.timeline_delta_validation.match=true`

本次修复的建模要点：

- hit_count 按 `cache_scope(pid) + page` 维护，避免两个 TP 的同一 page 各命中一次时误判达到 selective 阈值；
- HiCache insert 缺少 request_id 时，使用同一 cache scope 下最近一次 lookup path 配对，避免 full-prefix insert 经 `prefix_len` 裁剪后丢失 hit_count；
- 显式 write policy target 仍消费 CPU_PINNED `remove_page`，因为它是 host/L2 resident state transition，不是 write movement。

已通过的真实 page-size prediction：

```text
base:
data/profile_runs/sglang/20260606_194950_profiling_hicache_state_validation

target oracle:
data/profile_runs/sglang/20260606_195957_profiling_hicache_state_page64_validation

prediction:
data/profile_runs/sglang/20260606_194950_profiling_hicache_state_validation/modeling/predict_page64_state_20260606_195957_rerun9_leaf_group_l2_clear
```

结果：

- `target_replay.validation_ready=true`
- `target_replay.final_state_match=true`
- `prediction.validation_ready=true`
- `prediction.final_state_match=true`
- `prediction.invariant_coverage_ready=true`
- `prediction.missing_page_identity_events=0`
- `prediction.skipped_non_invariant_events=432`
- `prediction.l1_resident_pages=111/111`
- `prediction.l2_resident_pages=250/250`
- `prediction.dirty_pages=0/0`
- `prediction.backuped_pages=250/250`
- `prediction.evicted_pages=139/139`

该结果说明 page-size prediction 所需输入事实已经具备，且当前 C++ HiCache state model
已经能用 target page identity、target radix prefix skeleton、leaf group eviction 和 L2 host
deletion 语义推导出真实 target final state。后续 page-size 相关工作应从 final-set 验证继续下钻到
operation-level transition 时间线，而不是继续扩大 profiling 字段。

已通过的真实 capacity prediction：

```text
base:
data/profile_runs/sglang/20260606_212731_profiling_hicache_state_capacity_base_validation

target oracle:
data/profile_runs/sglang/20260606_211101_profiling_hicache_state_capacity_validation

prediction:
data/profile_runs/sglang/20260606_212731_profiling_hicache_state_capacity_base_validation/modeling/predict_capacity_state_20260606_211101_rerun7_validation_visibility
```

结果：

- `prediction.validation_ready=true`
- `prediction.final_state_match=true`
- `prediction.l1_resident_pages=46/46`
- `prediction.l2_resident_pages=96/96`
- `prediction.dirty_pages=0/0`
- `prediction.backuped_pages=96/96`
- `prediction.evicted_pages=50/50`

已通过的真实 prefetch wait prediction：

```text
base:
data/profile_runs/sglang/20260607_031354_profiling_hicache_state_validation

target oracle:
data/profile_runs/sglang/20260607_032622_profiling_hicache_state_prefetch_wait_validation

prediction:
data/profile_runs/sglang/20260607_031354_profiling_hicache_state_validation/modeling/predict_prefetch_wait_state_20260607_032622
```

结果：

- `prediction.validation_ready=true`
- `prediction.final_state_match=true`
- `prediction.l1_resident_pages=56/56`
- `prediction.l2_resident_pages=121/121`
- `prediction.dirty_pages=0/0`
- `prediction.backuped_pages=121/121`
- `prediction.evicted_pages=65/65`
- `prediction.prefetch_planned_pages=166/166`
- `prediction.prefetch_ready_pages=8/8`
- `prediction.unchecked_model_state_keys=["l3_resident_pages"]`

该结果修正了旧假设：`wait_complete` 不能把所有 scheduled pages 直接标记为 ready；
schedule 只表示 planned，ready 必须由 `check_prefetch_progress` 的 operation progress
证据或已完成的 `l3_to_l2_transfer_end` 驱动。

已通过的真实 prefetch best_effort prediction：

```text
base:
data/profile_runs/sglang/20260607_031354_profiling_hicache_state_validation

target oracle:
data/profile_runs/sglang/20260607_035602_profiling_hicache_state_prefetch_best_effort_validation

prediction:
data/profile_runs/sglang/20260607_031354_profiling_hicache_state_validation/modeling/predict_prefetch_best_effort_state_derived_target_experiment_v3
```

结果：

- `prediction.validation_ready=true`
- `prediction.final_state_match=true`
- `prediction.l1_resident_pages=56/56`
- `prediction.l2_resident_pages=121/121`
- `prediction.dirty_pages=0/0`
- `prediction.backuped_pages=121/121`
- `prediction.evicted_pages=65/65`
- `prediction.prefetch_planned_pages=166/166`
- `prediction.prefetch_ready_pages=8/8`
- `prediction.prefetch_suppressed_pages=158/158`
- `prediction.ignored_sets_diff_by_tier.locked_pages=0/8`，旧 run 缺少 lock/ref model input，作为 legacy ignore。

已通过的真实 aggressive timeout prediction：

```text
base:
data/profile_runs/sglang/20260607_031354_profiling_hicache_state_validation

target oracle:
data/profile_runs/sglang/20260607_041431_profiling_hicache_state_prefetch_timeout_aggressive_validation

prediction:
data/profile_runs/sglang/20260607_031354_profiling_hicache_state_validation/modeling/predict_prefetch_timeout_aggressive_state_20260607_041431
```

结果：

- `prediction.validation_ready=true`
- `prediction.final_state_match=true`
- `prediction.l1_resident_pages=56/56`
- `prediction.l2_resident_pages=121/121`
- `prediction.dirty_pages=0/0`
- `prediction.backuped_pages=121/121`
- `prediction.evicted_pages=65/65`
- `prediction.prefetch_planned_pages=166/166`
- `prediction.prefetch_ready_pages=8/8`
- `prediction.prefetch_suppressed_pages=158/158`
- `prediction.ignored_sets_diff_by_tier.locked_pages=0/8`，旧 run 缺少 lock/ref model input，作为 legacy ignore。

本轮修复了两个 prefetch state 关键问题：

- Python probe 的 `start/end` 事件在 merged trace 中同 timestamp 时可能出现 `end` 先于
  `start`。HiCache state model 现在会在同 timestamp 内按 `start -> end` 的调用逻辑顺序消费
  HiCache facts，避免先看到空 progress end 后误判 suppressed。
- `best_effort` / aggressive `timeout` prediction 不能把 base trace 中没有
  `operation_hash_pages` 的 `has_ongoing_prefetch=true` 当作 late page 证据；没有 operation
  page identity 时只允许在 terminal empty progress 上标记 suppressed。late 必须依赖
  operation progress evidence。
- `l3_to_l2_transfer_end` 是完成事实，timeout policy 不能因为 target timeout window 激进就
  否掉已经完成的 8 页 ready evidence。

验收：

- `invariant_coverage_ready=true`
- `missing_invariant_facts=[]`
- `non_invariant_fact_usage=[]`
- final L1/L2/dirty/backuped/evicted sets 对齐；
- 每个 request 的 predicted transition 与 target state trace 可对齐；
- 不一致时 `first_mismatch` 能定位 request、operation、page、transition kind 和 reason。

### 端到端验证还是逐 trace 验证

本阶段主验收是逐 trace 验证，不是端到端验证。

逐 trace 验证包括：

- request 顺序对齐；
- operation role 对齐；
- page set 对齐；
- transition kind 对齐；
- before/after state digest 对齐；
- missing facts 和 policy reason 对齐。

端到端验证只作为 sanity check：

- state-only 阶段 `dag_mutations=0`，因此 E2E 不应作为 cache state 正确性的判断依据；
- 如果 E2E 变了，说明错误发生在非 state-only 边界；
- 真正 E2E acceptance 要等 DAG patch 阶段，用 state-to-DAG 输出验证。

## State Validation Diff

当前状态：**基础 diff 已完成；final set 级别已能闭合真实 replay，request-level coverage
和 transition coverage 已补齐，request/transition 时间线逐步对齐仍需继续增强。**

`scripts/internal/model_runner.py` 已增强 `cache_state` validation。

当启用：

```bash
--emit-validation --emit-module-summary
```

且 `validation.hicache_state.enabled=true` 时，才执行：

- 从独立 state trace 或 merged trace 中标记为非执行输入的 SGLang `state_snapshot` 提取 oracle；
- 从 C++ `model_summary.json` 中读取 model final state 和 transition trace；
- 读取 predicted target state trace；
- 在 `validation.json` 中生成 `hicache_state` 字段；
- 输出 profiling invariant coverage。

未开启 validation 时，不读取 state snapshot、不生成 diff、不影响 `prediction.json`。

`validation.json.hicache_state` 至少包含：

- `state_trace_ready`
- `state_trace_events`
- `model_transition_events`
- `final_state_match`
- `sets_diff_by_tier`
- `first_mismatch`
- `request_transition_coverage`
- `transition_coverage`
- `missing_page_identity_events`
- `skipped_non_invariant_events`
- `unmatched_state_trace_events`
- `invariant_coverage_ready`
- `missing_invariant_facts`
- `non_invariant_fact_usage`

diff 必须给出可修复信息：

- 哪个 role 出错；
- 哪个 request / operation / page 出错；
- model 多了什么、少了什么；
- 对应 SGLang snapshot 的 event name、target id、timestamp。

当前 diff 已能给出 final set 层面的 missing / extra，并能在 synthetic mismatch 和真实
write-back prediction 中定位到候选 transition。`request_transition_coverage` 已能统计 predicted
transition 和 oracle snapshot 中带 request id 的请求集合，暴露缺失请求。
`transition_coverage` 已能统计 predicted transition kind、operation kind、source event、
predicted/oracle page 覆盖，用于判断某个 page 是否存在可解释 transition。后续仍需要把
`first_mismatch` 从 final set 继续下钻到严格 operation / transition 时间线，用于更复杂的
page size / capacity / prefetch policy mismatch。

## Toolchain 与约束

当前状态：**已完成。**

已在当前环境安装 `clang-format`。新环境可使用：

```bash
dnf install -y clang-tools-extra || dnf install -y clang
```

如果安装后只有版本化命令，例如 `clang-format-17`，创建 `/usr/local/bin/clang-format`
软链指向最新版本。

验证命令：

```bash
clang-format --version
git ls-files '*.c' '*.cc' '*.cpp' '*.h' '*.hpp' | xargs clang-format --dry-run --Werror
```

`docs/project_constraints.md` 已更新以下约束：

- 仓库根目录 `.clang-format` 是唯一 C/C++ 格式规范。
- C/C++ 改动提交前必须能运行 `clang-format --dry-run --Werror`。
- 复杂 C++ 子模块必须采用面向对象结构。
- 不允许把状态机、fact parser、summary、policy 和 DAG mutation 全部堆在单个匿名 namespace。
- HiCache state validation 默认通过 Python probe 采集 SGLang state snapshot，不直接修改
  third_party SGLang，除非后续证明 probe 无法获得必要事实。
- 验证逻辑必须通过编译选项、命令行参数或配置显式开启；默认业务代码路径不能采集完整 state trace，
  也不能输出 validation-only 字段。

## 完整 HiCache Workload

当前状态：**已用 phased workload 跑过 base、write-back target 和 page64 target；base / page64
同配置 state replay 已通过，write-back cross-config prediction 已通过；page64 strict
cross-config prediction 未通过。**

现有 workload 脚本：

```text
scripts/bench/hicache_phased_workload.py
```

workload 脚本支持的阶段：

| 阶段 | 目的 | 当前 expected mechanisms |
| --- | --- | --- |
| `seed_A` | 建立 A 前缀 cache resident，触发首次 insert / write。 | `lookup`、`insert`、`write_backup`、`write_storage` |
| `reuse_A` | 同前缀复用，验证 lookup / hit 路径。 | `lookup` |
| `backup_wait_A` | 给 write-through selective / storage backup 留出完成窗口。 | `lookup`、`write_backup`、`write_storage` |
| `pressure_B` | 写入大量 B 前缀，对容量和 eviction 施压。 | `lookup`、`insert`、`evict` |
| `reuse_A_after_pressure` | 在 pressure 后复用 A，验证 load_back / resident 恢复。 | `lookup`、`load_back` |
| `prefetch_seed_C` | 建立 C 前缀，为 storage prefetch 提供对象。 | `lookup`、`insert`、`write_backup`、`write_storage` |
| `prefetch_reuse_C` | 复用 C，触发 prefetch decision / schedule / query / transfer。 | `lookup`、`prefetch_decision`、`prefetch_schedule`、`prefetch_query`、`prefetch_transfer` |
| `dirty_eviction` | 目标配置下触发 dirty page eviction 和 write-back。 | `lookup`、`insert`、`evict`、`write_backup`、`write_storage` |

write-back target 下的 expected mechanisms 会按写策略调整：

- `seed_A` / `prefetch_seed_C` 只要求 `lookup`、`insert`，因为普通 insert 后不会立即写 L2/L3；
- `backup_wait_A` 只要求 `lookup`；
- `prefetch_reuse_C` 不强制要求 `prefetch_transfer`，因为 C 阶段 seed 不一定已落 L3；
- dirty writeback 由 eviction 路径验证，而不是由普通 insert 阶段验证。

已完成 replay workload 结果：

- 使用 `configs/experiments/hicache_state/profiling_hicache_state_validation.json`。
- `scripts/bench/hicache_phased_workload.py` 已加入 distinct pressure prefix 和 phase wait，保证
  pressure、load_back、prefetch、write 完成窗口更稳定。
- 最新真实 base run `20260607_031354` 中 observed mechanisms 覆盖 lookup、insert、load_back、evict、
  prefetch decision/schedule/query/transfer、write backup/storage。
- 最新真实 wait_complete target run `20260607_032622` 中 observed mechanisms 覆盖 lookup、insert、
  load_back、evict、prefetch decision/progress/schedule/query/transfer、write backup/storage。
- 真实 write-back target run `20260606_191934` 中 observed mechanisms 覆盖 lookup、insert、
  load_back、evict、prefetch decision/schedule/query、write backup/storage；不要求
  `prefetch_transfer`。
- 真实 page64 target run `20260606_195957` 中 observed mechanisms 覆盖 lookup、insert、
  load_back、evict、prefetch decision/schedule/query/transfer、write backup/storage。
- profile quality `quality_ready=true`，`missing_cache_mechanisms=[]`。
- replay 验收仍以 state 为中心，不以 E2E 为中心；`dag_mutations=0` 是正确结果。

仍需补充的 workload 能力：

- capacity target 已能稳定触发 clean eviction；dirty eviction 已在 write-back target 中验证。
  后续可追加“低容量 + write-back”组合，用一个真实 target 同时覆盖 dirty eviction 和 capacity。
- prefetch wait/best_effort/timeout target 已能验证 planned / ready / late / suppressed；
  write_through_selective 的 hit_count threshold 已通过真实 target。后续 workload 重点转向
  低容量 write-back dirty eviction 组合和逐 transition oracle。

下一轮 prediction workload 要求：

- base run 和 target run 必须使用相同 workload 序列，避免请求内容差异污染 state diff。
- target run 可以通过 cache config 改变机制行为，但不能改变 workload 请求顺序。
- `dirty_eviction` 只在 write-back / capacity target 中作为必过项开启。
- 跨配置 target 至少覆盖：
  - page size：`128 -> 64` 已通过；后续可追加一个更大或边界 page size 作为压力补充；
  - write policy：`write_through -> write_back`、`write_through -> write_through_selective`
    已通过；
  - prefetch policy：`timeout -> wait_complete`、`timeout -> best_effort` 和 aggressive
    `timeout` late/suppressed 已通过；
  - capacity：降低 L2 capacity，触发 clean eviction；在 write-back target 中触发 dirty eviction。

## 实验配置

当前状态：**state replay、write-back target、write_through_selective target、capacity target、
prefetch wait、prefetch best_effort 和 aggressive timeout target 已完成真实跨配置验证；
page64 target 同配置 replay 已完成，但 strict 跨配置验证未通过。**

已新增专用 profiling 配置：

```text
configs/experiments/hicache_state/profiling_hicache_state_validation.json
configs/experiments/hicache_state/profiling_hicache_state_write_back_validation.json
configs/experiments/hicache_state/profiling_hicache_state_write_through_selective_validation.json
configs/experiments/hicache_state/profiling_hicache_state_page64_validation.json
configs/experiments/hicache_state/profiling_hicache_state_capacity_validation.json
configs/experiments/hicache_state/profiling_hicache_state_prefetch_wait_validation.json
configs/experiments/hicache_state/profiling_hicache_state_prefetch_best_effort_validation.json
configs/experiments/hicache_state/profiling_hicache_state_prefetch_timeout_aggressive_validation.json
```

该配置基于当前 `profiling_minimal_sglang_hicache.json`，用于 state-only 快速验证，只启用：

```json
"channels": ["python_probe", "ld_preload"]
```

同时：

- `profiling.torch.enabled = false`
- `profiling.python_probe.state_trace.enabled = true`
- 保留 `python_probe.targets`
- 保留 `ld_preload`
- 保留 server 命令中的 `--disable-cuda-graph`
- 保留 `--hicache-ratio 2.0`

`profiling_hicache_state_capacity_validation.json` 使用同 page size 和同 `--hicache-ratio 2.0`，
只把 `--max-total-tokens` 降到 `6144`，并把 pressure 请求数提高到 `32`。capacity pressure
来自目标容量和 workload，不通过随意修改 hicache ratio 达成。

`profiling_hicache_state_prefetch_wait_validation.json` 只把
`--hicache-storage-prefetch-policy` 改为 `wait_complete`，用于和 base timeout run 做 prefetch
policy state prediction。该配置已验证 `prefetch_planned_pages=166/166` 和
`prefetch_ready_pages=8/8`、`prefetch_suppressed_pages=158/158`；不能再假设
wait_complete 会让所有 scheduled pages ready。

`profiling_hicache_state_prefetch_best_effort_validation.json` 只把
`--hicache-storage-prefetch-policy` 改为 `best_effort`。该配置已验证
`prefetch_planned_pages=166/166`、`prefetch_ready_pages=8/8` 和
`prefetch_suppressed_pages=158/158`。

`profiling_hicache_state_prefetch_timeout_aggressive_validation.json` 使用
`--hicache-storage-prefetch-policy timeout`，并把 storage backend extra config 中的
`prefetch_timeout_base/per_ki_token/max` 全部设为 `0.0`。该配置用于稳定触发 timeout
终止路径，已验证 ready/suppressed prediction 与真实 target oracle 对齐。

如果同一轮实验要验证 faithful replay 或为 cache patch 收集 DAG anchor，则不能使用这个
state-only 配置，必须启用 torch 并消费完整真实执行 trace。

真实 profiling 必须通过外层容器入口运行：

```bash
scripts/profile.sh configs/experiments/hicache_state/profiling_hicache_state_validation.json
scripts/profile.sh configs/experiments/hicache_state/profiling_hicache_state_write_back_validation.json
scripts/profile.sh configs/experiments/hicache_state/profiling_hicache_state_page64_validation.json
scripts/profile.sh configs/experiments/hicache_state/profiling_hicache_state_prefetch_best_effort_validation.json
scripts/profile.sh configs/experiments/hicache_state/profiling_hicache_state_prefetch_timeout_aggressive_validation.json
scripts/profile.sh configs/experiments/hicache_state/profiling_hicache_state_capacity_validation.json
scripts/profile.sh configs/experiments/hicache_state/profiling_hicache_state_prefetch_wait_validation.json
```

已新增专用 modeling 配置：

```text
configs/modeling/hicache_state/modeling_hicache_state_validation.json
```

已新增跨配置 prediction 配置：

```text
configs/modeling/hicache_state/modeling_hicache_state_prediction_write_back.json
configs/modeling/hicache_state/modeling_hicache_state_prediction_write_through_selective.json
configs/modeling/hicache_state/modeling_hicache_state_prediction_page64.json
configs/modeling/hicache_state/modeling_hicache_state_prediction_capacity.json
configs/modeling/hicache_state/modeling_hicache_state_prediction_prefetch_wait.json
```

`modeling_hicache_state_prediction_write_back.json` 当前用于 write-back target state prediction。
真实运行时通过 CLI 传入 target run 的 oracle trace：

```bash
python3 scripts/internal/model_runner.py \
  --config configs/modeling/hicache_state/modeling_hicache_state_prediction_write_back.json \
  --profile-manifest <base_run_dir>/profile_manifest.json \
  --hicache-oracle-trace <target_run_dir>/modeling/cache_state_replay/merged_trace/merged_trace_00.json \
  --output-dir <base_run_dir>/modeling/predict_write_back_state \
  --mode cache_state \
  --emit-module-summary \
  --emit-validation
```

跨配置 prediction 配置必须满足：

- model 输入使用 base run 的 `profile_manifest.json`；
- `validation.hicache_state.oracle_trace_paths` 指向 target run 的 state snapshot trace 或 merged trace；
- C++ HiCache config 使用 target cache config；
- 输出目录命名为 `<base_run_dir>/modeling/predict_<target_name>_state`。

本轮已通过命令：

```bash
python3 scripts/internal/model_runner.py \
  --config configs/modeling/hicache_state/modeling_hicache_state_prediction_write_back.json \
  --profile-manifest data/profile_runs/sglang/20260606_185311_profiling_hicache_state_validation/profile_manifest.json \
  --hicache-oracle-trace data/profile_runs/sglang/20260606_191934_profiling_hicache_state_write_back_validation/modeling/cache_state_replay/merged_trace/merged_trace_00.json \
  --output-dir data/profile_runs/sglang/20260606_185311_profiling_hicache_state_validation/modeling/predict_write_back_state_strict_diff_v3_l1cap56 \
  --mode cache_state \
  --emit-module-summary \
  --emit-validation
```

新增 page64 prediction 入口：

```bash
python3 scripts/internal/model_runner.py \
  --config configs/modeling/hicache_state/modeling_hicache_state_prediction_page64.json \
  --profile-manifest <base_run_dir>/profile_manifest.json \
  --hicache-oracle-trace <page64_target_run_dir>/modeling/cache_state_replay/merged_trace/merged_trace_00.json \
  --output-dir <base_run_dir>/modeling/predict_page64_state \
  --mode cache_state \
  --emit-module-summary \
  --emit-validation
```

`modeling_hicache_state_prediction_page64.json` 通过
`input.target_experiment_config=configs/experiments/hicache_state/profiling_hicache_state_page64_validation.json`
派生 target 配置：

- `page_size=64`
- `write_policy=write_through`
- `l1_capacity_pages=128`
- `l2_capacity_pages=256`
- validation-only 忽略 `locked_pages` 和 prefetch debug 集合

容量不再由 runner 根据 `--max-total-tokens` / `--hicache-ratio` 粗算，而是来自目标实验
配置中的 `modeling.hicache` 显式字段。page64 旧 run 缺少 lock/ref model input，且本实验
目标是 page-size/radix/capacity state，因此 `locked_pages` 和 prefetch debug 集合只进入
`ignored_sets_diff_by_tier` 解释输出，不作为硬门槛。最新 v4 结果为
`validation_ready=true`，L1 `111/111`、L2 `250/250`、dirty `0/0`、backuped `250/250`、
evicted `139/139`。

新增 capacity prediction 入口：

```bash
python3 scripts/internal/model_runner.py \
  --config configs/modeling/hicache_state/modeling_hicache_state_prediction_capacity.json \
  --profile-manifest <base_run_dir>/profile_manifest.json \
  --hicache-oracle-trace <capacity_target_run_dir>/modeling/cache_state_replay/merged_trace/merged_trace_00.json \
  --output-dir <base_run_dir>/modeling/predict_capacity_state \
  --mode cache_state \
  --emit-module-summary \
  --emit-validation
```

`modeling_hicache_state_prediction_capacity.json` 通过
`input.target_experiment_config=configs/experiments/hicache_state/profiling_hicache_state_capacity_validation.json`
派生 target 配置：

- `page_size=128`
- `l1_capacity_pages=46`
- `l2_capacity_pages=96`
- `write_policy=write_through`
- `prefetch_policy=timeout`
- validation-only 忽略 `locked_pages`

这里的 `l1_capacity_pages=46` 是本次 target run 的有效 device page budget；不能直接用
`6144 / 128 = 48` 粗算。最新 v4 结果为 `validation_ready=true`，L1 `46/46`、L2
`96/96`、dirty `0/0`、backuped `96/96`、evicted `50/50`、prefetch planned `326/326`、
ready `8/8`；旧 run 缺少 lock/ref model input，`locked_pages=0/8` 只作为 legacy ignore。

新增 prefetch wait prediction 入口：

```bash
python3 scripts/internal/model_runner.py \
  --config configs/modeling/hicache_state/modeling_hicache_state_prediction_prefetch_wait.json \
  --profile-manifest <base_run_dir>/profile_manifest.json \
  --hicache-oracle-trace <prefetch_wait_target_run_dir>/modeling/cache_state_replay/merged_trace/merged_trace_00.json \
  --output-dir <base_run_dir>/modeling/predict_prefetch_wait_state \
  --mode cache_state \
  --emit-module-summary \
  --emit-validation
```

`modeling_hicache_state_prediction_prefetch_wait.json` 固定：

- `page_size=128`
- `write_policy=observed`
- `prefetch_policy=wait_complete`

当前已通过的真实命令：

```bash
python3 scripts/internal/model_runner.py \
  --config configs/modeling/hicache_state/modeling_hicache_state_prediction_prefetch_wait.json \
  --profile-manifest data/profile_runs/sglang/20260607_031354_profiling_hicache_state_validation/profile_manifest.json \
  --hicache-oracle-trace data/profile_runs/sglang/20260607_032622_profiling_hicache_state_prefetch_wait_validation/modeling/cache_state_replay/merged_trace/merged_trace_00.json \
  --output-dir data/profile_runs/sglang/20260607_031354_profiling_hicache_state_validation/modeling/predict_prefetch_wait_state_20260607_032622 \
  --mode cache_state \
  --emit-module-summary \
  --emit-validation
```

该配置的 `wait_complete` 语义是：schedule 只生成 planned pages；只有 progress
operation 证明完成的 pages 才进入 ready。不能把所有 scheduled pages 自动写入
`prefetch_ready_pages`。

## 测试计划

### 已完成基础验证

这些命令在当前实现收尾时已经通过，后续修改相关代码后仍需重跑：

- `cmake --build build --target trace_graph -j 8`
- `git ls-files '*.c' '*.cc' '*.cpp' '*.h' '*.hpp' | xargs clang-format --dry-run --Werror`
- `python3 -m py_compile scripts/internal/model_runner.py scripts/internal/profile_runner.py scripts/internal/profile_quality.py src/profiling/python_probe/trace_sim_probe/probes/sglang_hicache_callable.py tests/run_hicache_state_fixtures.py tests/run_modeling_smoke_fixtures.py tests/run_profiling_fixtures.py`
- `python3 tests/run_tracegraph_fixtures.py`
- `python3 tests/run_modeling_smoke_fixtures.py`
- `python3 tests/run_hicache_state_fixtures.py`
- `python3 tests/run_profiling_fixtures.py`
- `python3 tests/run_native_hook_fixtures.py`
- `scripts/profile.sh configs/experiments/hicache_state/profiling_hicache_state_validation.json --dry-run`
- `scripts/profile.sh configs/experiments/hicache_state/profiling_hicache_state_write_back_validation.json --dry-run`
- `scripts/profile.sh configs/experiments/hicache_state/profiling_hicache_state_page64_validation.json --dry-run`
- `scripts/profile.sh configs/experiments/hicache_state/profiling_hicache_state_capacity_validation.json --dry-run`
- `scripts/profile.sh configs/experiments/hicache_state/profiling_hicache_state_prefetch_wait_validation.json --dry-run`

已完成的 fixture 覆盖：

- fake `HiRadixCache` tree，验证 `hicache_state:self` 生成 L1/L2/dirty/backuped snapshot。
- synthetic replay trace，验证同配置 replay 时 `final_state_match=true`。
- synthetic prediction trace，验证 source trace + 独立 oracle trace 可以走同一套 state diff。
- synthetic mismatch，验证 `first_mismatch` 能定位候选 transition。
- synthetic write-back capacity，验证 dirty -> writeback -> evict transition。
- snapshot dirty 派生，验证 `has_device_value && !backuped` 可作为 write-back dirty oracle。
- target page size 变化缺少 target invariant 时会显式报 `missing_invariant_facts`。
- target page size 变化提供 `target_page_identity` 时可使用 target page set 更新 state。
- target radix prefix skeleton，验证 page size what-if 下 insert 使用 target lookup prefix 截取 suffix。
- target leaf group eviction，验证 page size what-if 下 eviction 按 insert suffix group 整体释放。
- L2 host deletion，验证 host entry 被删除后会清理最终 `evicted_pages`。
- capacity target skip，验证容量 what-if 不消费 base run 中观测到的 `remove_page`。
- same-page capacity exact eviction，验证同 page size 容量 target 按 page 精确驱逐，且
  pre-allocation 只释放超容量差值。
- prefetch wait_complete，验证 scheduled prefetch pages 会进入 ready L2，并跳过 base transfer completion。
- L3 evidence-only 场景，验证 validation 不要求 SGLang 提供全量 L3 final set。
- request-level coverage，验证 predicted transition 与 oracle snapshot 的 request id 覆盖可输出。
- transition coverage，验证 predicted transition kind、operation kind、source event 和 page 覆盖可输出。
- `page_hashes:` 支持裸字面量 page size，验证 base trace 可额外采 target page identity。
- page size 变化下缺少 target page identity 的 base movement 会被跳过并计数。
- 多进程 state snapshot validation 使用 per-process final snapshot union，而不是只取全局最后一个 snapshot。
- validation-only `state_snapshot` 不进入 C++ 性能 DAG。

仍需补强的 fixture：

- operation-level state trace 严格对齐，验证不只 final set / request coverage / transition coverage 对齐。
- prefetch ready / late / suppressed oracle 对齐。

### 已完成完整 workload replay

```bash
scripts/profile.sh configs/experiments/hicache_state/profiling_hicache_state_validation.json

python3 scripts/internal/profile_quality.py \
  --manifest data/profile_runs/sglang/20260606_185311_profiling_hicache_state_validation/profile_manifest.json \
  --output data/profile_runs/sglang/20260606_185311_profiling_hicache_state_validation/profile_quality.json

python3 scripts/internal/model_runner.py \
  --config configs/modeling/hicache_state/modeling_hicache_state_validation.json \
  --profile-manifest data/profile_runs/sglang/20260606_185311_profiling_hicache_state_validation/profile_manifest.json \
  --output-dir data/profile_runs/sglang/20260606_185311_profiling_hicache_state_validation/modeling/cache_state_replay \
  --mode cache_state \
  --emit-module-summary \
  --emit-validation
```

完整 workload replay 验收结果：

- manifest `status=completed`
- workload report 中所有 request `status=ok`
- profile quality `quality_ready=true`
- `missing_cache_mechanisms=[]`
- `stateful_page_identity_ready=true`
- Python probe trace 存在且包含 `state_snapshot`
- `hicache.status=state_model`
- `hicache.dag_mutations=0`
- `validation.hicache_state.state_trace_ready=true`
- `validation.hicache_state.final_state_match=true`
- `validation.hicache_state.invariant_coverage_ready=true`
- `validation.hicache_state.missing_page_identity_events=0`

本轮真实结果已经满足以上验收。

### 已完成 write-back 跨配置 prediction

```bash
scripts/profile.sh configs/experiments/hicache_state/profiling_hicache_state_validation.json
scripts/profile.sh configs/experiments/hicache_state/profiling_hicache_state_write_back_validation.json

python3 scripts/internal/profile_quality.py \
  --manifest data/profile_runs/sglang/20260606_185311_profiling_hicache_state_validation/profile_manifest.json \
  --output data/profile_runs/sglang/20260606_185311_profiling_hicache_state_validation/profile_quality.json

python3 scripts/internal/profile_quality.py \
  --manifest data/profile_runs/sglang/20260606_191934_profiling_hicache_state_write_back_validation/profile_manifest.json \
  --output data/profile_runs/sglang/20260606_191934_profiling_hicache_state_write_back_validation/profile_quality.json

python3 scripts/internal/model_runner.py \
  --config configs/modeling/hicache_state/modeling_hicache_state_prediction_write_back.json \
  --profile-manifest data/profile_runs/sglang/20260606_185311_profiling_hicache_state_validation/profile_manifest.json \
  --hicache-oracle-trace data/profile_runs/sglang/20260606_191934_profiling_hicache_state_write_back_validation/modeling/cache_state_replay/merged_trace/merged_trace_00.json \
  --output-dir data/profile_runs/sglang/20260606_185311_profiling_hicache_state_validation/modeling/predict_write_back_state_20260606_191934 \
  --mode cache_state \
  --emit-module-summary \
  --emit-validation
```

验收结果：

- base 和 target 的 workload report 均全部成功。
- base 和 target 的 profile quality 均 `quality_ready=true`。
- prediction 使用 base manifest 作为模型输入，target actual trace 只作为 oracle。
- `validation.hicache_state.invariant_coverage_ready=true`
- `validation.hicache_state.final_state_match=true`
- final L1/L2/dirty/backuped/evicted sets 全部对齐。

### 下一轮跨配置 prediction

```bash
# 1. 跑 base 配置，采集 base profiling trace。base 使用完整 workload。
scripts/profile.sh configs/experiments/hicache_state/profiling_hicache_state_validation.json

# 2. 跑 target 配置，采集 target oracle state trace。target 必须使用相同 workload 请求序列。
scripts/profile.sh <target_cache_config>

# 3. 审计 base 和 target 的 profiling 质量。
python3 scripts/internal/profile_quality.py --manifest <base_run_dir>/profile_manifest.json
python3 scripts/internal/profile_quality.py --manifest <target_run_dir>/profile_manifest.json

# 4. 用 base profiling/merged trace 中的不变量 facts + target cache config 预测 target state trace。
python3 scripts/internal/model_runner.py \
  --config <modeling_config_with_target_cache_config> \
  --profile-manifest <base_run_dir>/profile_manifest.json \
  --output-dir <base_run_dir>/modeling/predict_<target_name>_state \
  --mode cache_state \
  --emit-module-summary \
  --emit-validation
```

当前立即执行项是复核 strict page64 prediction。优先复用最新已经完成的 base / target profiling
run，避免在模型语义未闭环前反复重跑 SGLang：

```bash
cmake --build build --target trace_graph -j 8
python3 tests/run_hicache_state_fixtures.py
python3 tests/run_profiling_fixtures.py
python3 tests/run_modeling_smoke_fixtures.py

python3 scripts/internal/model_runner.py \
  --config configs/modeling/hicache_state/modeling_hicache_state_prediction_page64.json \
  --profile-manifest data/profile_runs/sglang/20260607_144832_profiling_hicache_state_validation/profile_manifest.json \
  --output-dir data/profile_runs/sglang/20260607_144832_profiling_hicache_state_validation/modeling/predict_page64_state_strict_after_prefix_recheck \
  --mode cache_state \
  --emit-module-summary \
  --emit-validation \
  --hicache-oracle-trace data/profile_runs/sglang/20260607_133143_profiling_hicache_state_page64_validation/modeling/cache_state_replay_pagehash_concat_v1/merged_trace/merged_trace_00.json
```

若该重验仍失败，修复顺序必须是：

- 先检查 `prefetch_schedule` 的 planned page set 是否来自 target page size 下的 suffix pages；
- 再检查 `check_prefetch_progress` 是否只有在包含明确 ready payload 时才提供 ready credit；
- 再检查 `l3_to_l2_transfer_end` 是否只给目标 page identity 中确实完成的 page 加 ready credit；
- 最后检查 eviction 和 backuped 差异是否是前面 prefetch ready/planned 差异的级联结果。

跨配置 prediction 验收：

- base 和 target 的 workload report 均全部成功。
- base 和 target 的 profile quality 均 `quality_ready=true`。
- prediction 使用 base manifest 作为模型输入，不能读取 target actual trace 作为输入。
- target actual trace 只通过 `validation.hicache_state.oracle_trace_paths` 作为 oracle。
- `validation.hicache_state.invariant_coverage_ready=true`
- `validation.hicache_state.non_invariant_fact_usage=[]`
- final L1/L2/dirty/backuped/evicted sets 对齐。
- strict page64 还要求 `prefetch_planned_pages`、`prefetch_ready_pages`、
  `prefetch_suppressed_pages` 对齐，且 `timeline_delta_validation.match=true`。
- 每个不一致必须能通过 `first_mismatch` 定位 request、operation、page、transition kind 和 reason。
- prediction 通过前不进入 state-to-DAG patch。

## 失败处理

如果真实实验失败，先保留并记录：

- `profile_manifest.json`
- `serve.log`
- workload report
- Python probe trace
- LD_PRELOAD trace
- modeling `run_summary.json`
- modeling `model_summary.json`
- modeling `validation.json`

不要立即修改模型逻辑。先判断失败属于：

- profiling 运行失败；
- state trace 缺字段；
- 不变量事实缺失；
- C++ fact parser 缺字段；
- C++ state transition 逻辑错误；
- SGLang snapshot 与 event 顺序对齐问题；
- target config 与真实 target run 配置不一致。
