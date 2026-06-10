# HiCache State Model 缺陷清单

维护方式：本文只记录当前 HiCache state model 的已知缺陷、边界和处理优先级。本文不保存实验流水账，
不依赖 `data/` 目录长期存在；真实 run 的指标只应以稳定验证编号、配置摘要和关键结论形式写入。

## 结论范围

截至 `2026-06-10`，本文描述的是关闭旧 observed/default 兼容路径后的 state model 缺陷。

当前已确认的前提：

- `replay` 只表示 `mode=faithful_replay` 的 trace graph baseline。
- HiCache state 只允许 `self-config prediction` 或 `cross-config prediction`。
- 显式 `write_policy=observed`、`prefetch_policy=observed`、`storage_prefetch_policy=observed` 是非法配置。
- source run 中已经发生的 movement 不能直接驱动 target state。
- `non_invariant_fact_usage` 正常必须为空；`skipped_non_invariant_events` 只是诊断计数。
- state snapshot、target actual trace、debug 字段和 oracle-only transient 只能用于 validation/debug。

当前文档基于代码审查、fixture 和已有 validation 结论整理。关闭旧兼容路径后，还需要对现有 S1A/S1B
profile manifest 重新执行 modeling-only validation，才能得到最新真实大 run 指标。

## 缺陷分级

| 等级 | 含义 | 处理原则 |
| --- | --- | --- |
| `P0` | 会阻断 invariant-only state prediction 的核心机制缺口。 | 优先修复或明确需要新增不变量。 |
| `P1` | 会导致真实大 run final state / timeline 明显不对齐。 | 随主线一 self-config / cross-config validation 逐项修。 |
| `P2` | 当前 fixture 可覆盖部分行为，但不是完整 SGLang 机制。 | 保留边界说明，补充更强 fixture 或 oracle。 |
| `P3` | 维护性或验证新鲜度问题。 | 不阻塞短期实验，但应避免继续积累。 |

## 处理类型

| 类型 | 含义 |
| --- | --- |
| `采集更多事件` | 当前 profiling facts 不足以证明 exact；这不等于立刻重跑 profiler，默认先作为 future / blocked 条件登记。 |
| `完善建模` | 现有 facts 基本足够，当前迭代应优先改 C++ state model、Python validation 或 fixture。 |
| `采集更多事件 + 完善建模` | 既可能缺事实，也缺模型；当前迭代仍先用现有数据做 modeling-only 修复，只有逐 trace 证明事实不足时才补采。 |
| `重跑验证` | 不需要新 profiler，也不一定改模型，先用当前代码重新执行 modeling-only validation 更新结论。 |
| `代码清理` | 不影响当前语义，但需要删除、重命名或加 guard，降低维护风险。 |

## 迭代策略

为保证迭代速度，当前默认策略是 **不重新采集 profiler**：

1. 优先使用已有 profile manifest 做 modeling-only validation。
2. 先修 `non_invariant_fact_usage=[]` 前提下仍失败的 self-config prediction。
3. 每个 mismatch 先做逐 trace 对比，判断是模型规则错误还是事实缺失。
4. 只有满足以下条件之一，才进入集中重采窗口：
   - 同一类 mismatch 已经用现有 facts 定位到无法区分的二义性；
   - 缺少的事实是 target-independent 不变量，而不是 target actual 答案；
   - 补采能同时服务多个缺陷或主线实验，不是为单个样本硬补；
   - 新采集字段可以进入 fixture 或 validation oracle，成为长期回归资产。

因此，本文中的“采集更多事件”主要表示长期闭环或 exact 验证需要新增事实；不表示当前每一轮修复都要重跑 profiling。
如果确实进入重采，应尽量一次性补齐所有已识别缺失事件，避免每发现一个缺陷就重跑一次。

## 集中重采原则

重采不是默认迭代动作；它是一个集中设计、集中执行、集中归档的 profiling 版本升级。

一次重采应尽量同时覆盖：

- write-back flush decision / completion；
- prefetch task lifecycle 和 transfer completion；
- lock/ref parent-chain transition；
- radix split / merge / removed pages；
- page-size independent token path / range hash；
- ordered state transition oracle；
- capacity / eviction candidate 或 evictable state。

重采前必须先完成：

1. 用现有 profile manifest 跑完 modeling-only validation。
2. 用逐 trace 对比列出无法由现有 facts 区分的缺口。
3. 把所有缺口合并成一份 profiling event contract。
4. 用 fixture 固化新增字段的最小样例。
5. 再启动一次集中 profiling，而不是按缺陷分批反复重采。

## 总览

| ID | 等级 | 处理类型 | 缺陷 | 当前表现 | 主要影响 |
| --- | --- | --- | --- | --- | --- |
| `HCSM-D1` | `P0` | 现有数据先修建模；若重采则纳入集中 event contract | target-only write-back flush 缺失 | 只处理 dirty eviction 触发的 modeled writeback，不知道 target 何时后台 flush。 | L2、dirty、backuped、evicted 容易错。 |
| `HCSM-D2` | `P0` | 现有数据先修建模；若重采则纳入集中 event contract | prefetch async scheduler 未建模 | 只能用 schedule/progress/transfer evidence 近似 planned/ready/suppressed。 | prefetch ready / late / suppressed 容易错。 |
| `HCSM-D3` | `P0` | 当前跳过；若重采则纳入集中 event contract | lock/ref parent chain 未建模 | `lock_ref_inc/dec` 被跳过，不生成 target locked state。 | locked_pages 与 evictable set 缺失。 |
| `HCSM-D4` | `P1` | 现有数据先修建模 | capacity / eviction exactness 不足 | 使用 LRU-like 近似，不等价于 SGLang evictable / allocator / lock 逻辑。 | resident、dirty、evicted page 可能偏离。 |
| `HCSM-D5` | `P1` | 现有数据先修建模；若重采则纳入集中 event contract | 完整 radix split / merge 缺失 | 只用 lookup path、known prefix 和 operation-level removed pages 做最小模型。 | page-size what-if 与 prefix overlap 场景仍有风险。 |
| `HCSM-D6` | `P1` | 不阻塞当前 page64/page128；若重采则纳入集中 event contract | page identity 仍依赖有限 target page size 字段 | `target_page_identity_page<page_size>` 只覆盖预声明 page size。 | 新 page size 往往需要重新 profile。 |
| `HCSM-D7` | `P1` | 按 mismatch 判断事实缺口；若重采则合并进集中 event contract | source movement 被跳过后的覆盖缺口 | 只有 source movement 能解释的状态变化不会被预测。 | final state mismatch 会暴露，但模型不能自动补齐。 |
| `HCSM-D8` | `P2` | 不阻塞 final state；若重采则纳入集中 event contract | timeline exact oracle 不足 | 现有 timeline 多为 kind/page multiset coverage，不是 ordered transition log。 | `exact_match=false` 难以严格归因。 |
| `HCSM-D9` | `P2` | `完善建模` | state-to-DAG patch 尚未实现 | HiCacheModule 仍是 state-only，不修改 DAG。 | state 对齐不等于 E2E prediction 完成。 |
| `HCSM-D10` | `P3` | `代码清理` | C++ 中仍有旧 movement apply 分支 | run loop 已跳过，但 `apply_write_to_l2/l3`、`apply_remove_page`、`apply_lock_ref` 等仍在。 | 维护时容易误读为仍可消费 source movement。 |
| `HCSM-D11` | `P3` | `重跑验证`，不重采 | 大 run validation 结果需要刷新 | 旧 S1A/S1B 指标形成于关闭旧兼容路径之前。 | 不能代表当前代码口径。 |

## HCSM-D1：target-only write-back flush 缺失

处理类型：现有数据先修建模；只有逐 trace 证明 flush decision 不可由现有 facts 推导时，才纳入集中重采。

### 当前行为

模型当前可以处理一种明确因果：当模型自己的 capacity / eviction 逻辑选择了 dirty victim 时，
会执行 modeled writeback，把 page 放入 host/L2、标记 backuped、清除 dirty。

### 缺失机制

模型不知道 target run 中更一般的 write-back flush：

- 何时触发后台 flush；
- flush 哪些 dirty page；
- flush 与 eviction、lookup、prefetch、controller task 的先后顺序；
- policy / memory pressure / writing check 是否触发 flush；
- flush 后 host backup 何时对后续 load 可见。

### 原因

这些行为不是 source movement 的不变量。source `write_backup` / `write_storage_schedule` 不能直接用于 target，
因为 source policy、timing、capacity pressure 和 target policy 可能不同。

### 影响

旧基线中 `S1B self-config prediction` 的 L2、dirty、evicted 差异与该缺陷高度相关。当前模型可能低估
backuped/L2 pages，也可能保留过多 dirty pages。

### 处理方向

短期只能继续完善 dirty eviction 下的 modeled flush，并用逐 trace 对比定位 remaining mismatch。
真正修复需要新增能描述 target flush decision 的不变量，例如 operation-level writeback decision fact，
或更强的 write-back oracle。

需要新增采集或 oracle：

当前不立即重采。若进入集中重采，以下字段应一次性纳入 event contract：

- operation-level writeback decision：request、operation、page、触发原因、source/target tier；
- writeback start/end 或 flush completion：page、bytes/tokens、是否形成 host backup；
- flush 与 eviction / insert / prefetch 的调用内顺序；
- dirty -> backuped 的 before/after transition oracle。

需要完善建模：

- 将 dirty eviction flush 与 background / policy flush 分成不同路径；
- 建模 flush 后 L2/backuped/dirty 的状态转移；
- 明确 write-back 下 L2 与 L3 可读性的边界；
- 在 capacity eviction 前后做逐 page flush 顺序校验。

## HCSM-D2：prefetch async scheduler 未建模

处理类型：现有 schedule / progress / transfer evidence 先修建模；future exact scheduler 进入集中重采。

### 当前行为

模型维护：

- `prefetch_planned_pages`；
- `prefetch_ready_pages`；
- `prefetch_late_pages`；
- `prefetch_suppressed_pages`。

模型使用 `prefetch_schedule` 生成 planned pages，使用绑定 target schedule 的 `l3_to_l2_transfer` 和
`prefetch_progress_state` 提供 completion / progress evidence。

### 缺失机制

模型没有模拟 SGLang cache controller 的异步 prefetch scheduler：

- 不模拟 storage bandwidth；
- 不模拟 transfer queue；
- 不模拟并发请求之间的抢占和排队；
- 不模拟 target policy 下何时真正完成；
- source run 没有 transfer/progress 时，模型不能凭空预测 target ready；
- write-back target 下 transfer credit 仍需要显式配置开关，而不是完整机制推导。

### 影响

prefetch ready / late / suppressed 是当前最容易出现集合差异的区域。旧基线中 `prefetch ready 0/36`
一类结果说明，仅有 schedule 不足以证明 target ready。

### 处理方向

短期先把 planned completion evidence 的使用边界收紧，避免未绑定 schedule 的 transfer 污染 state。
中期需要采集 operation-level prefetch transition 或 controller task oracle，用于验证和训练更明确的 timing 规则。

需要新增采集或 oracle：

当前不立即重采。若进入集中重采，以下字段应一次性纳入 event contract：

- prefetch task enqueue / start / complete / cancel；
- storage transfer start/end、request id、operation id、page list、completed tokens；
- controller queue 中同 request 的 planned pages 和实际 ready pages；
- timeout / best_effort / wait_complete 的终止原因；
- prefetch suppressed / late 的 operation-level transition。

需要完善建模：

- 将 schedule、progress、transfer completion 的状态职责拆开；
- 为 `timeout`、`best_effort`、`wait_complete` 分别定义终止条件；
- 明确 write-back target 是否允许 transfer credit；
- 避免重复 transfer 对同一 request 重复增加 L2 resident；
- 用 target schedule pages 归一化跨 page size completion credit。

## HCSM-D3：lock/ref parent chain 未建模

处理类型：当前跳过，不用 source lock/ref 修 state；未来需要 parent/ref oracle 时进入集中重采。

### 当前行为

`lock_ref_inc` / `lock_ref_dec` 现在作为非不变量跳过，不再生成 `mark_locked` / `clear_locked` transition。

### 缺失机制

SGLang 的 lock/ref 沿 radix tree 父链传播。page size、prefix split、node merge、policy timing 变化后，
source lock/ref 页集合不是 target 不变量。

### 影响

- `locked_pages` 不能由当前模型预测；
- eviction eligibility 缺少 lock/ref 约束；
- capacity eviction 可能选择真实 target 中被 lock 保护的 page；
- cross-config final state 中的 locked_pages 差异不能靠消费 source lock/ref 修复。

### 处理方向

必须重建足够的 target radix parent chain，或新增 ordered lock/ref oracle。短期 validation 可以把 lock/ref
作为独立缺口记录，不能把 source lock/ref 重新接回模型输入。

需要新增采集或 oracle：

当前不立即重采。若进入集中重采，以下字段应一次性纳入 event contract：

- lock/ref mutation 的 ordered transition：page、node id、parent id、delta、before/after ref count；
- radix node parent-child 关系；
- page size what-if 下 target node path；
- evictable 判断时的 locked / host_ref_counter / lock_ref 状态。

需要完善建模：

- 基于 target radix tree 推导 lock/ref parent chain；
- 将 locked_pages 纳入 eviction eligibility；
- 在 capacity eviction 中跳过 target locked pages；
- 区分 final locked set validation 和 transition-level lock/ref coverage。

## HCSM-D4：capacity / eviction exactness 不足

处理类型：现有数据先修建模；只有 victim 选择出现不可解释二义性时才补采。

### 当前行为

模型有 L1/L2 capacity 配置、touch order 和 LRU-like eviction。dirty victim 会触发 modeled writeback。

### 缺失机制

当前不是 SGLang exact eviction：

- 没有完整 evictable set；
- 没有完整 lock/ref 约束；
- 没有完整 allocator / memory pool 行为；
- 没有完整 node-level radix tree；
- leaf group 处理仍是局部近似；
- `evict_summary` 只作为部分 target eviction trigger。

### 影响

容量压力越强，L1/L2 resident、dirty、backuped、evicted 越容易偏离 oracle。该缺陷会和 write-back flush
缺陷互相放大。

### 处理方向

优先在 S1B self-config prediction 中逐 trace 对比 eviction 前后的 page 集合，确认是 victim 选择错误、
dirty flush 错误，还是 radix removed / prefix 判断错误。

需要新增采集或 oracle：

当前不立即重采。只有逐 trace 证明 victim 选择缺少事实时，才把以下字段合并进集中 event contract：

- 如果逐 trace 发现 victim 选择无法由现有 facts 解释，需要采集 evictable set 或 eviction candidate list；
- 如果真实 eviction 被 lock/ref 保护影响，需要采集 lock/ref oracle；
- 如果 allocator / memory pool 影响容量，需要采集 pool capacity、available size、evictable size 的 operation-level snapshot。

需要完善建模：

- 改进 LRU-like touch order；
- 将 backuped、dirty、locked、leaf group 纳入 victim 选择；
- 区分 L1 capacity eviction 和 L2 host cache eviction；
- 明确 write-through / write-back 下 eviction 后 evicted、backuped、dirty 的状态组合。

## HCSM-D5：完整 radix split / merge 缺失

处理类型：现有 operation-level removed pages 先修建模；复杂 split / merge 无法解释时进入集中重采。

### 当前行为

模型用以下事实近似 radix 行为：

- lookup path；
- known prefix pages；
- insert prefix 裁剪；
- operation-level `radix_removed_page_identity`；
- target radix removed pages；
- page-size specific target page identity。

### 缺失机制

模型没有重建完整 radix tree：

- 没有 node split；
- 没有 parent-child；
- 没有完整 merge / remove 生命周期；
- prefix overlap 复杂时仍依赖启发式；
- page size what-if 下不能完整重演 target tree mutation。

### 影响

复杂 prefix overlap、page split、重复插入和 remove 组合仍可能造成 resident / dirty / backuped 清理错误。

### 处理方向

短期继续通过 operation-level removed pages 缩小误差。长期应采集或推导 size-independent token path / range
信息，让 target radix tree 能从不变量重建。

需要新增采集或 oracle：

当前不立即重采。若进入集中重采，以下字段应一次性纳入 event contract：

- radix node id、parent id、children hash、page range；
- insert 前后 split / merge 的 operation-level mutation；
- removed pages 对应的原因：覆盖、merge、eviction、prefix overwrite；
- target page size 下的 token range 到 page hash 映射。

需要完善建模：

- 从 lookup path 和 token ranges 重建 target radix tree；
- 让 insert 能执行 target split / merge，而不是只做 prefix 裁剪；
- 将 radix removed pages 与 resident / dirty / backuped / evicted 清理统一；
- 减少依赖 `target_radix_removed_page_identity_page<page_size>` 这种有限枚举字段。

## HCSM-D6：page identity 仍依赖有限 target page size 字段

处理类型：不阻塞当前已声明 page64/page128 的主线验证；若重采则补 size-independent identity。

### 当前行为

跨 page size prediction 依赖：

- `target_page_identity`；
- `target_page_identity_page64`；
- `target_page_identity_page128`；
- 同类 target radix removed page identity 字段。

### 缺失机制

这些字段只覆盖 profile 时预声明的目标 page size。如果后续预测新 page size，现有 trace 不一定有对应 identity。

### 影响

新增 page size 可能必须重新 profile，否则模型会暴露 `target_page_identity_or_token_path` 缺失。

### 处理方向

设计 size-independent token path digest / range hash。page identity 应从 token range 和 hash path 推导，而不是按 page size
无限增加预声明字段。

需要新增采集或 oracle：

当前不为 page64/page128 重采。若进入集中重采，以下字段应一次性纳入 event contract：

- token range；
- page hash seed / hash path digest；
- prefix path 与 new input suffix 的稳定表示；
- page-size independent 的 range hash 或 token path digest。

需要完善建模：

- 在新增事实落地前，C++ 侧只能继续按已声明 page size 选择 target identity；
- 新事实落地后，需要实现 token range -> target page identity 的推导器。

## HCSM-D7：source movement 被跳过后的覆盖缺口

处理类型：按 mismatch 判断事实缺口；默认先判定模型规则是否可修，不能单点反复重采。

### 当前行为

以下 source movement 不再驱动 target state：

- `load_back`；
- `write_backup`；
- `write_storage_schedule`；
- `remove_page`；
- `evict`；
- `lock_ref_inc` / `lock_ref_dec`；
- 未绑定 target schedule 的 `l3_to_l2_transfer`；
- 普通 tier movement。

### 缺失机制

如果某个 target state 变化只能从 source movement 看出来，而没有独立不变量，模型不会预测它。

### 影响

这会造成 final state mismatch，但这是正确暴露缺口，不是回归。错误做法是重新消费 source movement，
因为那会把 source run 的答案误当 target prediction。

### 处理方向

每个 mismatch 都应拆成两类：

- 模型已有足够不变量但机制没写对；
- profiling 没有提供足够不变量。

第二类必须补 profile fact 或 oracle，不能用 source movement 兜底。

需要新增采集或 oracle：

当前不立即重采。只有 source movement 背后的 decision 无法由现有 facts 推导时，才合并进集中 event contract：

- 把 source movement 背后的 decision 抽成 operation-level invariant fact；
- 对 write/load/remove/evict 分别记录触发条件，而不是只记录已经发生的 movement；
- 对 target validation 记录对应 transition oracle，帮助判断缺的是 fact 还是模型规则。

需要完善建模：

- 在没有新增不变量前，不应补模型逻辑；
- 新增 fact 后，再按 fact 语义实现 target state transition。

## HCSM-D8：timeline exact oracle 不足

处理类型：不阻塞 final state match；若进入集中重采，则同时补 timeline exact oracle。

### 当前行为

timeline validation 主要比较 raw snapshot timeline 的 kind/page multiset coverage。

### 缺失机制

它不是严格 ordered transition log：

- 同 timestamp 排序可能不稳定；
- start/end snapshot 粒度不够；
- 多进程对象可能缺 object id；
- oracle-only transient 可能只在 snapshot 中出现；
- final correction 可能掩盖 lock/ref 释放顺序。

### 影响

`timeline match=true` 能说明模型没有明显额外 transition，但 `exact_match=false` 不能直接判定模型错误。

### 处理方向

主线三需要新增 ordered transition oracle：每次 state mutation 输出 operation id、request id、scope、page、kind、
before/after 和真实执行顺序。

需要新增采集或 oracle：

当前不立即重采。若进入集中重采，以下字段应一次性纳入 event contract：

- ordered state transition log；
- object id / node id；
- transition kind；
- affected page set；
- before/after 最小状态字段；
- 调用内顺序号，不能只靠 timestamp。

需要完善建模：

- C++ state model 已经输出 transition trace；等 ordered oracle 可用后，需要把 validation 从 multiset coverage
  升级为 ordered / scoped transition compare。

## HCSM-D9：state-to-DAG patch 尚未实现

处理类型：`完善建模`。

### 当前行为

HiCacheModule 仍是 state-only：

- 不修改 DAG；
- 不输出 state-derived latency；
- 不把 hit/miss/prefetch/writeback 映射成性能变化。

### 影响

即使 state 完全对齐，也不能说明 HiCache E2E prediction 完成。E2E 残差只能作为后续 DAG patch 的 sanity check。

### 处理方向

state model 闭环后，另起 state-to-DAG patch 主线，定义 transition -> DAG mutation / duration / dependency 的映射。

需要新增采集或 oracle：

- 当前阶段不强制新增；如果 DAG patch 需要 latency/bandwidth 参数，后续再采集 transfer duration、storage bandwidth、
  host/device copy duration 等性能事实。

需要完善建模：

- 定义 state transition 到 DAG mutation 的映射；
- 给 hit/miss/load/writeback/prefetch 分配 anchor node；
- 建立 duration 和 dependency 规则；
- 保持默认 E2E 来自 DAG 拓扑仿真，而不是 state transition 求和。

## HCSM-D10：旧 movement apply 分支仍在代码中

处理类型：`代码清理`。

### 当前行为

run loop 已经跳过非不变量 movement，但 C++ 中仍保留：

- `apply_write_to_l2`；
- `apply_write_to_l3`；
- `apply_remove_page`；
- `apply_lock_ref`；
- `apply_generic_tier_move`。

### 影响

这些分支当前主要是受保护的历史代码或未来入口，维护时容易误读为模型仍可直接消费 source movement。

### 处理方向

后续可以继续清理：

- 删除确认为死分支的逻辑；
- 或把它们改名为只接受未来 invariant operation fact 的入口；
- 并在入口处加更硬的 guard 和 fixture。

需要新增采集或 oracle：

- 不需要。

需要完善建模：

- 不需要改变状态语义，主要是删除死分支、重命名、加 guard；
- 如果未来这些分支要服务新的 invariant fact，必须先定义新 fact 契约。

## HCSM-D11：大 run validation 结果需要刷新

处理类型：`重跑验证`。

### 当前行为

已有 S1A/S1B 大 run 结论形成于关闭旧兼容路径之前。它们仍可作为机制定位线索，但不能代表当前代码口径。

### 影响

当前不能直接用旧表格判断：

- S1A lock/ref 跳过后还有哪些 mismatch；
- S1B write-back / prefetch / eviction 缺口是否扩大或缩小；
- cross-config prediction 的 timeline extra 是否仍相同。

### 处理方向

不重新 profiler 的前提下，使用已有 profile manifest 重新执行 modeling-only validation，更新 active validation 文档。

需要新增采集或 oracle：

- 不需要重新 profiler；
- 使用已有 profile manifest 和 target oracle trace 即可。

需要完善建模：

- 先不改模型，先刷新 validation；
- 刷新后再根据 mismatch 分类决定进入 `完善建模` 还是 `采集更多事件`。

## 当前不允许的修复方式

- 不允许恢复 observed replay。
- 不允许显式 observed policy。
- 不允许用 target actual trace 作为模型输入。
- 不允许整体消费 state snapshot。
- 不允许用 source `write_backup/remove_page/load_back/lock_ref` 直接修 final state。
- 不允许因为 final state 偶然对齐就忽略 `non_invariant_fact_usage`。

## 建议处理顺序

1. 重跑当前代码口径下的 S1A/S1B modeling-only validation，刷新缺陷定位。
2. 优先修 `S1B self-config prediction`，因为它旧基线中 `non_invariant_fact_usage=[]`，问题更集中在真实机制。
3. 按逐 trace 对比分拆 write-back、prefetch、eviction 三类 mismatch。
4. 对确认为不变量不足的问题，补 profiling fact 或 ordered oracle，不用 source movement 兜底。
5. 当前 state model 达到 final state / timeline coverage 验收后，再进入 state-to-DAG patch。

## 验证命令模板

不重新 profiler 时，使用已有 profile manifest 重跑 modeling：

```bash
python3 scripts/internal/model_runner.py \
  --config <self_config_or_cross_config_modeling_config> \
  --profile-manifest <profile_manifest.json> \
  --hicache-oracle-trace <target_oracle_trace.json> \
  --output-dir <modeling_output_dir> \
  --mode cache_state \
  --emit-module-summary \
  --emit-validation
```

代码级回归检查：

```bash
cmake --build build --target trace_graph -j 8
python3 tests/run_hicache_state_fixtures.py
python3 tests/run_modeling_smoke_fixtures.py
python3 tests/run_hicache_mainline_config_fixtures.py
python3 tests/run_profiling_fixtures.py
clang-format --dry-run --Werror \
  src/modeling/trace_graph/include/trace_graph/modules/hicache/hicache_model.hpp \
  src/modeling/trace_graph/src/modules/hicache/hicache_model.cpp \
  src/modeling/trace_graph/include/trace_graph/frontend/model_config.hpp \
  src/modeling/trace_graph/src/frontend/model_config.cpp
git diff --check
```
