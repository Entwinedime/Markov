# HiCache State Validation

本文是 HiCache state validation 的 active 文档，只维护当前有效规则、建模边界、下一步主线和后续新结果。
当前 state model 缺陷清单维护在 [hicache_state_model_defects.md](hicache_state_model_defects.md)。
历史只读结论已经拆到 [hicache_state_validation_legacy.md](hicache_state_validation_legacy.md)。

## 文档分工

- 本文只记录仍然指导后续工作的内容：目标、术语、硬约束、profiling 不变量、state model 边界、验证入口、当前主线和新结果模板。
- 缺陷文档只记录当前 state model 未闭环机制、影响面、禁止的错误修复方式和处理顺序。
- 历史只读文档只保存旧批次结论和旧口径结果，不再追加新主线、新约束或新修复。
- 旧结果如果没有 `non_invariant_fact_usage=[]` 和 `invariant_coverage_ready=true`，只能作为历史诊断，不能作为
  invariant-only prediction 已闭环的证明。
- 新结果必须写回本文；只有当某个批次被后续结论完全取代时，才把它移动到历史只读文档。

## 记录规则

- 验证批次使用 `HCSV-YYYYMMDD-<scope>` 作为稳定编号；单次真实运行只保留 run label、配置名、命令入口和抽取后的指标。
- 文档不得把 `data/profile_runs/...`、`data/modeling_runs/...` 或 `/tmp/...` 作为长期证据入口。
- 需要复现时按本文命令重新运行，并把新的 profile quality、faithful replay、self-config prediction 和
  cross-config prediction 摘要写回本文。
- 如果一次验证依赖多个真实 profiling run，文档只写 run label 和内联指标；run 目录可在文档更新后清理。
- `--hicache-ratio` 可以为验证需要调整，但必须 `> 1.0`；调整时要说明目的，capacity pressure 优先来自 workload 或显式 capacity config。
- commit message、验证记录和约束文档都应使用规范、可检索的措辞；提交前必须完成 build/test/json/format/diff 检查。

## 术语约束

- `replay` 是保留术语，只能指 `mode=faithful_replay`：不加载任何子模块、不执行 DAG patch，只消费完整真实 merged trace
  构建并拓扑重放 trace graph baseline。
- 凡是启用 `HiCacheModule` 的 state 建模，都必须称为 `self-config prediction` 或 `cross-config prediction`。
- `self-config prediction` 指 base facts 和显式 target config 来自同一个配置场景；它仍是 prediction，不允许省略 target config
  后让模型回落到 source 行为答案。
- `cross-config prediction` 指 base facts 来自 source run，显式 target config 来自另一个配置场景。
- HiCache state model 在任何场景下都只能消费不变量 facts 和显式 target config；target actual trace、state snapshot、
  source movement、oracle-only transient、debug 字段和 policy 结果只能进入 validation / debug，不能进入模型输入。
- 显式 `write_policy=observed`、`prefetch_policy=observed` 或 `storage_prefetch_policy=observed`
  是非法配置，不再作为旧配置兼容入口。
- `--hicache-oracle-trace` 只提供 validation oracle。它可以来自目标 run 的 merged trace 或专门抽取的 oracle trace，
  但不能作为模型事实源。

## 验证目标

本阶段只验证一个问题：HiCache cache state 能否由 profiling 采集到的不变量 facts 和 target cache config 推导出来。
它不是 DAG patch，也不是 E2E 性能预测验收。

目标 trace 的建模口径是：

```text
base profiling / merged trace 中的不变量 facts + target cache config
  -> C++ HiCache state model
  -> predicted target cache state trace
```

真实 target run 只作为 oracle，用来验证 predicted target cache state trace；cross-config prediction 不能偷读 target actual trace
作为模型输入。

state validation 用来给下一阶段 state-to-DAG patch 切开责任边界：

- 如果 cache state 推导不准，本阶段必须能定位到 request、operation、page 和 transition。
- 如果 state 已闭环但后续 E2E 不准，应优先检查 state-to-DAG 映射、DAG anchor、duration、带宽或 dependency。
- E2E 残差不能反向证明 cache state 正确，也不能掩盖 state trace mismatch。

## 验收口径

| 层级 | 验证对象 | 当前用途 |
| --- | --- | --- |
| Summary 验证 | final page count、resident set、transition count | 快速发现明显错误，但不能单独作为验收。 |
| 逐 trace 验证 | request / operation / page / state transition | 本阶段主验收，用于定位 state 推导错误。 |
| Timeline coverage | raw snapshot timeline 的 kind/page multiset coverage | 验证模型没有预测 raw snapshot 完全无证据的额外 transition。 |
| E2E 验证 | 端到端预测时间 | 只作为后续 DAG patch 的旁路 sanity check。 |

当前硬门槛：

- `faithful_replay` 必须不加载任何子模块，验证 trace graph baseline 的 DAG 拓扑重放。
- `self-config prediction` 必须使用显式 target config，并同时满足 final state match、invariant coverage ready、无非法
  non-invariant fact usage；full-mechanism 输入还应满足 event delta match 或记录 event delta 边界。
- `cross-config prediction` 必须满足 final state match、invariant coverage ready、无非法 non-invariant fact usage。
- page size、capacity、write policy、prefetch policy 这类 target config 变化，不能消费 target actual trace 作为模型输入。
- `timeline match=true` 表示模型 transition 全部被 raw snapshot timeline 覆盖。
- `timeline exact_match=false` 是诊断项，不等价于 state model 错误；需要结合 `model_extra_transition_count`、final state
  和 mismatch kind 判断。

## 当前状态

截至 `2026-06-09 21:00:30 +0800`：

- `HCSV-20260608-state-matrix` 已移入历史只读文档。该矩阵的 final state、timeline coverage、profile quality 指标仍可用于
  机制覆盖和回归定位，但形成时还没有 `non_invariant_fact_usage` 字段，不能单独证明 invariant-only prediction 已闭环。
- `HCSV-20260609-mainline-one-manual-l1` 的 profiling quality 已通过，但双向 cross-config prediction 未通过，主线一不能宣称完成。
- `HCSV-20260609-code-audit-new-invariant` 已完成 modeling-only 重跑，不重新 profiler；该批次确认当前 state model 仍有机制缺口，
  其指标作为关闭旧兼容路径前的修复基线保留，不能代表当前代码口径的最新验证结果。

当前代码已经具备的检查：

- C++ HiCache summary 输出 `non_invariant_fact_usage_by_role` 和 `non_invariant_fact_usage`。
- Python validation 只要看到 `non_invariant_fact_usage` 非空，就把 `invariant_coverage_ready` 判为 `false`。
- fixture 覆盖显式 observed policy 非法、source lock/ref 跳过、未绑定 target schedule 的
  `l3_to_l2_transfer` 跳过、source `write_backup/remove_page/write_storage_schedule` 不驱动 target state。

## 当前有效结果

### HCSV-20260609-code-audit-new-invariant

本批次只用已有 `S1A/S1B` profile manifest 和目标 oracle trace 重新执行 modeling validation，未重新采集 profiler。
这些指标是关闭旧兼容路径前的修复基线；后续必须在当前代码口径下重新执行 modeling validation。

| prediction | `final_state_match` | `invariant_coverage_ready` | non-invariant usage | 主要结果 |
| --- | --- | --- | --- | --- |
| `S1A self-config prediction` | `false` | `false` | `lock_ref_inc=516`、`lock_ref_dec=514` | L1 `64/54`、evicted `42/52`；final mismatch，同时仍消费 lock/ref source facts。 |
| `S1B self-config prediction` | `false` | `true` | `[]` | L2 `52/144`、dirty `128/72`、evicted `52/108`、prefetch ready `0/36`；主要是 write-back flush / prefetch completion 机制未建完整。 |
| `S1A -> S1B` | `false` | `true` | `[]` | L1 `102/108`、L2 `158/144`、dirty `82/72`、evicted `124/108`、prefetch `728/36/692` vs `742/36/706`；timeline `model_extra_transition_count=52`。 |
| `S1B -> S1A` | `false` | `true` | `[]` | 除 `locked_pages` 外 final state 对齐；model `0`、oracle `1`，timeline `match=true`、`model_extra_transition_count=0`。 |

结论：

- 当前 `S1A/S1B` 主线一不能视为完成。
- `S1B -> S1A` 已经接近，只剩 target lock/ref final oracle 缺口。
- `S1A -> S1B` 和 `S1B self-config prediction` 仍有实质 state model 缺口。
- `S1A self-config prediction` 在该基线中仍消费 lock/ref source facts，不能称为 invariant-only prediction；
  当前代码已经改为跳过这类事实，需重新跑 modeling-only validation 更新指标。

## Profiling 不变量

当前 profiling 能提供的可建模不变量如下：

| fact | 采集位置 / 字段 | 能支持的 state model 行为 | 审计结论 |
| --- | --- | --- | --- |
| token path -> page hash | `page_hashes:*`、`page_hashes_concat:*`、`page_hashes_after_prefix:*` | lookup、insert、prefetch schedule、page-size what-if target page identity | 充分，是跨 page size prediction 的核心不变量。 |
| request / operation / timestamp | `request_id`、`operation_id`、event role、`ts` | request-scope pending lookup/prefetch、progress 终止、transition trace 归因 | 基本充分，但 timestamp 只能近似真实 async 调度，不等价于 target scheduler。 |
| prefix / new input tokens | `prefix_len`、`new_input_tokens`、insert / schedule tokens | target suffix pages、insert prefix 裁剪、prefetch suffix 生成 | 对大多数 page split 足够；完整 radix node split 仍是近似。 |
| target page identity | `target_page_identity`、`target_page_identity_page<page_size>` | page-size what-if 下替换 base page identity | 对已声明 page size 充分；当前主线一同时采 page64/page128。长期更好方案是 size-independent token path digest / range hash，避免新增 page size 时重新 profile。 |
| operation-level radix removal | `radix_removed_page_identity`、`target_radix_removed_page_identity`、`target_radix_removed_page_identity_page<page_size>` | insert 内 radix pages 消失后的 resident/dirty/backuped 清理 | 对当前 operation-level 清理足够；不是完整 radix tree oracle。 |
| prefetch progress evidence | `prefetch_progress_state` 中的 policy、page size、ongoing、loaded/completed tokens、operation hash pages | ready/late/suppressed 推导、timeout/best_effort 终止 | 部分充分；没有 source transfer/progress 的 target pages 不能凭空预测 ready。 |
| transfer completion evidence | `l3_to_l2_transfer`、`completed_tokens`、source/target pages | 将 planned pages 标为 ready，并补 L2/L3 resident evidence | 有条件充分；跨 policy/write-back 时必须显式决定是否允许 credit。 |
| capacity config | modeling config 或 target experiment `modeling.hicache` | L1/L2 capacity enforcement、LRU-like eviction | 只有显式 config 才可作为模型输入；snapshot 中 observed max/final count 只用于 audit。 |
| state snapshot | `hicache_state:self`，`model_input=false` | validation oracle、profile quality、radix removed materialization 的收尾辅助 | 不能整体进入 C++ state model；只能生成 operation-level fact 或 validation。 |

## State Model 边界

已建模或部分建模：

- lookup / insert 的 target page identity 和最小 prefix/radix-known 逻辑；
- explicit capacity 下的 LRU-like eviction；
- write-through 和 write-through-selective 的基础状态更新；
- dirty eviction 触发的 modeled writeback；
- prefetch planned / ready / late / suppressed 的基础集合维护；
- operation-level radix removed pages 的 resident / dirty / backuped 清理。

尚未完整建模：

- **target-only write-back flush**：当前没有不变量告诉模型 target `write_back` 何时 flush、flush 哪些 page、flush 与
  eviction/prefetch 的顺序如何交错。
- **prefetch async scheduler**：source run 的 progress/transfer 不等价于 target policy 下的完成时间。
- **lock/ref parent chain**：`lock_ref_inc/dec` 沿 radix tree 父链更新；page size、prefix split、policy timing 变化后，
  source lock/ref 页集合不再是 target 不变量。
- **capacity / eviction exactness**：当前是 LRU-like 近似，没有完整 allocator / evictable / locked oracle。
- **完整 radix split**：当前用 lookup path、known prefix pages 和 operation-level removed pages 做最小模型，尚未重建完整
  target radix node split / merge / parent-child 结构。

## 复现入口

每个真实 profile 的基本流程如下：

```bash
scripts/profile.sh <profiling_config> --experiment <experiment_id>

python3 scripts/internal/profile_quality.py \
  --manifest <run_dir>/profile_manifest.json \
  --output <run_dir>/profile_quality.json
```

trace graph baseline 使用唯一的 replay 入口：

```bash
python3 scripts/internal/model_runner.py \
  --config configs/modeling/hicache/modeling_hicache_from_manifest.json \
  --profile-manifest <run_dir>/profile_manifest.json \
  --output-dir <run_dir>/modeling/faithful_replay \
  --mode faithful_replay \
  --emit-module-summary \
  --emit-validation
```

HiCache state 同场景验证使用 self-config prediction。`<self_config_modeling_config>` 必须显式给出同场景 target
page size、capacity、write policy 和 prefetch policy；不能省略 target config，也不能使用显式 observed policy。

```bash
python3 scripts/internal/model_runner.py \
  --config <self_config_modeling_config> \
  --profile-manifest <run_dir>/profile_manifest.json \
  --hicache-oracle-trace <target_oracle_trace.json> \
  --output-dir <run_dir>/modeling/<self_config_prediction_label> \
  --mode cache_state \
  --emit-module-summary \
  --emit-validation
```

跨配置 prediction 的基本流程如下：

```bash
python3 scripts/internal/model_runner.py \
  --config <prediction_modeling_config> \
  --profile-manifest <base_run_dir>/profile_manifest.json \
  --hicache-oracle-trace <target_oracle_trace.json> \
  --output-dir <base_run_dir>/modeling/<prediction_label> \
  --mode cache_state \
  --emit-module-summary \
  --emit-validation
```

`--hicache-oracle-trace` 只用于 validation oracle；模型输入仍来自 base manifest 对应的 base trace。

主线一 profiling 入口：

```bash
scripts/profile.sh configs/experiments/hicache_state/profiling_hicache_state_mainline_one_matrix.json --list-experiments
scripts/profile.sh configs/experiments/hicache_state/profiling_hicache_state_mainline_one_matrix.json --experiment s1a_manual
scripts/profile.sh configs/experiments/hicache_state/profiling_hicache_state_mainline_one_matrix.json --experiment s1b_manual
scripts/profile.sh configs/experiments/hicache_state/profiling_hicache_state_mainline_one_matrix.json --experiments s1a_bench,s1b_bench
```

当前只保留一个主线一 suite config。它展开为 `s1a_manual`、`s1a_bench`、`s1b_manual`、
`s1b_bench` 四个实验；其中 server 维度只有 `s1a` / `s1b`，input 维度只有 `manual` / `bench`。

## 下一步工作主线

### 主线一：两个强差异配置场景 + 两个大输入

目标：用两个全新的 HiCache 配置场景和两个大型输入验证 state model 的跨配置能力。两个场景不仅彼此完全不同，
也不能等同于任何此前已经跑过的 HiCache state profiling 配置组合。

当前候选配置：

| 场景 | joint signature | 当前状态 |
| --- | --- | --- |
| `S1A_baseline_large` | page128、L1/L2 capacity `64/145`、`write_through_selective`、`wait_complete`、ratio `2.25`、prefetch timeout extra config `8s/0/8s`。 | profiling quality 已通过；关闭旧兼容路径后需要重新跑 self-config prediction。 |
| `S1B_divergent_large` | page64、L1/L2 capacity `128/321`、`write_back`、`best_effort`、ratio `2.5`、prefetch timeout extra config `6s/0/6s`。 | profiling quality 已通过；self-config 无 non-invariant usage，但 final state 不对齐。 |

输入集合：

| input | 来源 | 规模要求 | 覆盖目的 |
| --- | --- | --- | --- |
| `L1_manual_phased` | `scripts/bench/hicache_phased_workload.py` | 比 I2/I3 更大；保留 deterministic phase、prompt seed 和可复用参数。 | 强压 lookup/insert/load/write/evict/prefetch/lock 的可控组合。 |
| `L2_bench_serving_large` | 大型 bench serving 模式 | 更接近真实 serving 流量；记录请求集、seed、并发、输入长度分布和输出长度分布。 | 验证模型在非手工 phase 流量下的 faithful replay 和 prediction 稳定性。 |

当前优先级：

1. 先修 `S1B self-config prediction`：它 `non_invariant_fact_usage=[]`，失败更集中在 write-back / prefetch / eviction 模型。
2. 再修 `S1A self-config prediction`：先用当前代码重跑，确认 lock/ref 已跳过且 `non_invariant_fact_usage=[]`，再看 L1/evicted mismatch。
3. 最后回到 `S1A -> S1B` 和 `S1B -> S1A` cross-config prediction。
4. `L1_manual_phased` 完成后再进入 `L2_bench_serving_large`。

验收：

- 两个 faithful replay 都必须满足 trace graph baseline 验收。
- 两个 self-config prediction 都必须 final state match、invariant coverage ready、无非法 non-invariant usage。
- 两个 cross-config prediction 都必须 final state match、invariant coverage ready、无非法 non-invariant usage。
- timeline 必须 `match=true` 且 `model_extra_transition_count=0`。
- event delta 只在 self-config prediction 中作为强诊断；跨配置 prediction 不要求 event key comparable。

### 主线二：用户手动 profiling 大矩阵

目标：用户负责手动完成大型真实 profiling 矩阵；Codex 不再启动这些 profile run，而是接收 manifests 和 config mapping，
只做 faithful replay、self-config prediction、cross-config prediction、validation 摘要和必要模型修复。

用户侧需要提供：

- profile manifest；
- workload 描述：input id、请求数、phase 或 serving 参数、seed、错误数；
- config id：HiCache page size、write policy、prefetch policy、capacity、timeout、ratio；
- target config 文件或等价 config snapshot；
- 是否作为 base facts、target oracle、self-config prediction 或 cross-config prediction；
- 已知运行异常，例如 server restart、request error、missing trace shard。

Codex 侧执行：

```text
profile_quality
self-config prediction
cross-config prediction
validation summary extraction
docs/validation/hicache_state_validation.md 更新
必要时修复模型并重跑 faithful replay / self-config prediction / cross-config prediction
```

### 主线三：构建更强 oracle，推进 timeline exact

当前 snapshot timeline 是 multiset coverage，不是严格 transition log。要验证真正 exact，需要新增 ordered transition oracle：

1. 在 probe 侧记录 operation-level transition。
   - 每个 state mutation 输出 operation id、request id、scope、page、kind、before/after 必要字段。
   - 记录顺序必须是调用内真实执行顺序，不能只依赖同 timestamp 排序。

2. 区分可观察 transient 和不可观察 transient。
   - write-through dirty transient、evicted mark/clear、host/device resident 切换需要明确是否可在 completed snapshot 看到。
   - oracle 要标记 transition 是 required、optional 还是 unobservable。

3. 对多进程 snapshot 做 ordered merge。
   - 当前 final oracle 使用 per-process final snapshot union；ordered oracle 需要保留 process id 和 object id。
   - 不能把一个进程中延迟暴露的状态变化误归因到另一个进程的 operation。

4. 将 validation 从 coverage 推进到 exact。
   - 第一阶段：对 full-mechanism input 做 ordered transition diff，但仍允许 documented optional transient。
   - 第二阶段：对 page64 和 low-capacity input 做 exact diff。
   - 第三阶段：把 exact oracle 用于 lock/ref attribution，关闭当前 page64 lock/ref event delta 边界。

## 新结果记录模板

```markdown
### HCSV-YYYYMMDD-<scope>

目的：

输入 / 配置：

运行摘要：

| prediction | final | invariant coverage | non-invariant usage | timeline | 结论 |
| --- | --- | --- | --- | --- | --- |

失败定位：

后续动作：
```

## 数据清理约束

本文只保留长期指标。清理生成物时允许删除：

```bash
rm -rf data/profile_runs data/modeling_runs data/traces
```

清理前必须确认：

- 本文或历史只读文档已写入 run label、配置、输入、关键计数和结论；
- 没有未归档的 final mismatch、profile quality 异常或模型修复原因；
- 需要提交的源码、配置、测试和文档变更仍在工作树中；
- 清理命令只删除生成数据，不删除 configs、docs、scripts、src、tests。
