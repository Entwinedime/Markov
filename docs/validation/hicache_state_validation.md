# HiCache State 验证记录

维护方式：本文记录 HiCache state validation 的当前规则、验证矩阵、实际结果、已知边界和下一步主线。
本文不依赖本地 `data/` 目录长期存在；profiling / modeling 产物在指标提取后可以清理。

## 记录规则

- 验证批次使用 `HCSV-YYYYMMDD-<scope>` 作为稳定编号；单次真实运行只保留 run label、配置名、命令入口和抽取后的指标。
- 文档不得把 `data/profile_runs/...`、`data/modeling_runs/...` 等本地产物路径作为长期证据入口。
- 需要复现时按本文命令重新运行，并把新的 profile quality、replay、prediction 摘要写回本文。
- 如果一次验证依赖多个真实 profiling run，文档只写 run label 和内联指标；run 目录可在文档更新后清理。
- `--hicache-ratio` 可以为验证需要调整，但必须 `> 1.0`；调整时要说明目的，capacity pressure 优先来自 workload 或显式 capacity config。
- commit message、验证记录和约束文档都应使用规范、可检索的措辞；提交前必须先完成 build/test/json/format/diff 检查。

## 当前结论

截至 `2026-06-09 05:48:03 +0800`，本轮验证基于 HEAD commit `43d303c`
和当前工作树改动。当前工作树包含四类 HiCache state / validation 修复：

- 显式 `write_back` target 默认不消费 base write-through 的 `L3->L2` prefetch transfer credit；只有组合配置显式设置
  `write_back_prefetch_transfer_credit=true` 时才使用该 credit。
- 显式 `best_effort` 或带 timeout 参数的 `timeout` target 不消费 base prefetch policy 下观测到的 lock/ref。
- page size what-if 下，prefetch transfer completion 只要 source pages 覆盖同 request 的 schedule source pages，
  completion credit 就归到 target schedule pages；不再要求 transfer 的 `target_page_identity` 与 schedule target pages 前缀相同。
- profiling runner 在 state_trace 开启时，会把同一次 insert start/end validation snapshot 中消失的 radix pages
  materialize 成 `hicache_insert_end.radix_removed_page_identity`。C++ state model 在同 page size replay 中消费这个
  operation-level fact，page-size what-if 下跳过它。

本轮重新跑完了 `I0`、`I1`、`I2`、`I3` 四类输入，以及 `C0-C7`、`write_back + low capacity`
共九类配置/组合的 state replay 或 cross-config prediction。结论如下：

- 所有列入当前矩阵的同配置 replay 和跨配置 prediction 都达到 `final_state_match=true`。
- 所有列入当前矩阵的 validation 都达到 `timeline match=true` 且 `model_extra_transition_count=0`。
- profile page identity coverage 全部满足 `stateful_required_events_missing_page_identity=0`。
- `l3_resident_pages` 仍没有 full-set oracle，因此继续作为 unchecked diagnostic。
- timeline `exact_match=false` 主要来自 oracle-only transient，例如 `mark_evicted` / `clear_evicted` 或 snapshot 粒度差异。
- `I2_page_split` 扩大了 page boundary 和 prefix overlap 后，暴露并修复了 page64 prediction 的 prefetch ready credit 缺口。
- `I2_page_split` 也再次确认 page size 变化下 lock/ref event delta 仍不是逐 event exact；final locked set 为 `0/0`。
- `I3_prefetch_timing` 是 suppressed-only timing 输入，故 profile quality 报缺 `prefetch_transfer`；该输入用于验证 timeout/suppressed
  终止语义，不用于证明 transfer-ready 覆盖。transfer-ready 覆盖由 `I0`、`I1`、`I2` 和 write-back capacity 组合承担。

## 验证目标

本阶段只验证一个问题：HiCache cache state 能否由 profiling 采集到的不变量事实和 target cache config 推导出来。
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

## 验证分层

| 层级 | 验证对象 | 当前用途 |
| --- | --- | --- |
| Summary 验证 | final page count、resident set、transition count | 快速发现明显错误，但不能单独作为验收。 |
| 逐 trace 验证 | request / operation / page / state transition | 本阶段主验收，用于定位 state 推导错误。 |
| Timeline coverage | raw snapshot timeline 的 kind/page multiset coverage | 验证模型没有预测 raw snapshot 完全无证据的额外 transition。 |
| E2E 验证 | 端到端预测时间 | 只作为后续 DAG patch 的旁路 sanity check。 |

当前验收口径：

- 同配置 replay 必须满足 final state match；full-mechanism 输入还应满足 event delta match 或记录 event delta 边界。
- 跨配置 prediction 必须满足 final state match、invariant coverage ready、无非法 non-invariant fact usage。
- page size、capacity、write policy、prefetch policy 这类 target config 变化，不能消费 target actual trace 作为模型输入。
- `timeline match=true` 表示模型 transition 全部被 raw snapshot timeline 覆盖。
- `timeline exact_match=false` 是诊断项，不等价于 state model 错误；需要结合 `model_extra_transition_count`、final state 和 mismatch kind 判断。

## 输入集合

| input id | workload 形态 | 本轮验证范围 | 覆盖重点 |
| --- | --- | ---: | --- |
| `I0_full_mechanism` | `65` requests：A seed/reuse/backup、`pressure_B=16`、C prefetch seed/reuse。 | 7 个 replay + 6 个 prediction | lookup、insert、load_back、evict、prefetch、write、lock 的基础覆盖。 |
| `I1_capacity_pressure` | `81` requests：同 I0，但 `pressure_B=32`。 | 3 个 replay + 2 个 prediction | clean eviction、低容量 L1/L2、write-back+capacity 组合。 |
| `I2_page_split` | `65` requests：`shared-prefix-repeat=192`、`unique-suffix-repeat=20`、`pressure_B=16`。 | base + page64 + prediction | page64 target identity、radix split、leaf group eviction、target suffix、prefetch completion rekey。 |
| `I3_prefetch_timing` | `43` requests：A 阶段缩小、`pressure_B=8`、`prefetch_seed=8`、`prefetch_reuse=10`、`phase_wait=4s`。 | base + aggressive timeout + prediction | timeout/suppressed timing、terminal progress、同输入分布跨 policy prediction。 |
| `I4_lock_overlap` | 未纳入本轮真实矩阵。 | 0 | 需要 workload 支持并发/重叠窗口；当前顺序 workload 不能真实制造 lock/ref 与 eviction/prefetch 交叠。 |

I3 的请求数经过缩减，目的是把 prefetch timing 输入保持在可重复运行的耗时内；两个 I3 配置使用完全相同的 workload command，
只改变 target timeout 配置。

## 配置集合

| config id | profiling config | cache 配置 | 验证重点 |
| --- | --- | --- | --- |
| `C0_base_timeout` | `profiling_hicache_state_validation.json` | page128、write-through、timeout | 基线 replay 和 cross-config base facts。 |
| `C1_write_back` | `profiling_hicache_state_write_back_validation.json` | page128、write-back、timeout | dirty、writeback、eviction 后 L2/backuped 对齐。 |
| `C2_write_through_selective` | `profiling_hicache_state_write_through_selective_validation.json` | page128、write-through-selective、timeout | hit_count、selective write、scope 隔离。 |
| `C3_page64` | `profiling_hicache_state_page64_validation.json` | page64、write-through、timeout | target page identity、radix split、lock/ref 边界。 |
| `C4_capacity` | `profiling_hicache_state_capacity_validation.json` | page128、write-through、timeout、low capacity | clean eviction、capacity audit。 |
| `C5_prefetch_wait` | `profiling_hicache_state_prefetch_wait_validation.json` | page128、write-through、wait_complete | planned/ready/suppressed 语义。 |
| `C6_prefetch_best_effort` | `profiling_hicache_state_prefetch_best_effort_validation.json` | page128、write-through、best_effort | best_effort terminate 和 ready/suppressed 语义。 |
| `C7_prefetch_timeout_aggressive` | `profiling_hicache_state_prefetch_timeout_aggressive_validation.json` | page128、write-through、timeout + explicit timeout params | aggressive timeout suppressed/ready 优先级。 |
| `C8_write_back_capacity` | `profiling_hicache_state_write_back_capacity_validation.json` | page128、write-back、timeout、low capacity | dirty eviction + write-back + low capacity + prefetch transfer credit。 |

## 复现入口

每个真实 profile 的基本流程如下：

```bash
scripts/profile.sh <profiling_config>

python3 scripts/internal/profile_quality.py \
  --manifest <run_dir>/profile_manifest.json \
  --output <run_dir>/profile_quality.json

python3 scripts/internal/model_runner.py \
  --config configs/modeling/hicache_state/modeling_hicache_state_validation.json \
  --profile-manifest <run_dir>/profile_manifest.json \
  --output-dir <run_dir>/modeling/cache_state_replay \
  --mode cache_state \
  --emit-module-summary \
  --emit-validation
```

跨配置 prediction 的基本流程如下：

```bash
python3 scripts/internal/model_runner.py \
  --config <prediction_modeling_config> \
  --profile-manifest <base_run_dir>/profile_manifest.json \
  --hicache-oracle-trace <target_replay_dir>/merged_trace/merged_trace_00.json \
  --output-dir <base_run_dir>/modeling/<prediction_label> \
  --mode cache_state \
  --emit-module-summary \
  --emit-validation
```

`--hicache-oracle-trace` 只用于 validation oracle；模型输入仍来自 base manifest 对应的 base trace。

## HCSV-20260608-state-matrix

本批次重新运行并记录以下 run label。run label 只是批次标签，不是长期路径依赖。

| run label | input | config | workload |
| --- | --- | --- | --- |
| `20260608_052627_profiling_hicache_state_validation` | `I0` | `C0_base_timeout` | `65/65 ok` |
| `20260608_054834_profiling_hicache_state_write_back_validation` | `I0` | `C1_write_back` | `65/65 ok` |
| `20260608_061634_profiling_hicache_state_write_through_selective_validation` | `I0` | `C2_write_through_selective` | `65/65 ok` |
| `20260608_064000_profiling_hicache_state_page64_validation` | `I0` | `C3_page64` | `65/65 ok` |
| `20260608_090111_profiling_hicache_state_prefetch_wait_validation` | `I0` | `C5_prefetch_wait` | `65/65 ok` |
| `20260608_092411_profiling_hicache_state_prefetch_best_effort_validation` | `I0` | `C6_prefetch_best_effort` | `65/65 ok` |
| `20260608_095511_profiling_hicache_state_prefetch_timeout_aggressive_validation` | `I0` | `C7_prefetch_timeout_aggressive` | `65/65 ok` |
| `20260608_072559_profiling_hicache_state_capacity_base_validation` | `I1` | `C0_capacity_base` | `81/81 ok` |
| `20260608_080550_profiling_hicache_state_capacity_validation` | `I1` | `C4_capacity` | `81/81 ok` |
| `20260608_102444_profiling_hicache_state_write_back_capacity_validation` | `I1` | `C8_write_back_capacity` | `81/81 ok` |
| `20260608_111412_profiling_hicache_state_i2_page_split_base_validation` | `I2` | `C0_base_timeout` | `65/65 ok` |
| `20260608_114903_profiling_hicache_state_i2_page_split_page64_validation` | `I2` | `C3_page64` | `65/65 ok` |
| `20260608_123956_profiling_hicache_state_i3_prefetch_timing_base_validation` | `I3` | `C0_base_timeout` | `43/43 ok` |
| `20260608_125130_profiling_hicache_state_i3_prefetch_timing_timeout_aggressive_validation` | `I3` | `C7_prefetch_timeout_aggressive` | `43/43 ok` |

### Profile Quality 摘要

| input/config | `quality_ready` | missing mechanisms | observed mechanisms 摘要 | page identity 缺口 |
| --- | --- | --- | --- | ---: |
| `I0/C0` | `true` | `[]` | lookup `396`、insert `264`、load `138`、evict `38`、lock `872`、prefetch transfer `2`、write `108/108` | `0` |
| `I0/C1` | `true` | `[]` | lookup `396`、insert `264`、load `138`、evict `26`、lock `800`、prefetch transfer `0`、write `78/78` | `0` |
| `I0/C2` | `true` | `[]` | lookup `396`、insert `264`、load `138`、evict `38`、lock `872`、prefetch transfer `2`、write `108/108` | `0` |
| `I0/C3` | `true` | `[]` | lookup `396`、insert `264`、load `140`、evict `74`、lock `960`、prefetch transfer `2`、write `242/240` | `0` |
| `I0/C5` | `true` | `[]` | lookup `396`、insert `264`、load `138`、evict `38`、lock `872`、prefetch transfer `2`、write `108/108` | `0` |
| `I0/C6` | `true` | `[]` | lookup `396`、insert `264`、load `132`、evict `38`、lock `868`、prefetch transfer `2`、write `114/114` | `0` |
| `I0/C7` | `true` | `[]` | lookup `396`、insert `264`、load `138`、evict `38`、lock `876`、prefetch transfer `2`、write `114/114` | `0` |
| `I1/C0_capacity_base` | `true` | `[]` | lookup `492`、insert `328`、load `170`、evict `102`、lock `1128`、prefetch transfer `2`、write `204/204` | `0` |
| `I1/C4` | `true` | `[]` | lookup `492`、insert `328`、load `170`、evict `110`、lock `1128`、prefetch transfer `2`、write `204/204` | `0` |
| `I1/C8` | `true` | `[]` | lookup `492`、insert `328`、load `170`、evict `104`、lock `992`、prefetch transfer `2`、write `192/180` | `0` |
| `I2/C0` | `true` | `[]` | lookup `396`、insert `264`、load `178`、evict `88`、lock `988`、prefetch transfer `16`、write `198/198` | `0` |
| `I2/C3` | `true` | `[]` | lookup `396`、insert `264`、load `180`、evict `88`、lock `988`、prefetch transfer `16`、write `198/198` | `0` |
| `I3/C0` | `false` | `prefetch_transfer` | lookup `264`、insert `176`、load `94`、evict `8`、lock `576`、prefetch transfer `0`、write `60/60` | `0` |
| `I3/C7` | `false` | `prefetch_transfer` | lookup `264`、insert `176`、load `94`、evict `8`、lock `576`、prefetch transfer `0`、write `60/60` | `0` |

I3 的 `quality_ready=false` 是本输入的覆盖范围声明：该输入没有产生 transfer-ready evidence，只验证 planned/suppressed
timing；这不作为 `I3` replay/prediction 失败处理。

### 同配置 Replay 摘要

| replay | final | state counts | event delta | timeline |
| --- | --- | --- | --- | --- |
| `I0/C0` | pass | L1 `56/56`、L2 `121/121`、dirty `0/0`、backuped `121/121`、evicted `65/65`、prefetch `166/8/158` | match `true`、shared `221`、mismatch `0` | match `true`、exact `false`、oracle-only `16` |
| `I0/C1` | pass | L1 `56/56`、L2 `118/118`、dirty `48/48`、backuped `118/118`、evicted `110/110`、prefetch `166/0/166` | match `true`、shared `242`、mismatch `0` | match `true`、exact `false`、oracle-only `244` |
| `I0/C2` | pass | L1 `56/56`、L2 `121/121`、dirty `0/0`、backuped `121/121`、evicted `65/65`、prefetch `166/8/158` | match `true`、shared `241`、mismatch `0` | match `true`、exact `false`、oracle-only `348` |
| `I0/C3` | pass | L1 `111/111`、L2 `250/250`、dirty `0/0`、backuped `250/250`、evicted `139/139`、prefetch `364/17/347` | match `false`、shared `202`、mismatch `118` | match `true`、exact `false`、oracle-only `1570` |
| `I0/C5` | pass | L1 `56/56`、L2 `121/121`、dirty `0/0`、backuped `121/121`、evicted `65/65`、prefetch `166/8/158` | match `true`、shared `224`、mismatch `0` | match `true`、exact `false`、oracle-only `16` |
| `I0/C6` | pass | L1 `56/56`、L2 `121/121`、dirty `0/0`、backuped `121/121`、evicted `65/65`、prefetch `166/8/158` | match `true`、shared `220`、mismatch `0` | match `true`、exact `true`、oracle-only `0` |
| `I0/C7` | pass | L1 `56/56`、L2 `121/121`、dirty `0/0`、backuped `121/121`、evicted `65/65`、prefetch `166/8/158` | match `false`、shared `220`、mismatch `29` | match `true`、exact `false`、oracle-only `90` |
| `I1/C0_capacity_base` | pass | L1 `56/56`、L2 `126/126`、dirty `0/0`、backuped `126/126`、evicted `70/70`、prefetch `326/8/318` | match `true`、shared `254`、mismatch `0` | match `true`、exact `false`、oracle-only `16` |
| `I1/C4` | pass | L1 `46/46`、L2 `96/96`、dirty `0/0`、backuped `96/96`、evicted `50/50`、prefetch `326/8/318` | match `true`、shared `255`、mismatch `0` | match `true`、exact `false`、oracle-only `16` |
| `I1/C8` | pass | L1 `46/46`、L2 `88/88`、dirty `38/38`、backuped `88/88`、evicted `80/80`、prefetch `326/8/318` | match `true`、shared `288`、mismatch `0` | match `true`、exact `false`、oracle-only `592` |
| `I2/C0` | pass | L1 `56/56`、L2 `120/120`、dirty `0/0`、backuped `120/120`、evicted `64/64`、prefetch `276/20/256` | match `false`、shared `216`、mismatch `132` | match `true`、exact `false`、oracle-only `1888` |
| `I2/C3` | pass | L1 `112/112`、L2 `240/240`、dirty `0/0`、backuped `240/240`、evicted `128/128`、prefetch `562/40/522` | match `false`、shared `215`、mismatch `131` | match `true`、exact `false`、oracle-only `3776` |
| `I3/C0` | pass | L1 `61/61`、L2 `88/88`、dirty `0/0`、backuped `88/88`、evicted `27/27`、prefetch `88/0/88` | match `true`、shared `152`、mismatch `0` | match `true`、exact `false`、oracle-only `8` |
| `I3/C7` | pass | L1 `61/61`、L2 `88/88`、dirty `0/0`、backuped `88/88`、evicted `27/27`、prefetch `88/0/88` | match `true`、shared `152`、mismatch `0` | match `true`、exact `false`、oracle-only `8` |

### 跨配置 Prediction 摘要

| prediction | final | skipped non-invariant | state counts | timeline |
| --- | --- | ---: | --- | --- |
| `I0 C0 -> C1_write_back` | pass | `1488` | L1 `56/56`、L2 `118/118`、dirty `48/48`、backuped `118/118`、evicted `110/110`、prefetch `166/0/166` | match `true`、exact `false`、oracle-only `8` |
| `I0 C0 -> C2_write_through_selective` | pass | `424` | L1 `56/56`、L2 `121/121`、dirty `0/0`、backuped `121/121`、evicted `65/65`、prefetch `166/8/158` | match `true`、exact `false`、oracle-only `364` |
| `I0 C0 -> C3_page64` | pass | `1340` | L1 `111/111`、L2 `250/250`、dirty `0/0`、backuped `250/250`、evicted `139/139`、prefetch `364/17/347` | match `true`、exact `false`、oracle-only `34` |
| `I0 C0 -> C5_prefetch_wait` | pass | `426` | L1 `56/56`、L2 `121/121`、dirty `0/0`、backuped `121/121`、evicted `65/65`、prefetch `166/8/158` | match `true`、exact `false`、oracle-only `32` |
| `I0 C0 -> C6_prefetch_best_effort` | pass | `1296` | L1 `56/56`、L2 `121/121`、dirty `0/0`、backuped `121/121`、evicted `65/65`、prefetch `166/8/158` | match `true`、exact `true`、oracle-only `0` |
| `I0 C0 -> C7_prefetch_timeout_aggressive` | pass | `1296` | L1 `56/56`、L2 `121/121`、dirty `0/0`、backuped `121/121`、evicted `65/65`、prefetch `166/8/158` | match `true`、exact `false`、oracle-only `6` |
| `I1 C0_capacity_base -> C4_capacity` | pass | `1014` | L1 `46/46`、L2 `96/96`、dirty `0/0`、backuped `96/96`、evicted `50/50`、prefetch `326/8/318` | match `true`、exact `false`、oracle-only `32` |
| `I1 C0_capacity_base -> C8_write_back_capacity` | pass | `2142` | L1 `46/46`、L2 `88/88`、dirty `38/38`、backuped `88/88`、evicted `80/80`、prefetch `326/8/318` | match `true`、exact `false`、oracle-only `16` |
| `I2 C0 -> C3_page64` | pass | `1814` | L1 `112/112`、L2 `240/240`、dirty `0/0`、backuped `240/240`、evicted `128/128`、prefetch `562/40/522` | match `true`、exact `false`、oracle-only `80` |
| `I3 C0 -> C7_prefetch_timeout_aggressive` | pass | `824` | L1 `61/61`、L2 `88/88`、dirty `0/0`、backuped `88/88`、evicted `27/27`、prefetch `88/0/88` | match `true`、exact `false`、oracle-only `8` |

Cross-config event delta 使用不同真实 run 的 trace，因此 event key 不可比；表中以 final state、invariant coverage、non-invariant usage
和 timeline coverage 为主。

## 能力矩阵

| 能力 | 当前证据 | 结论 |
| --- | --- | --- |
| base 同配置 replay | `I0/C0` replay final、event、timeline coverage 全部通过。 | 已验证。 |
| write-through -> write-back prediction | `I0 C0 -> C1` final 对齐，dirty `48/48`。 | 已验证。 |
| write-back target replay | `I0/C1` replay final 对齐，dirty `48/48`。 | 已验证。 |
| write-through-selective prediction | `I0 C0 -> C2` final 对齐，prefetch `166/8/158`。 | 已验证。 |
| page64 target replay | `I0/C3` 和 `I2/C3` replay final 对齐。 | 已验证；event delta lock/ref 边界保留。 |
| page64 strict prediction | `I0 C0 -> C3`、`I2 C0 -> C3` final 对齐，timeline coverage 通过。 | 已验证；I2 暴露并修复 transfer completion rekey。 |
| capacity prediction | `I1 C0 -> C4` final 对齐，L1 `46/46`、L2 `96/96`。 | 已验证。 |
| write-back + low capacity prediction | `I1 C0 -> C8` final 对齐，dirty `38/38`、ready `8/8`。 | 已验证；需要 `write_back_prefetch_transfer_credit=true`。 |
| prefetch wait prediction | `I0 C0 -> C5` final 对齐，ready `8/8`。 | 已验证。 |
| prefetch best_effort prediction | `I0 C0 -> C6` final 对齐，timeline exact `true`。 | 已验证。 |
| aggressive timeout prediction | `I0 C0 -> C7` 与 `I3 C0 -> C7` final 对齐。 | 已验证。 |
| lock-enabled replay | `I0/C0`、`I1`、`I2`、`I3` 均有 lock_ref 覆盖，final locked `0/0`。 | final state 已验证。 |
| page size lock/ref event attribution | `I0/C3`、`I2/C0`、`I2/C3` event delta mismatch 均集中在 lock/ref。 | 已知边界，不作为 final state failure。 |
| timeline coverage | 所有当前 replay/prediction `timeline match=true` 且 `model_extra_transition_count=0`。 | 已验证。 |
| timeline exact | 多数 run `exact_match=false`，主要为 oracle-only transient。 | 尚未作为验收闭环，需要更强 oracle。 |
| I4 lock overlap | 当前 workload 顺序执行，不能制造真实并发 lock/ref overlap。 | 后续工作，需要 workload 支持。 |

## 关键修复与原因

### 1. Aggressive Timeout Lock/Ref 非不变量

问题：显式 `best_effort` 或 aggressive `timeout` 会改变 prefetch 终止点。base policy 下观测到的 prefetch lock/ref
start/end 对数不是 target timeline 不变量。

修复：在 `should_skip_non_invariant_target_movement` 中，对 `best_effort` 和带 timeout 参数的 `timeout`
target 跳过 base lock/ref movement。fixture 覆盖：

- `run_prefetch_timeout_lock_ref_non_invariant_fixture`
- `run_prefetch_best_effort_lock_ref_non_invariant_fixture`

结果：`I0 C0 -> C7` final state 对齐，剩余 timeline mismatch 只有 oracle-only `mark_evicted=3`、`clear_evicted=3`。

### 2. Write-Back + Low Capacity Transfer Credit

问题：普通 `write_through -> write_back` prediction 不能消费 base write-through 下的 L3->L2 prefetch transfer；
但 `write_back + low capacity` target 需要 transfer ready evidence，否则后续 lookup/eviction 会把
dirty、evicted、prefetch ready 集合推偏。

修复：新增显式配置项：

```json
"write_back_prefetch_transfer_credit": true
```

默认值为 `false`。只有 `C8_write_back_capacity` 启用。fixture 覆盖：

- `run_prefetch_write_back_transfer_non_invariant_fixture`
- `run_prefetch_write_back_capacity_transfer_credit_fixture`

结果：

- `I0 C0 -> C1` 仍跳过 transfer，dirty `48/48`、ready `0/0`。
- `I1 C0 -> C8` 使用 transfer credit，dirty `38/38`、evicted `80/80`、ready `8/8`。

### 3. Page64 Prefetch Completion Rekey

问题：`I2 C0 -> C3_page64` 首次 prediction 失败，resident/dirty/evicted 均对齐，但 prefetch ready `26/40`、
suppressed `536/522`。缺的 14 页来自后续 2-page transfer completion。

原因：page size what-if 下，base transfer 的 `target_page_identity` 可能因为 base prefetch 的 last_hash 或 parent context
与 target schedule identity 不一致。旧逻辑要求 transfer target pages 是 schedule target pages 的前缀，并且 schedule 页数大于
transfer 页数；I2 中后续 transfer 页数相等但 identity 不同，因此没有把 completion credit 归到 target schedule pages。

修复：只要 transfer source pages 已覆盖同 request 的 schedule source pages，就用 target schedule pages 作为 completion credit。
新增 fixture 覆盖等长但 target identity 不一致的场景。

结果：`I2 C0 -> C3_page64` 重跑后 ready `40/40`、suppressed `522/522`，final state 和 timeline coverage 通过。

## Event Delta 边界

event delta validation 当前口径：

- inclusive oracle 保留每个 start/end snapshot 的包围差分，用于观察真实调用包含了哪些状态变化。
- exclusive oracle 只比较没有嵌套 state snapshot 的调用，避免把外层函数包含的内层 HiCache 状态变化误判成模型归因错误。
- mismatch 只在 predicted 和 oracle 都有 shared exclusive event key 时比较 page delta。
- 跨配置 prediction 因 base run 和 target run 时间戳不同，event key 不可比，仍以 final state 和 transition coverage 为主。
- `locked_pages` 只有在模型也输出 lock transition 时才参与 event delta 比较，否则作为 ignored state key 暴露。

当前 event delta 的真实边界：

- `I0/C3_page64` replay：event mismatch `118`。
- `I2/C0` replay：event mismatch `132`。
- `I2/C3_page64` replay：event mismatch `131`。

这些 mismatch 集中在 lock/ref attribution：SGLang lock/ref 会沿当前 radix tree 父链更新，page size 变化和更复杂 prefix overlap
会改变父链归因。当前模型能闭合 final locked set，但还没有逐 operation exact 归因 oracle。

## Timeline Exact 状态

所有当前 validation 都满足：

```text
timeline match=true
model_extra_transition_count=0
```

这说明模型没有预测 raw snapshot timeline 完全没有证据的额外 transition。

`exact_match=false` 的主要来源：

- raw snapshot 对同一 object 不是完整状态日志，而是稀疏采样；
- 多进程场景下，某个 cache object 长时间未被采样时，下一次任意 HiCache 调用的 snapshot 可能暴露之前已经发生的状态变化；
- oracle-only `mark_evicted` / `clear_evicted` transient 常见，尤其是 replay 中的 L1/L2 eviction 中间态；
- write-through 的 dirty transient 可能在 completed snapshot 中不可见；
- page64 lock/ref 父链归因无法用当前 snapshot/probe 粒度逐 event exact 对齐。

因此，当前 timeline exact 的判断是：

- `exact_match=false` 不是直接的 state model 错误。
- 如果 `model_extra_transition_count=0`、final state 对齐、缺口集中在 oracle-only transient，则优先归为 oracle 粒度问题。
- 真正要验证 operation-level exact，需要新增 ordered transition oracle，而不是继续扩大当前 snapshot multiset 比较。

## 建模契约

### 模型输入

| 类型 | 来源 | 用途 | 约束 |
| --- | --- | --- | --- |
| profiling 不变量事实 | base profiling / merged trace | 预测 target state | cache 配置变化后仍成立。 |
| target cache config | modeling config 或 target experiment config | 控制 page size、capacity、write/prefetch policy | 只能来自配置，不能从 target actual trace 偷读。 |
| oracle facts | 同配置或 target run 的 state snapshot / validation trace | replay / prediction 对比 | 只能用于 validation，不能参与 what-if state 推导。 |

### 状态集合

| 状态 | 含义 |
| --- | --- |
| `radix_tree` | target page size 下的 radix tree，维护 prefix、split、node/page 映射。 |
| `l1_resident_pages` | device KV cache 中当前 resident pages。 |
| `l2_resident_pages` | host KV cache 中当前 resident pages。 |
| `l3_evidence_pages` | storage 中被写入、查询命中或 transfer 完成的 evidence pages；不要求枚举全量 L3。 |
| `dirty_pages` | 已生成或修改，但尚未观察到备份的 pages。 |
| `backuped_pages` | 已备份到 L2 或 L3 的 pages。 |
| `evicted_pages` | 被从 L1 或 L2 释放的 pages。 |
| `locked_pages` | 被 request、prefetch、backup 或 load 保护，不能被 eviction 选择的 pages。 |
| `touch_order` | 用于 LRU-like eviction 的访问顺序。 |
| `prefetch_planned_pages` | policy 计划预取的 pages。 |
| `prefetch_ready_pages` | 已完成并可用于后续 hit 的 prefetch pages。 |
| `prefetch_late_pages` | 计划过但未赶上使用点的 prefetch pages。 |
| `prefetch_suppressed_pages` | 被 policy、capacity、timeout 或终止证据归入 suppressed 的 pages。 |

### 状态转移要求

| transition | 建模要求 |
| --- | --- |
| `lookup` | 根据 target page size 重新计算 page set，在 target radix tree 和 resident sets 中判断 L1/L2/L3/miss，并更新 touch order / hit count。 |
| `insert` | 将新生成 KV page 插入 target radix tree，标记 L1 resident，并根据 write policy 标记 dirty 或触发 write。 |
| `load` | L2 hit 时生成 L2->L1 load；L3 hit 且需要 foreground load 时生成 L3->L2，再生成 L2->L1。 |
| `prefetch` | 根据 target prefetch policy、threshold、timeout、capacity 判断 planned/ready/late/suppressed；ready 必须有 progress 或 transfer completion 证据。 |
| `write` | `write_through` 触发 L1->L2/L3；`write_through_selective` 依赖 hit count / threshold；`write_back` 到 eviction/flush 才写回。 |
| `evict` | 根据 capacity 和 locked state 选候选；dirty eviction 必须先 writeback，再释放 resident。 |
| `lock_ref` | 按 scope+page 维护 lock ref count；root/no-op 事件不能要求 page identity。 |

### Profiling 事实边界

Profiling 必须只采事实，不做 target 行为推断。

必须采集的不变量事实包括：

- request 顺序、phase、timestamp；
- canonical token path、prefix token ids 或等价 hash-chain 输入；
- page size、prior hash、hash chain 输入；
- operation id、target id、event role、timestamp；
- lookup key、request id、prefix scope；
- insert input、inserted key/value token length、priority、chunked 信息；
- write policy input、hit count、backuped、dirty、threshold；
- prefetch new input tokens、last hash、threshold、timeout、request 使用点；
- storage query hashes、batch hit count、transfer completion、write hashes；
- L1/L2 capacity、allocation size、locked/evictable 状态；
- insert 调用内 radix 结构消失的 page identity，例如 `radix_removed_page_identity`。该字段只能来自同一次调用
  start/end 的 radix state delta；完整 state snapshot 仍只作为 validation oracle，不直接喂给 target what-if。
- DAG anchor facts，用于后续 state-to-DAG patch。

只能用于 oracle 的字段包括：

- base page count / base page identity；
- SGLang radix node id；
- base hit/miss 结果；
- base load_back / prefetch / write 次数；
- base final resident set；
- SGLang state snapshot。

缺关键事实必须暴露为 `missing_invariant_facts`。page size 变化时，base `load_back`、write、transfer、
remove/evict 等 observed movement 不是 target 不变量；缺少 target page identity 时必须跳过并计入
`skipped_non_invariant_events`，不能静默更新 target state。

## 下一步工作主线

后续工作拆成三条平级主线。主线一先用两个强差异 HiCache 配置场景和两个大输入验证 state model；
主线二再接收用户手动完成的大型 profiling 矩阵，只做 simulation / replay / prediction；
原 timeline exact 主线顺延为主线三。

### 主线一：两个强差异配置场景 + 两个大输入

目标：先用最朴素但约束清晰的办法验证 state model 的跨配置能力。构建两个全新的 HiCache 配置场景：
它们不仅彼此完全不同，也不能等同于任何此前已经跑过的 HiCache state profiling 配置组合。这里的“此前已经跑过”
覆盖本文当前矩阵、已清理的历史运行、临时验证运行和失败后重跑前的草案配置；不能只按 `C0-C8` 编号集合判断。
每个可变配置项都尽量不同，然后用两个大型输入分别做 replay 和双向 cross-config prediction。

#### 配置场景

| 场景 | 配置要求 | 覆盖目的 |
| --- | --- | --- |
| `S1A_baseline_large` | 新建一套稳定基线；page size、write policy、prefetch policy、capacity、timeout、ratio 等联合配置不得复用任何历史已跑配置，包括但不限于 C0-C8、write-back capacity、page64、capacity、prefetch 变体、临时验证 run 和已清理 run。 | 提供可复用 base facts，同时验证新的 baseline 组合是否仍能稳定 replay。 |
| `S1B_divergent_large` | 与 `S1A` 的可变项尽量全部不同，并且同样不得复用任何历史已跑配置组合；例如 page size、write policy、prefetch policy、capacity、timeout、ratio 至少应形成一个新的联合配置。 | 强制触发 page split、dirty/writeback、capacity eviction、prefetch ready/suppressed 等 target 行为变化，同时验证未见过配置组合的 target 行为。 |

当前候选配置签名：

| 场景 | joint signature | 历史配置比对 |
| --- | --- | --- |
| `S1A_baseline_large` | page128、L1/L2 capacity `64/145`、`write_through_selective`、`wait_complete`、ratio `2.25`、prefetch timeout extra config `8s/0/8s`。 | 不同于 `C2_write_through_selective`，因为 prefetch policy、显式 capacity、ratio 和 timeout extra config 不同；不同于 `C5_prefetch_wait`，因为 write policy、显式 capacity、ratio 和 timeout extra config 不同；也不同于早期主线一 S1A 2.0 草案签名。脚本化扫描当前仓库可查的非主线一 HiCache state 实验配置，并额外比对 HCSV 固化的历史签名黑名单，`old_matches=0`。 |
| `S1B_divergent_large` | page64、L1/L2 capacity `128/321`、`write_back`、`best_effort`、ratio `2.5`、prefetch timeout extra config `6s/0/6s`。 | 不同于 `C3_page64` / `I2_page64`，因为 write policy、prefetch policy、ratio 和 timeout extra config 不同；不同于 `C1_write_back` / `C8_write_back_capacity`，因为 page size、capacity、prefetch policy、ratio 和 timeout extra config 不同；也不同于早期主线一 S1B 2.0 草案签名。脚本化扫描当前仓库可查的非主线一 HiCache state 实验配置，并额外比对 HCSV 固化的历史签名黑名单，`old_matches=0`。 |

本次签名检查按 page size、L1/L2 capacity、write policy、prefetch policy、`--hicache-ratio`、prefetch extra config
组成联合 signature；`S1A` 和 `S1B` 的联合 signature 也互不相同。ratio 分别使用 `2.25` 和 `2.5`，均满足
`> 1.0` 约束，目的是让主线一候选配置同时区别于当前矩阵、已清理历史运行、临时验证运行和早期主线一草案，同时避免
ratio `1.5` 这类低 ratio 草案造成的真实 profile 严重慢速。
`tests/run_hicache_mainline_config_fixtures.py` 同时扫描当前仓库非主线一配置，并内置 HCSV 历史签名黑名单；
已清理且仓库中没有实体配置的历史 run 只保留签名摘要，不把缺失的 `data/` 记录作为文档依赖。

#### 早期 S1A 手工输入排查

ratio `2.0` 的 `S1A_baseline_large + L1_manual_phased` 已完成一次真实 profile 和 replay 排查。它用于定位
radix removed materialization、observed replay config 和 prediction target config 的口径差异；由于主线一候选签名现在已经更新，
该批次不作为新主线一完成证据。结论如下：

| 项 | 结果 |
| --- | --- |
| run label | `20260608_203828_profiling_hicache_state_mainline_one_manual_s1a`。run label 只标识本批次，不作为长期路径依赖。 |
| workload | `83/83 ok`、`errors=0`；各阶段为 warmup `1`、seed `8`、reuse `8`、backup wait `8`、pressure `24`、reuse-after-pressure `8`、prefetch seed `8`、prefetch reuse `10`、dirty eviction `8`。 |
| profile quality | `quality_ready=true`，`quality_errors=[]`，23/23 Python targets observed，stateful required page identity `2130/2130`，缺失 `0`。 |
| 机制覆盖 | evict `122`、insert `336`、load_back `216`、lock_ref `1266`、lookup `504`、prefetch_decision `168`、prefetch_progress `424`、prefetch_query `114`、prefetch_schedule `282`、prefetch_transfer `16`、write_backup `294`、write_storage `288`。 |
| radix removed materialization | 真实 live source 仍未稳定产出非空字段；runner 收尾从同一次 insert start/end validation snapshot materialize `2` 个 `hicache_insert_end`，每个 `13` 页，合计 `26` 页。materialization 后 insert end 统计为 `336` total、`2` nonempty、`max_len=13`。 |
| 同配置 replay 口径 | 使用 observed replay config，不使用主线一 prediction target config。原因是 prediction config 中的显式 capacity 表示 what-if target，会跳过 observed remove/evict movement；同配置 replay 必须消费真实 observed movement。 |
| observed replay final | `validation_ready=true`、`validation_errors=[]`、`final_state_match=true`；model/oracle 均为 L1 `54`、L2 `106`、backuped `106`、evicted `52`、locked `1`、prefetch planned/ready/suppressed `356/18/338`。 |
| observed replay timeline | `match=true`、`model_extra_transition_count=0`、`exact_match=false`、`oracle_extra_transition_count=2946`。 |
| event delta | `match=false`、`mismatch_count=168`，剩余差异只集中在 lock/ref transition 粒度：`mark_locked` / `clear_locked` oracle-only pages。 |

本次 S1A 预验证补充了两条约束：

- `radix_removed_page_identity` 是 operation-level fact，可以由 live extractor 直接产出，也可以在 profiling runner 收尾阶段从同一次
  insert start/end validation snapshot materialize；完整 state snapshot 仍不作为 C++ state model 输入。
- 同配置 replay 和跨配置 prediction 必须使用不同 config 口径。replay 使用 observed config 验证事实采集和状态重放；
  prediction 才使用 `S1A` / `S1B` target config 验证 what-if 行为。

约束：

- `S1A` 和 `S1B` 的 request 内容必须一致，只改变 HiCache config。
- `S1A` 和 `S1B` 必须拥有新的 config id 和完整 config snapshot；不能只把旧配置改名当作新配置。
- 判定“新配置”时按联合配置判断：page size、write policy、prefetch policy、capacity、timeout、ratio 等核心项的组合不能和任何已跑配置一致。
- 主线一开跑前必须把候选 `S1A` / `S1B` 与历史已跑配置清单做一次人工或脚本化比对；比对结果写入 config metadata 或 HCSV 摘要。
- `--hicache-ratio` 可以变化，但必须 `> 1.0`，并在 config metadata 或 HCSV 记录中说明原因。
- capacity pressure 优先来自 workload 或显式 capacity，不用 `<=1.0` ratio 构造。
- 如果某个可变项因为 SGLang 限制不能同时改变，必须在 HCSV 记录里说明保留原因。

#### 输入集合

| input | 来源 | 规模要求 | 覆盖目的 |
| --- | --- | --- | --- |
| `L1_manual_phased` | 当前 Python 手工 workload，即 `scripts/bench/hicache_phased_workload.py`。 | 比 I2/I3 更大；保留 deterministic phase、prompt seed 和可复用参数。 | 强压 lookup/insert/load/write/evict/prefetch/lock 的可控组合。 |
| `L2_bench_serving_large` | 大型 bench serving 模式。 | 比手工 phased 更接近真实 serving 流量；需要记录请求集、seed、并发、输入长度分布和输出长度分布。 | 验证模型在非手工 phase 流量下的 replay 和 prediction 稳定性。 |

#### 验证流程

对每个 input，都执行四步：

```text
input + S1A profile -> replay(S1A)
input + S1B profile -> replay(S1B)
S1A facts + S1B config + S1B oracle -> prediction(S1A -> S1B)
S1B facts + S1A config + S1A oracle -> prediction(S1B -> S1A)
```

配置口径：

- 同配置 replay 使用 observed replay config，用来验证当前 trace 事实能否重放真实 state；它不把显式 target capacity/policy 当成
  what-if 分支。
- 跨配置 prediction 使用 `configs/modeling/hicache_state/mainline_one/` 下的 S1A/S1B target config；这些配置中的
  page size、capacity、write policy、prefetch policy 是 what-if target，不能拿来替代同配置 replay config。
- 如果同一 profiling run 同时要做 replay 和 prediction，必须分别记录两个 modeling config 和两个 validation 结论。

验收：

- 两个 replay 都必须 final state match。
- 两个 prediction 都必须 final state match、invariant coverage ready、无非法 non-invariant usage。
- timeline 必须 `match=true` 且 `model_extra_transition_count=0`。
- event delta 只在同配置 replay 中作为强诊断；跨配置 prediction 不要求 event key comparable。
- 如果 `L2_bench_serving_large` 无法覆盖某个 mechanism，不能直接归因模型失败，必须写清输入覆盖缺口。

### 主线二：用户手动 profiling 大矩阵，Codex 只做 simulation / replay / prediction

目标：把大规模 profiling 的人工调度和后处理建模拆开。用户负责手动完成大型真实 profiling 矩阵；Codex 不再启动这些
profile run，而是接收 manifests 和 config mapping，只做 simulation、replay、prediction、validation 摘要和必要模型修复。

#### 用户侧输入

每个矩阵单元需要提供：

- profile manifest；
- workload 描述：input id、请求数、phase 或 serving 参数、seed、错误数；
- config id：HiCache page size、write policy、prefetch policy、capacity、timeout、ratio；
- target config 文件或等价 config snapshot；
- 是否作为 base facts、target oracle 或同配置 replay；
- 已知运行异常，例如 server restart、request error、missing trace shard。

#### Codex 侧工作

Codex 对每个矩阵单元执行：

```text
profile_quality
cache_state replay
cross-config prediction
validation summary extraction
docs/validation/hicache_state_validation.md 更新
必要时修复模型并重跑 simulation / prediction
```

Codex 不在主线二中负责真实 profiling 调度，也不假设 `data/` 目录长期存在。用户提供的 manifest 被视为临时输入；
长期证据仍必须抽取成 HCSV 摘要。

#### 矩阵建议

第一批手动矩阵优先覆盖：

- `page128 <-> page64`；
- `write_through <-> write_back`；
- `timeout <-> best_effort <-> wait_complete`；
- normal capacity <-> low capacity；
- phased workload <-> bench serving workload；
- low concurrency <-> higher concurrency / lock overlap。

矩阵完成后，再决定哪些组合进入自动化回归 fixture 或较小规模 smoke validation。

### 主线三：构建更强 oracle，推进 timeline exact

当前 snapshot timeline 是 multiset coverage，不是严格录像式 transition log。要验证真正 exact，需要新增 ordered transition oracle：

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

## 数据清理约束

本文已经抽取了当前验证需要长期保留的指标。清理生成物时允许删除：

```bash
rm -rf data/profile_runs data/modeling_runs data/tmp data/traces
```

清理前必须确认：

- 本文已写入 run label、配置、输入、关键计数和结论；
- 没有未归档的 final mismatch、profile quality 异常或模型修复原因；
- 需要提交的源码、配置、测试和文档变更仍在工作树中；
- 清理命令只删除生成数据，不删除 configs、docs、scripts、src、tests。

清理后如果需要重新验证，按本文复现入口重新生成 run，并把新 run label 和摘要替换到本文。
