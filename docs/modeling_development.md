# Modeling 开发文档

维护方式：这是 modeling 主线文档。更新时直接删改本文件内容，不写流水账。

## 目标

Modeling 基于 profiling 事实构建 C++ TraceGraph，并在目标配置下通过子模块维护状态或修改 DAG。

默认流程：

```text
profile manifest / explicit trace
  -> scripts/trace/trace_merger.py
  -> C++ TraceGraph
  -> optional SimulationModule
  -> topological simulation
  -> prediction.json / optional summary / optional validation
```

默认主输出：

```json
{
  "predicted_e2e_ns": 0
}
```

`predicted_e2e_ns` 来自 DAG 拓扑仿真。对于当前 HiCache state-only backend，它不是 cache state 正确性的验收指标。

## 运行入口

smoke：

```bash
python3 scripts/internal/model_runner.py \
  --config configs/modeling/smoke/modeling_smoke_hicache.json
```

faithful replay：

```bash
python3 scripts/internal/model_runner.py \
  --config configs/modeling/hicache/modeling_hicache_from_manifest.json \
  --profile-manifest <run_dir>/profile_manifest.json \
  --output-dir <run_dir>/modeling/faithful_replay \
  --mode faithful_replay \
  --emit-validation \
  --emit-module-summary
```

HiCache S1A self-config prediction：

```bash
python3 scripts/internal/model_runner.py \
  --config configs/modeling/hicache_state/modeling_hicache_state_mainline_one_prediction_s1a.json \
  --profile-manifest <run_dir>/profile_manifest.json \
  --output-dir <run_dir>/modeling/token_backend_s1a \
  --mode cache_state \
  --emit-module-summary \
  --emit-validation
```

常用覆盖项：

| CLI | 作用 |
| --- | --- |
| `--profile-manifest` | 覆盖 config 中的 manifest。 |
| `--output-dir` | 覆盖输出目录。 |
| `--mode` | 覆盖 modeling mode。 |
| `--emit-dag-chrome-trace` | 输出 DAG Chrome trace。 |
| `--emit-module-summary` | 输出 `model_summary.json`。 |
| `--emit-validation` | 输出 `validation.json`。 |

## Modeling Mode

| mode | 语义 |
| --- | --- |
| `faithful_replay` | 不加载子模块，不 patch DAG；消费完整真实执行 trace，验证 base DAG。 |
| `cache_state` | 加载状态子模块，维护内部状态，不修改 DAG。 |
| `cache_patch` | 子模块维护状态并通过 mutation API 修改 DAG；HiCache 尚未实现。 |

`replay` 只允许指 `mode=faithful_replay`。启用 HiCacheModule 的场景必须称为 `self-config prediction` 或
`cross-config prediction`。

## Trace Merger

`scripts/trace/trace_merger.py` 在 manifest 模式下合并：

- torch profiler trace；
- LD_PRELOAD trace；
- Python probe sidecar。

Trace merger 不根据 modeling mode 删除真实执行事件。`faithful_replay`、`cache_state` 和 `cache_patch`
应看到同一份 merged trace；差异只在是否加载子模块、是否产生 DAG mutation。

非执行类事件需要通过字段路由隔离：

| 字段 | 作用 |
| --- | --- |
| `model_input` | 是否进入 modeling 输入集合。 |
| `dag_input` | 是否作为默认性能 DAG 节点。 |
| `state_model_input` | 是否允许状态子模块消费。 |
| `fact_class` | 子模块事实分类。 |

## TraceGraph 结构

C++ 后端位于 `src/modeling/trace_graph`：

| 目录 | 内容 |
| --- | --- |
| `include/trace_graph/core` / `src/core` | `TraceEvent`、`DagGraph` 等基础结构。 |
| `include/trace_graph/frontend` / `src/frontend` | model config 和 trace normalize。 |
| `include/trace_graph/io` / `src/io` | Chrome trace 读取输出。 |
| `include/trace_graph/simulation` / `src/simulation` | 拓扑仿真。 |
| `include/trace_graph/modules` / `src/modules` | SimulationModule 子模块。 |
| `modules/hicache` | HiCache fact parser、radix tree、state model、summary。 |

构建目标：

```bash
cmake --build build --target trace_graph -j2
```

## SimulationModule

所有 what-if 都必须规约为 C++ `SimulationModule`。Python 侧只做配置、trace merge、validation 编排。

子模块职责：

- 读取 normalized DAG / trace event；
- 解析自身事实；
- 维护内部状态；
- 必要时通过统一 mutation API 修改 DAG；
- 输出可选 summary/debug。

当前 active 子模块：

| 模块 | 状态 |
| --- | --- |
| `NodeScaleModule` | smoke / 节点耗时缩放。 |
| `HiCacheModule` | state-only；维护 cache state，不修改 DAG。 |

## HiCache State Backend

当前 C++ HiCache backend 文件：

| 文件 | 作用 |
| --- | --- |
| `hicache_fact.hpp/.cpp` | 识别 HiCache event、收集 token dictionary、解析 span 和事实字段。 |
| `hicache_radix_tree.hpp/.cpp` | target page-level radix tree，支持 prefix、split、leaf group、remove。 |
| `hicache_model.hpp/.cpp` | state model 主体，按 target config 重建 page/state。 |
| `hicache_summary.hpp/.cpp` | 输出 final state、transition trace、审计计数。 |
| `hicache_module.hpp/.cpp` | SimulationModule registry glue。 |

后端输入分流规则已经收紧成一个主判断：

```text
consume fact iff fact_class == "invariant_state" && state_model_input == true
```

其它 HiCache 事件计入 `skipped_non_invariant_events`，不能更新 target state。

已知 invariant roles：

| role | 当前后端用途 |
| --- | --- |
| `request_tokens` | 建立 request -> target pages 映射。 |
| `lookup_path` | 在 target radix tree 上查 longest prefix，并按 L1/L2/L3 状态提升 resident。 |
| `insert_path` | 在 target radix tree 上插入 path，生成 L1 resident、dirty 或 write-through backup。 |
| `prefetch_intent` | 生成 target prefetch planned pages。 |
| `prefetch_check_point` | 按 target prefetch policy 标记 ready / late / suppressed。 |
| `capacity_request` | 按 target capacity 做 LRU-like eviction。 |
| `lock_scope_delta` | 以 token logical path 重建 target lock pages。 |
| `cache_config_observed` | 目前主要作为审计输入，不覆盖 target config。 |

unknown invariant role 会进入 `missing_invariant_facts["unknown_invariant_role"]`，不会静默消费。

### Page 重建

后端不消费 `page_identity` / `target_page_identity_page<page_size>` 作为主输入。page 由 token path 重建：

```text
for each full target page:
  page_hash = sha256(parent_hash_bytes + token_u32le...)
  page_id = cache_scope + "|" + page_hash
```

规则：

- target `page_size` 优先来自 modeling config；
- 没有 target page size 时才回落 source page size；
- 只生成完整 page，不生成 tail page；
- `cache_scope` 参与内部 page id，validation 可用 `oracle_page_key_mode=strip_scope` 与 raw oracle hash 对齐。

### State

当前维护的集合：

| 集合 | 说明 |
| --- | --- |
| `l1_resident_pages` | device/cache L1 resident。 |
| `l2_resident_pages` | host/cache L2 resident。 |
| `l3_resident_pages` | storage-readable / modeled L3 resident。 |
| `dirty_pages` | write-back dirty。 |
| `backuped_pages` | 已备份到 L2/L3 的 page。 |
| `evicted_pages` | L1 eviction 标记。 |
| `locked_pages` | target logical path 上 lock/ref 非零的 page。 |
| `prefetch_planned_pages` | target prefetch planned。 |
| `prefetch_ready_pages` | target prefetch ready。 |
| `prefetch_late_pages` | timeout 下 late。 |
| `prefetch_suppressed_pages` | best_effort / wait_complete finalization 或 timeout evidence 下 suppressed。 |
| `page_hit_counts` | write-through-selective hit count。 |

summary 输出位置：

```text
model_summary.json.modules[0].hicache
```

关键字段：

| 字段 | 说明 |
| --- | --- |
| `input_hicache_events` | 识别到的 HiCache events。 |
| `processed_hicache_events` | 实际消费的 invariant end events。 |
| `skipped_non_invariant_events` | 跳过的 source_actual / timing / oracle / debug events。 |
| `processed_events_by_role` | 各 role 消费计数。 |
| `missing_invariant_facts` | 缺失或未知 invariant 输入。 |
| `non_invariant_fact_usage` | 非不变量实际消费审计；正常必须为空。 |
| `final_state` | 模型最终 state sets 和 counts。 |
| `transition_trace` | request / operation / page 级模型状态转移。 |

## HiCache 当前验证状态

当前有效大 run 是 S1A manual：

```text
run label: 20260610_073946_profiling_hicache_state_mainline_one_matrix/01_s1a_manual
```

profile quality：

| 指标 | 值 |
| --- | --- |
| `quality_ready` | true |
| `profiling_ready` | true |
| invariant events | 6960 |
| required end events | 3480 |
| missing token dictionary refs | 0 |
| seq order errors | 0 |

model summary：

| 指标 | 值 |
| --- | --- |
| `input_hicache_events` | 7376 |
| `processed_hicache_events` | 3480 |
| `skipped_non_invariant_events` | 416 |
| `missing_invariant_facts` | `{}` |
| `non_invariant_fact_usage` | `[]` |
| `dag_mutations` | 0 |

normalized oracle validation：

| set | model | oracle | 结果 |
| --- | ---: | ---: | --- |
| `l1_resident_pages` | 32 | 54 | missing 22 |
| `l2_resident_pages` | 78 | 106 | missing 28 |
| `dirty_pages` | 0 | 0 | match |
| `backuped_pages` | 78 | 106 | missing 28 |
| `evicted_pages` | 46 | 52 | missing 28, extra 22 |
| `locked_pages` | 11 | 11 | match |

结论：采集契约和后端分流已经符合 invariant-only 目标，但 state model 仍不正确。当前应继续用 oracle mismatch
做逐 page provenance debug，而不是回退到 source movement 或 page identity 字段。

## 当前未实现

- HiCache state-to-DAG patch；
- async prefetch scheduler 的 timing/queue 模型；
- write-back background flush 的完整 target decision；
- exact eviction / evictable / allocator 逻辑；
- token-level radix node parent/ref chain 的完整 SGLang 等价实现；
- transition exact oracle。

这些缺口维护在 `docs/validation/hicache_state_model_defects.md`。

## Validation

validation 不是默认输出，只有 `--emit-validation` 或 config 中 `outputs.emit_validation=true` 时生成。

HiCache state validation 必须同时看：

- `validation_ready`；
- `validation_errors`；
- `hicache_state.invariant_coverage_ready`；
- `hicache_state.missing_invariant_facts`；
- `hicache_state.non_invariant_fact_usage`；
- `hicache_state.final_state_match` / `raw_final_state_match`；
- normalized `sets_diff_by_tier`。

只要 `non_invariant_fact_usage` 非空，即使 final state 偶然对齐，也不能宣称 invariant-only prediction 通过。
