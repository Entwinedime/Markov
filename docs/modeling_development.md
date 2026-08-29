# Modeling 开发与使用

本文件只描述当前可执行的建模流程。开发阶段的取舍、删除记录和数值回归见
`docs/tmp/hicache_modeling_further_slimming_plan_20260827.md` 与配套日志。

## 1. 范围

项目有一条框架无关的 DAG 主链：

```text
profile manifest -> source DAG -> simulation
```

SGLang 在这条主链上增加 HiCache Direct 预测：

```text
profile manifest
  -> source DAG
  -> target HiCache effect plan
  -> Direct I/O/control cost plan
  -> atomic DAG patch
  -> simulation
```

KTransformers 是同级目标框架，但没有 HiCache 模块，因此只执行第一条主链。不能为它伪造 HiCache effect、cost 或评分。

当前正式预测组件是 `hicache_direct`。`gap`、`prefill` 和 `decode` 有独立 ownership，但仍是 deferred；它们不进入
Direct 误差，也不能用来修正 Direct cost。

## 2. 六个阶段

| 阶段 | 输入 | 输出 | 唯一职责 |
| --- | --- | --- | --- |
| profile | framework、server、workload、channel | manifest + traces + formal window | 记录一次真实执行 |
| DAG build | manifest | normalized source DAG | 合并 trace 并构图 |
| effect planning | source facts + target config + calibration planning rates | target effect plan | 推导 I/O/control 结构 |
| cost | effect bytes/pages + calibration/base | operation cost plan | 估计 Direct duration |
| patch | source DAG + effect/cost plan | target DAG | 一次原子结构和 duration 变换 |
| simulation | materialized DAG | component timing | 计算预测时间 |

依赖始终单向。effect planner 不读 target duration；cost model 不决定 carrier；patcher 不估计系数；simulator 不回填模型。

## 3. 核心概念

### 3.1 Source DAG

Source DAG 来自一个真实 profile cell。它保留 source 的 CPU/GPU/NPU 执行、依赖关系和当前尚未建模的 phase context。
cross prediction 不是复制 target DAG，而是在 source DAG 上应用 target effect/cost plan。

### 3.2 Target effect plan

effect plan 由 source 中与目标配置无关的事实和 target HiCache 配置共同推导。它描述：

- effect 类型：prefetch、loadback、device-to-host commit、host-to-storage commit、capacity/control；
- operation/page/byte 数量；
- resource scope/lane；
- consumer 与 logical order。

target profile 中观测到的 I/O 结构只用于 score-only 比较，不参与预测结构生成。
异步 I/O readiness 只读取共享 calibration 派生的 `io_planning` 速率；这些速率不乘 selected-base cost scale。

### 3.3 Direct cost plan

cost plan 为 effect plan 中需要落地的 I/O/control operation 提供微秒 duration。当前输入边界是：

- 共享 physical/runtime calibration；
- 选定 base 的三个 workload observation；
- effect 的 direction、bytes、pages、operation count 和资源语义。

禁止输入 target E2E、target score duration、config/workload/cell ID。当前实现的公式、计时边界、参数估计表和可辨识性决定见
`docs/hicache_io_cost_model.md`。

### 3.4 Oracle-cost replay

oracle-cost replay 是 Debug 诊断，不是预测模型。它保持 source DAG 和预测 target I/O 结构不变，只把同一 effect 的 cost
替换为 target-observed Direct cost，用于回答两个问题：

1. 结构、operation shape 和 patch 是否正确；
2. model-cost 与 oracle-cost 的差异经过同一 DAG 后造成多少隔离 E2E 误差。

它不使用 target E2E，也不获得泛化成绩。

## 4. 容器和构建

所有建模动作通过 modeling image 执行：

```bash
docker compose -f docker/compose/inference.yml build modeling
```

构建 TraceGraph：

```bash
scripts/run.sh modeling -- bash -lc '
  cmake -S src/modeling/trace_graph -B build/modeling/trace_graph-release -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DTRACE_GRAPH_DEBUG=OFF &&
  cmake --build build/modeling/trace_graph-release --target trace_graph -j2
'
```

需要 structure/oracle 诊断时构建 validation binary：

```bash
scripts/run.sh modeling -- bash -lc '
  cmake -S src/modeling/trace_graph -B build/modeling/trace_graph-validation -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DTRACE_GRAPH_DEBUG=ON &&
  cmake --build build/modeling/trace_graph-validation --target trace_graph -j2
'
```

Release 只链接 DAG、HiCache business module、patch 和 simulator。validation 额外链接 compact model summary 与 oracle replay。

## 5. 公共入口

建模只有一个公共 shell 入口：

```bash
scripts/model.sh --help
```

它包含五个动作：

- `build-dag`：从任意受支持 framework 的 profile manifest 构图并仿真；
- `calibrate-hicache`：采集 physical 或 runtime DMA calibration；
- `build-hicache-model`：用共享 calibration 和一个 base 的三个 workload 估计 compact cost 系数；
- `predict-hicache`：只用 source manifest、显式 target 配置和 I/O model 执行正式预测；
- `evaluate-hicache`：可选地读取 target profile，对正式 predictor 的结果评分。

### 5.1 Framework-neutral DAG

```bash
scripts/model.sh build-dag \
  --profile-manifest <profile_manifest.json> \
  --output-dir <dag-output>
```

该入口使用 Release backend，只构图和 simulation。`--config <runner_config.json>` 仅用于精确重放 workflow 已生成的 cell。

### 5.2 构建 one-base model

physical calibration 只保存四类 service 曲线、两个 control 标量和 resource lane，不再生成可直接预测的“无 base model”。
已有 compact calibration 与 C5 base observation 可直接构建一套不携带 config identity 的系数：

```bash
scripts/model.sh build-hicache-model \
  --calibration-report data/calibration/hicache_io_qwen3_32b_tp2/calibration_report.json \
  --base-observations data/modeling_inputs/hicache/C5_observations.json \
  --output-dir <c5-model-output>
```

fresh physical capture 使用：

```bash
scripts/model.sh calibrate-hicache physical \
  --output-dir <calibration-output> \
  --model-config <model-config.json> \
  --tensor-parallel-size 2 \
  --storage-dir <temporary-storage-dir> \
  --runtime-dma-report <isolated-runtime-dma-report.json> \
  --concurrent-runtime-dma-report <tp-runtime-dma-report.json> \
  --control-primitives <snapshot-free-control-primitives.json>
```

`control-primitives` 只有 `prefetch_zero_payload_us_per_operation` 和 `load_us_per_page` 两个业务标量；raw profile 的身份元数据、
pressure/completion surface 都不进入最终 calibration report。

### 5.3 正式 Direct prediction

```bash
scripts/model.sh predict-hicache \
  --source-manifest <source/profile_manifest.json> \
  --target-config configs/modeling/hicache_target_example.json \
  --hicache-io-model <one-base-model.json> \
  --output-dir <output-dir> \
  --trace-threads 4 \
  --trace-file-threads 4
```

`target-config` 是 `{name?, hicache}` 对象，只描述 policy/capacity；它不能携带 I/O cost、target operation observation
或 target E2E。正式 prediction 不读取 target profile。多个 source workload 或 target config 可以重复传参。

### 5.4 可选 observed-matrix evaluation

以 C5 为 base 时，target 是另外四个配置，workload 是 W1/W2/W3：

```bash
scripts/model.sh evaluate-hicache \
  --profile-run-dir <5x3-profile-suite> \
  --source-configs C5_writeback_long_gate_timeout \
  --hicache-io-model <c5-one-base-model.json> \
  --output-dir <output-dir> \
  --model-run-jobs 4 \
  --trace-threads 4 \
  --trace-file-threads 4
```

matrix 的维度来自输入数据，不由 predictor 固定。当前 5×3 资产下，一个 base 对其他配置形成 12 个 cross，最终五个 base
形成 60 个 cross；这些数字只属于当前评分实验。开发时用 `--target-configs`、`--inputs` 或 `--max-predictions` 选择关键 cell。
evaluation 在相同 predictor 完成后才读取 target shape，不能回写 I/O model。
多 base 最终评分重复 `--base-io-model <base>=<model.json>`；需要 oracle-cost 时另外提供一个
score-only manifest：

```bash
scripts/model.sh evaluate-hicache \
  --profile-run-dir <5x3-profile-suite> \
  --base-io-model <base-1>=<model-1.json> \
  --base-io-model <base-2>=<model-2.json> \
  --oracle-scores <score-only-manifest.json> \
  --diagnostics full \
  --output-dir <output-dir>
```

manifest 按 base 列出 `base_observations` 和 `target_costs`。它是 evaluator 的数据驱动输入，不是
predictor 合同。

### 5.5 Dry-run 和 diagnostics

```bash
scripts/model.sh predict-hicache <options> --dry-run
scripts/model.sh predict-hicache <options> --diagnostics full
scripts/model.sh evaluate-hicache <options> --diagnostics full
```

默认 diagnostics 为 off。`full` 才保留每 cell Direct ledger、C++ model summary 和成功日志；它不改变 prediction 语义。

## 6. 产物

默认输出：

```text
<output>/
  preflight_summary.json
  workflow_summary.json
  model_runs/<id>/
    runner_config.json
    cpp_model_config.json
    run_summary.json
  artifacts/model_run_plan.json
```

`workflow_summary.json` 是第一阅读入口。默认不保留逐阶段 wall-clock、逐文件 trace timing、完整 proof row 或 model summary。

`--diagnostics full` 额外保留：

```text
artifacts/debug_rows/<model-run-id>.json
model_runs/<id>/model_summary.json
model_runs/<id>/model.log
```

### 6.1 Oracle replay

oracle-cost 不再有独立 runner CLI。它由统一 evaluator 在普通 prediction 和 structure score 完成后执行：

```bash
scripts/model.sh evaluate-hicache \
  <matrix-and-base-model-options> \
  --oracle-scores <score-only-manifest.json> \
  --diagnostics full
```

ledger 数量由 score bundle 决定。base observations 提供当前 selected base 的 source-self Direct 总量，用于计算 target delta；
target bundle 只在 prediction 完成后参与评分。`--max-predictions 1` 可用于开发烟测；不设限时每个
base 必须与自身 score bundle 的全部 cross cell 一一对应。最终紧凑摘要为：

```text
artifacts/oracle_cost_replay/<base>/summary.json
```

catalog、override 和 C++ replay output 都是 worker 临时文件。

## 7. 结果判读

Direct workflow 先看：

- preflight ready；
- model run usable；
- predicted effect/patch structure ready；
- topology valid；
- required cost ready；
- `component=hicache_direct`，其他 component deferred。

oracle summary 再看：

- Direct total WAPE/P90、delta weighted L1 和大变化方向；
- structure binding ready 与 effect/cost-response 数量；
- model replay 与 oracle replay 的隔离误差。

隔离误差的分母是 target Direct `service + control`，不是完整 target E2E。当前 gate 是 WAPE ≤3%、P90 ≤5%。单 cell
只能提供局部证据，不能宣布完整 gate PASS。

## 8. Component ownership

| Component | 当前状态 | 本轮评分 |
| --- | --- | --- |
| `hicache_direct` | implemented | 是 |
| `gap` | deferred | 否 |
| `prefill` | deferred | 否 |
| `decode` | deferred | 否 |
| probe snapshot overhead | 已从默认采集移除 | 否 |

同骨架 replay 的 model/oracle 差分天然排除了 source 固定的 phase/gap；完整 source-to-target E2E 则仍包含这些未建模项，不能与
Direct score 混报。

## 9. KTransformers

KTransformers 的 profile manifest 只启用实际存在的 channel 和 hook，并显式记录 framework。它直接使用共享 `build-dag`：

```bash
scripts/model.sh build-dag \
  --profile-manifest <ktransformers-run>/profile_manifest.json \
  --output-dir <dag-output>
```

预期 `module_results` 不含 HiCache module；source DAG 和 simulation 仍须成功。对 KTransformers 调用 HiCache prediction 会返回明确
capability 错误。其 image、installer、submodule、compose service 和专用 LD_PRELOAD hook 都属于保留能力。

## 10. 代码所有权

| 路径 | 职责 |
| --- | --- |
| `scripts/model.sh` | 唯一 modeling shell 入口 |
| `scripts/internal/markov_internal/modeling_workflow/` | profile selection、plan、execution、summary |
| `modeling_workflow/calibration/` | 物理 service/control calibration 的采集与紧凑投影 |
| `modeling_workflow/io_model_builder.py` | 共享 calibration + 一个 base 的 observation 参数估计 |
| `modeling_workflow/prediction/` | source-only HiCache request 与 compact ledger |
| `modeling_workflow/evaluation/` | observed-target score-only orchestration |
| `modeling_workflow/validations/final_dag/` | score-only target shape comparison |
| `modeling_workflow/validations/hicache/oracle_cost_replay/` | Debug oracle replay |
| `src/modeling/trace_graph/src/core/` | source DAG build |
| `src/modeling/trace_graph/src/modules/hicache/model/` | effect planning |
| `src/modeling/trace_graph/src/modules/hicache/patch/` | cost materialization 与 atomic patch |
| `src/modeling/trace_graph/src/simulation/` | topological simulation |

## 11. 当前结论与后续顺序

工作流瘦身、I/O cost 简化、C1/C3/C5 换 base 外推和五 base 60 cross 已完成。结构 60/60 通过，
source-invariance 15/15 通过；
数值结论为 `MODEL_LIMITATION`，因为 C2/C4 自身三个 workload 没有覆盖之后预测所需的所有
positive-payload service family。

后续顺序是：

1. 定义一个信息完备的 single-base 观测合同，或用新的独立 runtime calibration 覆盖缺失 family；
2. 不改 target-independent 结构 planner，只重新验证 cost identifiability 和 60-cell 数值；
3. 固定 Direct effect/cost 后，分别实现 `GapModel`、`PrefillModel` 和 `DecodeModel`；
4. 最后组合完整 E2E，继续保留 component attribution。
