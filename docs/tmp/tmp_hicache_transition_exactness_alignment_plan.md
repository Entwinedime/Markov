# HiCache Transition Exactness 最新结论

状态：active 临时文档。这里只保留最新 pre-bundle forced-token 基线的结果、当前根因和下一步边界。
历史 reconstruction 记录已清理，不再保留在本文。

注意：下述 2026-06-24 结果来自固定-plan workflow，早于显式 forced-token bundle gate。它仍是当前根因证据，但不是新
input contract 的 active 验收；修复模型前应先用新 bundle workflow 重跑并确认 failure set 复现。

## 最新保留基线

结果目录：

```text
data/profile_runs/sglang/20260624_150913_profiling_hicache_state_config_space_forced_python_probe/modeling/hicache_state_workflow_manual_3inputs
```

### final state

| 范围 | prediction | exact | 失败形态 |
| --- | ---: | ---: | --- |
| self 对角线 | 15 | 14 | 仅 `manual_deeper_pressure_prefetch -> c1_wts_wait_p128_low_l1` 失败 |
| full self/cross | 75 | 70 | 仅 target 为 `c1_wts_wait_p128_low_l1` 且 input 为 `manual_deeper_pressure_prefetch` 的 5 个格子失败 |

### transition exactness

| 范围 | prediction | ready | exact | 失败形态 |
| --- | ---: | ---: | ---: | --- |
| self 对角线 | 15 | 14 | 13 | `c0/deeper` marker-only，`c1/deeper` 被 final-state mismatch 阻塞 |
| full self/cross | 75 | 70 | 65 | `c0/deeper` 5 个格子 marker-only，`c1/deeper` 5 个格子 blocked |

### 分类汇总

| family | count | 结论 |
| --- | ---: | --- |
| `transition_exact` | 65 | final state、transition count、page lifecycle multiset 均对齐 |
| `evicted_marker_oscillation` | 5 | 只差 `mark_evicted` / `clear_evicted` 派生 marker，patch risk 为 low |
| `model_or_oracle_not_ready` | 5 | final state 未 ready，transition compare 不能作为完整结论 |

## 当前根因

### 1. c1 write-through-selective ack-stage ref 转移缺失

范围：

```text
input:  manual_deeper_pressure_prefetch
target: c1_wts_wait_p128_low_l1
source: 任意 config，包括 self c1 -> c1
```

现象：

```text
L1/L2/dirty/backuped/evicted: 全部 exact
locked_pages: model 11, oracle 1
extra locked pages: 10 个 prefix ancestor pages
```

语义结论：

- `write_backup()` 先把 GPU KV 写到 host，并通过 `inc_lock_ref()` 临时锁住 ancestor chain。
- `writing_check()` 看到 CPU write ack 后，会进入 `_finish_write_through_ack()`。
- `_finish_write_through_ack()` 在 storage enabled 时先走 `write_backup_storage()`，随后释放普通 ancestor lock。
- 这里的 `protect_host()` 是 storage backup 的 host 保护，不等价于普通 lock。

因此，模型当前缺的不是“再清掉一些 locked pages”，而是 ack 阶段的状态转移：

```text
ordinary write lock
    -> storage host protection
    -> ordinary lock release
```

当前 `c1` 的最终错误是把普通 lock 作为 active ref 一直保留到了 final state。

### 2. c0 deeper evicted marker 多一轮

范围：

```text
input:  manual_deeper_pressure_prefetch
target: c0_wt_timeout_p128_balanced
source: 任意 config，包括 self c0 -> c0
```

现象：

```text
final state: exact
add_l1_resident / add_l2_resident / mark_backuped / clear_backuped / remove_l1_resident / remove_l2_resident: 全部 exact
mark_evicted: model 251, observed 240
clear_evicted: model 186, observed 175
```

语义结论：

- 真实的 L1/L2/backuped 行为已经对齐。
- 差异只在 `evicted` 这个派生状态边界，多了一轮 host-only 可见中间态。
- 这类 mismatch 不应直接进入 DAG patch；当前 patch gate 维持 `drop` 是合理的。

## 下一步

当前只保留一个实现方向：

1. 先修 `c1` 的 write-through-selective ack/ref lifecycle。
2. 修完后再复查 `c1/deeper` 是否仍有 marker-only 偏移。
3. 只有在 `c1` 不再阻塞 final state 之后，才回看 `c0` 的 evicted marker 边界。

本阶段不要再把 `c1 dirty lifecycle`、`c2 best-effort prefetch completion` 或 `c3 low-host transient`
当成当前第一优先级问题，它们已经不在最新 failure set 里。
